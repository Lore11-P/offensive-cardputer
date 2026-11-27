# Build OrbitOS 3.0 + patch deauth IDF 5.4 + (opzionale) flash.
# Uso:
#   powershell -ExecutionPolicy Bypass -File tools\build_offensive.ps1
#   powershell -ExecutionPolicy Bypass -File tools\build_offensive.ps1 -Flash
param(
    [switch]$Flash,
    [string]$Port = "COM5"
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$fw = Join-Path $root 'firmware'
$build = Join-Path $fw 'build'
$py = 'C:\Users\utente\.espressif\python_env\idf5.4_py3.12_env\Scripts\python.exe'
$esptool = 'C:\Users\utente\esp\esp-idf-v5.4\components\esptool_py\esptool\esptool.py'

& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'idf.ps1') `
    -DORBIT_DIAG_SELFTEST=OFF build
if ($LASTEXITCODE -ne 0) { throw "build fallita" }

& $py (Join-Path $PSScriptRoot 'patch_raw_tx.py') `
    (Join-Path $build 'orbitos.elf') (Join-Path $build 'orbitos-patched.elf')
if ($LASTEXITCODE -ne 0) { throw "patch fallita" }

& $py $esptool --chip esp32s3 elf2image --flash_mode dio --flash_freq 80m `
    --flash_size 8MB --elf-sha256-offset 0xb0 --min-rev-full 0 --max-rev-full 99 `
    -o (Join-Path $build 'orbitos-patched.bin') (Join-Path $build 'orbitos-patched.elf')
if ($LASTEXITCODE -ne 0) { throw "elf2image fallito" }

if ($Flash) {
    & $py $esptool --chip esp32s3 --port $Port --baud 460800 `
        --before default_reset --after no_reset write_flash `
        0xe000 (Join-Path $build 'ota_data_initial.bin') `
        0x10000 (Join-Path $build 'orbitos-patched.bin')
    if ($LASTEXITCODE -ne 0) { throw "flash fallito" }
    # Il reset RTS lascia la scheda in download mode: watchdog reset.
    & $py $esptool --chip esp32s3 --port $Port --before no_reset `
        --after watchdog_reset read_mac 2>$null | Out-Null
}

Write-Output "OK: $build\orbitos-patched.bin"
