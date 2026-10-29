param([int]$Frames = 120, [string[]]$Extra = @())
# regenerate, build, run the host build under gdb, print the crash/log tail
$root = Split-Path -Parent $PSScriptRoot
python -I "$root\tools\recomp.py" | Out-Host
$o = powershell -ExecutionPolicy Bypass -File "$root\tools\build-host.ps1" *>&1 | ForEach-Object { "$_" }
$bad = $o | Where-Object { $_ -match 'error|undefined|failed' }
if ($bad) { $bad | Select-Object -First 25; return }
Push-Location "$root\local\host"
$g = gdb -batch -ex run -ex bt --args .\superhexagon.exe --data data --frames $Frames @Extra *>&1 | ForEach-Object { "$_" }
$g | Where-Object { $_ -notmatch '^\[(New|Thread)|warning: onecore' } | Select-Object -Last 30
Pop-Location
