param(
    [string]$Port = "",
    [switch]$Erase
)

$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$bin = Join-Path $here "bin"

$required = @("bootloader.bin", "partitions.bin", "firmware.bin", "littlefs.bin")
foreach ($file in $required) {
    $path = Join-Path $bin $file
    if (-not (Test-Path -LiteralPath $path)) {
        throw "File mancante: $path. Esegui prima build.ps1 dal progetto."
    }
}

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    throw "Python non trovato. Installa Python 3.10+ da python.org e riprova."
}

Write-Host "Verifica esptool..." -ForegroundColor Cyan
& python -c "import esptool" 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Installazione esptool..." -ForegroundColor Yellow
    & python -m pip install --upgrade "esptool>=5,<6"
    if ($LASTEXITCODE -ne 0) {
        throw "Installazione di esptool non riuscita."
    }
}

$esptoolVersion = & python -c "import esptool; print(esptool.__version__)" 2>$null
$esptoolMajor = 4
if ("$esptoolVersion" -match '^\s*(\d+)') {
    $esptoolMajor = [int]$Matches[1]
}
if ($esptoolMajor -ge 5) {
    $eraseCmd = "erase-flash"
    $writeCmd = "write-flash"
    $resetBefore = "default-reset"
    $resetAfter = "hard-reset"
    $flashModeFlag = "--flash-mode"
    $flashFreqFlag = "--flash-freq"
    $flashSizeFlag = "--flash-size"
} else {
    $eraseCmd = "erase_flash"
    $writeCmd = "write_flash"
    $resetBefore = "default_reset"
    $resetAfter = "hard_reset"
    $flashModeFlag = "--flash_mode"
    $flashFreqFlag = "--flash_freq"
    $flashSizeFlag = "--flash_size"
}

$common = @(
    "-m", "esptool",
    "--chip", "esp32s3",
    "--baud", "1500000",
    "--before", $resetBefore,
    "--after", $resetAfter
)
if ($Port -ne "") {
    $common += @("--port", $Port)
}

if ($Erase) {
    Write-Host "Cancellazione completa della flash..." -ForegroundColor Yellow
    & python @common $eraseCmd
    if ($LASTEXITCODE -ne 0) {
        throw "Cancellazione della flash non riuscita."
    }
}

Write-Host "Scrittura firmware CardputerFirmware v1.0..." -ForegroundColor Cyan
& python @common $writeCmd `
    $flashModeFlag dio `
    $flashFreqFlag 80m `
    $flashSizeFlag 8MB `
    0x0 (Join-Path $bin "bootloader.bin") `
    0x8000 (Join-Path $bin "partitions.bin") `
    0x10000 (Join-Path $bin "firmware.bin") `
    0x410000 (Join-Path $bin "littlefs.bin")

if ($LASTEXITCODE -ne 0) {
    throw "Scrittura non riuscita. Controlla la porta USB e riprova."
}

Write-Host ""
Write-Host "Firmware installato." -ForegroundColor Green
Write-Host "Apri un monitor seriale a 115200 baud per vedere il log di avvio."
Write-Host "Se la porta non viene rilevata: tieni premuto G0, reinserisci il cavo e riprova con -Port COMx."
