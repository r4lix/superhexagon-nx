param(
    [string]$Nro = "$PSScriptRoot\..\superhexagon-nx.nro",
    [int]$Seconds = 25,
    [string]$Shot = ""
)
# Run the homebrew in its OWN Eden process, screenshot that window, print the log, stop only that process.
$sw = "C:\Users\klaud\Desktop\switch"
$dir = "$env:APPDATA\eden\sdmc\switch\superhexagon"
if (Get-Process eden -ErrorAction SilentlyContinue) { Write-Output "Eden is already running; not starting another."; exit 2 }
if (-not (Test-Path "$dir\data\image.bin")) {
    New-Item -ItemType Directory -Force "$dir" | Out-Null
    Copy-Item "$PSScriptRoot\..\local\host\data" "$dir\data" -Recurse -Force
}
$proc = Start-Process "$sw\eden\eden.exe" -ArgumentList "`"$((Resolve-Path $Nro).Path)`"" -PassThru
Start-Sleep -Seconds $Seconds
if ($Shot) { & "$PSScriptRoot\shot-window.ps1" -ProcessId $proc.Id -Out $Shot }
if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
