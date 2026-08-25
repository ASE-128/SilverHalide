$ErrorActionPreference = 'Stop'
$root = Join-Path $PSScriptRoot ''
$qtBin = Join-Path $root 'qt\6.8.3\msvc2022_64\bin'
$env:PATH = $qtBin + ';' + $env:PATH
$env:QT_PLUGIN_PATH = Join-Path $root 'build_gui\platforms'
$exe = Join-Path $root 'build_gui\SilverHalide.exe'

$p = Start-Process -FilePath $exe -WorkingDirectory (Join-Path $root 'build_gui') -PassThru

# Wait up to 25s for a main window handle to appear.
$deadline = (Get-Date).AddSeconds(25)
while ((Get-Date) -lt $deadline) {
    $p.Refresh()
    if ($p.MainWindowHandle -ne 0) { break }
    Start-Sleep -Milliseconds 250
}
$p.Refresh()
Write-Output ("SMOKE_HANDLE=" + $p.MainWindowHandle)

if ($p.MainWindowHandle -eq 0) {
    $p.Kill() | Out-Null
    Write-Output 'SMOKE_RESULT=NO_WINDOW'
    exit 2
}

$closed = $p.CloseMainWindow()
Write-Output ("SMOKE_CLOSED=" + $closed)

if (-not $p.WaitForExit(30000)) {
    $p.Kill() | Out-Null
    Write-Output 'SMOKE_RESULT=TIMEOUT'
    exit 3
}
Write-Output ("SMOKE_EXIT=" + $p.ExitCode)
exit 0
