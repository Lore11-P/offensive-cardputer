# Esegue idf.py con l'ambiente ESP-IDF 5.4 gia' installato.
# Uso: powershell -ExecutionPolicy Bypass -File tools\idf.ps1 build
#      powershell -ExecutionPolicy Bypass -File tools\idf.ps1 -p COM5 flash
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$IdfArgs
)

$ErrorActionPreference = 'Stop'
$idf = if ($env:IDF_PATH) { $env:IDF_PATH } else { 'C:\Users\utente\esp\esp-idf-v5.4' }
$py312 = 'C:\Users\utente\AppData\Roaming\uv\python\cpython-3.12.14-windows-x86_64-none'

if (Test-Path $py312) {
    $env:PATH = "$py312;$env:PATH"
}
$env:ESP_IDF_SHELL = 'powershell.exe'
. "$idf\export.ps1" | Out-Null

Set-Location (Join-Path $PSScriptRoot '..\firmware')
idf.py @IdfArgs
