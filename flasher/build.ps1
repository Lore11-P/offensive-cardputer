param(
    [switch]$Offensive
)

$ErrorActionPreference = "Continue"
$flasherDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $flasherDir
Set-Location -LiteralPath $root

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    throw "Python non trovato. Installa Python 3.9+ da python.org."
}

& python -c "import platformio" 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Installazione PlatformIO..." -ForegroundColor Yellow
    & python -m pip install --upgrade platformio
    if ($LASTEXITCODE -ne 0) {
        throw "Installazione PlatformIO non riuscita."
    }
}

$envName = "cardputer-adv"
if ($Offensive) {
    $envName = "cardputer-adv-offensive"
}

Write-Host "Build firmware ($envName)..." -ForegroundColor Cyan
& python -m platformio run -e $envName
if ($LASTEXITCODE -ne 0) {
    throw "Build firmware non riuscita."
}

Write-Host "Build filesystem (icone + database IR)..." -ForegroundColor Cyan
& python -m platformio run -e $envName -t buildfs
if ($LASTEXITCODE -ne 0) {
    throw "Build filesystem non riuscita."
}

$buildDir = Join-Path $root ".pio\build\$envName"
$binOut = Join-Path $flasherDir "bin"
$webOut = Join-Path $flasherDir "web"
New-Item -ItemType Directory -Force -Path $binOut, $webOut | Out-Null

foreach ($file in @("bootloader.bin", "partitions.bin", "firmware.bin", "littlefs.bin")) {
    $source = Join-Path $buildDir $file
    if (-not (Test-Path -LiteralPath $source)) {
        throw "Artefatto mancante: $source"
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $binOut $file) -Force
    Copy-Item -LiteralPath $source -Destination (Join-Path $webOut $file) -Force
    Write-Host "  $file" -ForegroundColor DarkGray
}

Write-Host ""
if ($Offensive) {
    Write-Host "ATTENZIONE: flasher aggiornato con la build OFFENSIVA." -ForegroundColor Yellow
    Write-Host "Usala solo su hardware e reti propri, in ambiente RF isolato." -ForegroundColor Yellow
    Write-Host "Per ripristinare la build pubblica: .\build.ps1" -ForegroundColor DarkGray
} else {
    Write-Host "Flasher aggiornato: flasher/bin e flasher/web (build pubblica)." -ForegroundColor Green
}
