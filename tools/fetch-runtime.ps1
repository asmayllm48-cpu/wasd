# Downloads the pinned scrcpy v5.0.1 Windows release (adb, scrcpy-server, FFmpeg DLLs),
# verifies SHA-256 of the archive and of scrcpy-server, and copies them into runtime/.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$runtime = Join-Path $root 'runtime'
$url = 'https://github.com/Genymobile/scrcpy/releases/download/v5.0.1/scrcpy-win64-v5.0.1.zip'
$zipHash = 'B12A2C4EE8BE317422451FC7DCF8EE20A71B5EA7EF9AD73DDD825A25316227A5'
$serverHash = '764EB6F79811D5211FE9DF341120882BA9994C7A61B897D7BF3FB662E53BC536'
$zip = Join-Path $env:TEMP 'scrcpy-win64-v5.0.1.zip'
if (-not ((Test-Path $zip) -and ((Get-FileHash $zip).Hash -eq $zipHash))) {
    & curl.exe -sSfL --retry 5 -C - -o $zip $url
}
if ((Get-FileHash $zip).Hash -ne $zipHash) { throw 'scrcpy archive hash mismatch' }
$tmp = Join-Path $env:TEMP 'wasd-scrcpy'
Expand-Archive -Force $zip $tmp
New-Item -ItemType Directory -Force $runtime | Out-Null
$src = Join-Path $tmp 'scrcpy-win64-v5.0.1'
foreach ($f in 'adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll', 'scrcpy-server', 'avcodec-63.dll', 'avutil-61.dll', 'LICENSE') {
    Copy-Item -Force (Join-Path $src $f) $runtime
}
if ((Get-FileHash (Join-Path $runtime 'scrcpy-server')).Hash -ne $serverHash) { throw 'scrcpy-server hash mismatch' }
Write-Host 'runtime OK'
