param([Parameter(ValueFromRemainingArguments=$true)][string[]]$IdfArguments)
$ErrorActionPreference = 'Stop'
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    throw 'idf.py is not available. Open an ESP-IDF terminal or run export.ps1 first.'
}
& idf.py @IdfArguments
exit $LASTEXITCODE
