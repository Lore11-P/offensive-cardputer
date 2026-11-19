<#
.SYNOPSIS
  Converte una copertina album in cover.cvr (64x64, RGB565 little-endian) per Cardputer.

.DESCRIPTION
  Ritaglia al centro, ridimensiona a 64x64 senza distorcere e salva cover.cvr
  nella cartella dell'album sulla microSD (default E:\data\Music\<Album>).
  Richiede Windows PowerShell 5.1 e System.Drawing (inclusi in Windows).

.PARAMETER Image
  Immagine sorgente: jpg, jpeg, png, bmp o gif.

.PARAMETER Album
  Nome della cartella album sotto data\Music. Se manca, viene chiesto.

.PARAMETER OutDir
  Cartella base alternativa. Default: E:\data\Music.

.PARAMETER Force
  Sovrascrive un cover.cvr esistente senza chiedere conferma.

.EXAMPLE
  .\cover_to_cardputer.ps1 -Image "C:\cover.jpg" -Album "The Dark Side of the Moon"

.EXAMPLE
  .\cover_to_cardputer.ps1 -Image ".\cover.png" -Album "Prova" -OutDir "C:\temp\music" -Force
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Image,

    [string]$Album = '',

    [string]$OutDir,

    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$CoverSize = 64
$CoverBytes = $CoverSize * $CoverSize * 2
$CoverName = 'cover.cvr'
$DefaultOutDir = 'E:\data\Music'
$ValidExtensions = @('.jpg', '.jpeg', '.png', '.bmp', '.gif')

$exitCode = 0
$source = $null
$sourceImage = $null
$bmp = $null

try {
    # --- immagine sorgente ---
    $file = Get-Item -LiteralPath $Image -ErrorAction SilentlyContinue
    if ($null -eq $file -or $file.PSIsContainer) {
        throw "Immagine non trovata: $Image"
    }
    $source = $file.FullName
    $ext = [System.IO.Path]::GetExtension($source).ToLowerInvariant()
    if ($ValidExtensions -notcontains $ext) {
        throw "Formato non supportato ($ext). Usa jpg, png, bmp o gif."
    }

    # --- album ---
    $Album = $Album.Trim()
    if ([string]::IsNullOrWhiteSpace($Album)) {
        $Album = (Read-Host 'Nome album (cartella sotto data\Music)').Trim()
    }
    if ([string]::IsNullOrWhiteSpace($Album)) {
        throw 'Nome album mancante.'
    }
    $invalidChars = [System.IO.Path]::GetInvalidFileNameChars()
    if ($Album.IndexOfAny($invalidChars) -ge 0 -or $Album.Contains('/') -or $Album.Contains('\')) {
        throw "Nome album non valido: '$Album'. Non usare \ / : * ? `" < > |."
    }

    # --- destinazione ---
    if ([string]::IsNullOrWhiteSpace($OutDir)) {
        if (-not (Test-Path -LiteralPath 'E:\')) {
            throw "microSD non montata: l'unita E: non esiste. Inserisci la SD oppure indica una cartella con -OutDir."
        }
        $OutDir = $DefaultOutDir
    }
    $OutDir = [System.IO.Path]::GetFullPath($OutDir)
    $root = [System.IO.Path]::GetPathRoot($OutDir)
    if ([string]::IsNullOrEmpty($root) -or -not (Test-Path -LiteralPath $root)) {
        throw "Percorso non disponibile: $OutDir. Controlla che l'unita sia montata o usa -OutDir."
    }

    $albumDir = Join-Path $OutDir $Album
    $dest = Join-Path $albumDir $CoverName

    if ((Test-Path -LiteralPath $dest) -and -not $Force) {
        $answer = (Read-Host "$dest esiste gia. Sovrascrivere? [s/N]").Trim()
        if ($answer -notmatch '^(?i)(s|si|y|yes)$') {
            Write-Host 'Operazione annullata: cover.cvr esistente lasciato intatto.' -ForegroundColor Yellow
            return
        }
    }

    if (-not (Test-Path -LiteralPath $albumDir)) {
        New-Item -ItemType Directory -Path $albumDir -Force | Out-Null
    }

    # --- caricamento e ritaglio centrale ---
    try {
        $sourceImage = [System.Drawing.Image]::FromFile($source)
    } catch {
        throw "Impossibile leggere l'immagine '$source': $($_.Exception.Message)"
    }
    if ($sourceImage.Width -lt $CoverSize -or $sourceImage.Height -lt $CoverSize) {
        throw "Immagine troppo piccola: $($sourceImage.Width)x$($sourceImage.Height) px (minimo 64x64)."
    }

    $side = [Math]::Min($sourceImage.Width, $sourceImage.Height)
    $srcX = [int][Math]::Floor(($sourceImage.Width - $side) / 2)
    $srcY = [int][Math]::Floor(($sourceImage.Height - $side) / 2)

    $bmp = [System.Drawing.Bitmap]::new($CoverSize, $CoverSize, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bmp)
    try {
        # Sfondo bianco: le trasparenze (png/gif) vengono composte su bianco.
        $graphics.Clear([System.Drawing.Color]::White)
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $destRect = [System.Drawing.Rectangle]::new(0, 0, $CoverSize, $CoverSize)
        $srcRect = [System.Drawing.Rectangle]::new($srcX, $srcY, $side, $side)
        $graphics.DrawImage($sourceImage, $destRect, $srcRect, [System.Drawing.GraphicsUnit]::Pixel)
    } finally {
        $graphics.Dispose()
    }

    # --- conversione RGB565 little-endian ---
    $rect = [System.Drawing.Rectangle]::new(0, 0, $CoverSize, $CoverSize)
    $locked = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    try {
        $stride = $locked.Stride
        $scan = [byte[]]::new([Math]::Abs($stride) * $CoverSize)
        [System.Runtime.InteropServices.Marshal]::Copy($locked.Scan0, $scan, 0, $scan.Length)
    } finally {
        $bmp.UnlockBits($locked)
    }

    $out = [byte[]]::new($CoverBytes)
    for ($y = 0; $y -lt $CoverSize; $y++) {
        $row = if ($stride -gt 0) { $y * $stride } else { ($CoverSize - 1 - $y) * (-$stride) }
        for ($x = 0; $x -lt $CoverSize; $x++) {
            $i = $row + ($x * 3)   # memoria BGR
            $b = [int]$scan[$i]
            $g = [int]$scan[$i + 1]
            $r = [int]$scan[$i + 2]
            $rgb565 = (($r -shr 3) -shl 11) -bor (($g -shr 2) -shl 5) -bor ($b -shr 3)
            $o = ($y * $CoverSize + $x) * 2
            $out[$o] = [byte]($rgb565 -band 0xFF)          # byte basso
            $out[$o + 1] = [byte](($rgb565 -shr 8) -band 0xFF) # byte alto
        }
    }

    [System.IO.File]::WriteAllBytes($dest, $out)
    $written = [System.IO.File]::ReadAllBytes($dest)
    if ($written.Length -ne $CoverBytes) {
        throw "Scrittura anomala: $dest misura $($written.Length) byte invece di $CoverBytes."
    }

    $firstWord = [int]$written[0] -bor ([int]$written[1] -shl 8)
    $r8 = [int]((($firstWord -shr 11) -band 0x1F) * 255 / 31)
    $g8 = [int]((($firstWord -shr 5) -band 0x3F) * 255 / 63)
    $b8 = [int](($firstWord -band 0x1F) * 255 / 31)

    Write-Host ''
    Write-Host 'cover.cvr creato' -ForegroundColor Green
    Write-Host ("  Sorgente    : {0} ({1}x{2})" -f $source, $sourceImage.Width, $sourceImage.Height)
    Write-Host ("  Album       : {0}" -f $Album)
    Write-Host ("  Destinazione: {0}" -f $dest)
    Write-Host ("  Dimensione  : {0} byte (atteso {1})" -f $written.Length, $CoverBytes)
    Write-Host ("  Primo pixel : 0x{0:X4} = #{1:X2}{2:X2}{3:X2} (byte LE {4:X2} {5:X2})" -f $firstWord, $r8, $g8, $b8, $written[0], $written[1])
    if (([int]$sourceImage.Flags -band [int][System.Drawing.Imaging.ImageFlags]::HasAlpha) -ne 0) {
        Write-Host '  Trasparenza : composta su sfondo bianco.' -ForegroundColor DarkGray
    }
} catch {
    Write-Host ("ERRORE: {0}" -f $_.Exception.Message) -ForegroundColor Red
    $exitCode = 1
} finally {
    if ($null -ne $bmp) { $bmp.Dispose() }
    if ($null -ne $sourceImage) { $sourceImage.Dispose() }
}

exit $exitCode
