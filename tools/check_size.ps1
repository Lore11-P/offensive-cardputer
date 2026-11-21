<#
.SYNOPSIS
  Compila ORBIT OS e verifica che il firmware resti dentro il budget flash.

.DESCRIPTION
  Compila l'ambiente indicato (o legge firmware.bin con -SkipBuild) e fallisce
  se il binario supera il tetto di budget. Il tetto predefinito e' la baseline
  misurata (2.358.560 B) piu' 192 KiB per i 10 moduli; la dimensione dello slot
  app viene letta da partitions.csv e superarla e' sempre un errore critico.
  Vedi docs/integration/BUDGET.md.

.PARAMETER Environment
  Ambiente PlatformIO. Default: cardputer-adv-offensive.

.PARAMETER MaxBytes
  Tetto del budget in byte. Default: 2555168 (2.358.560 + 192 KiB).

.PARAMETER SkipBuild
  Non compila: legge il firmware.bin gia' presente.

.EXAMPLE
  .\tools\check_size.ps1

.EXAMPLE
  .\tools\check_size.ps1 -SkipBuild -MaxBytes 2560000
#>
[CmdletBinding()]
param(
    [string]$Environment = 'cardputer-adv-offensive',
    [int]$MaxBytes = 2555168,
    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    if (-not $SkipBuild) {
        Write-Host "Build $Environment..."
        & python -m platformio run -e $Environment
        if ($LASTEXITCODE -ne 0) {
            throw "Build fallita (exit $LASTEXITCODE)"
        }
    }

    $bin = Join-Path $root ".pio/build/$Environment/firmware.bin"
    if (-not (Test-Path -LiteralPath $bin)) {
        throw "firmware.bin non trovato: $bin (esegui senza -SkipBuild)"
    }
    $size = (Get-Item -LiteralPath $bin).Length

    # Slot app: quinta colonna (Size) della riga app0 di partitions.csv.
    $partitionCsv = Join-Path $root 'partitions.csv'
    $app0 = Select-String -Path $partitionCsv -Pattern '^\s*app0\s*,' | Select-Object -First 1
    if ($null -eq $app0) {
        throw "Riga app0 non trovata in $partitionCsv"
    }
    $slotText = ($app0.Line -split ',')[4].Trim() -replace '^0x', ''
    $slot = [Convert]::ToInt32($slotText, 16)

    $free = $slot - $size
    $margin = $MaxBytes - $size
    $percent = 100.0 * $size / $slot

    "{0}: {1:N0} B usati su {2:N0} B ({3:N1}% dello slot)" -f $Environment, $size, $slot, $percent
    "Budget: {0:N0} B | margine al tetto: {1:N0} B | riserva minima: 65.536 B" -f $MaxBytes, $margin

    if ($size -gt $slot) {
        Write-Host "ERRORE: firmware oltre lo slot app di $($size - $slot) B. Build non flashabile via OTA." -ForegroundColor Red
        exit 3
    }
    if ($size -gt $MaxBytes) {
        Write-Host "ERRORE: budget superato di $(-$margin) B. Applica il piano di riduzione in docs/integration/BUDGET.md." -ForegroundColor Red
        exit 2
    }

    Write-Host "OK: budget rispettato, $free B liberi nello slot." -ForegroundColor Green
    exit 0
}
finally {
    Pop-Location
}
