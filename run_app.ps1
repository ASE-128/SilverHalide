$ErrorActionPreference = "Continue"
$exe = Join-Path $PSScriptRoot "build_gui\SilverHalide.exe"
$out = Join-Path $PSScriptRoot "app_stdout.log"
$err = Join-Path $PSScriptRoot "app_stderr.log"
Remove-Item $out,$err -ErrorAction SilentlyContinue
Write-Host "=== Launching $exe ==="
try {
    $p = Start-Process -FilePath $exe -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
    if ($p.WaitForExit(8000)) {
        Write-Host ("EXITED with code 0x{0:X8} ({0})" -f $p.ExitCode)
    } else {
        Write-Host "STILL RUNNING after 8s -> window is up, app launched OK"
        Stop-Process -Id $p.Id -Force
        Write-Host "Stopped app for cleanup."
    }
} catch {
    Write-Host ("ERROR: " + $_.Exception.Message)
}
Write-Host "=== STDOUT ==="
if (Test-Path $out) { Get-Content $out }
Write-Host "=== STDERR ==="
if (Test-Path $err) { Get-Content $err }
