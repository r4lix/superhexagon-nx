# Host (Windows, MinGW) build of the port for fast iteration:  powershell -File tools\build-host.ps1 [-Run]
param([switch]$Run, [string[]]$RunArgs)
$root = Split-Path -Parent $PSScriptRoot
$sdl = Join-Path $root "local\host\SDL2-2.30.10\x86_64-w64-mingw32"
$out = Join-Path $root "local\host\superhexagon.exe"
$src = @(Get-ChildItem (Join-Path $root "src") -Filter *.cpp) + @(Get-ChildItem (Join-Path $root "src\gen") -Filter *.cpp) + @(Get-ChildItem (Join-Path $root "src\third_party\imgui") -Filter *.cpp)
$flags = @("-std=gnu++17","-O1","-g","-fpermissive","-w","-I$root\src","-I$root\src\gen","-I$sdl\include\SDL2","-DSDL_MAIN_HANDLED")
$objdir = Join-Path $root "local\host\obj"
New-Item -ItemType Directory -Force $objdir | Out-Null
$objs = @()
$hdrTime = (Get-ChildItem (Join-Path $root "src") -Include *.h -Recurse | Sort-Object LastWriteTime -Descending | Select-Object -First 1).LastWriteTime
foreach ($f in $src) {
    $o = Join-Path $objdir ($f.BaseName + ".o")
    $objs += $o
    if (-not (Test-Path $o) -or (Get-Item $o).LastWriteTime -lt $f.LastWriteTime -or (Get-Item $o).LastWriteTime -lt $hdrTime -or $f.Name -eq "game_gen.cpp") {
        Write-Host "CC $($f.Name)"
        & g++ @flags -c $f.FullName -o $o
        if ($LASTEXITCODE -ne 0) { throw "compile failed: $($f.Name)" }
    }
}
Write-Host "LD $out"
& g++ -o $out @objs "-L$sdl\lib" -lmingw32 -lSDL2 -static-libgcc -static-libstdc++
if ($LASTEXITCODE -ne 0) { throw "link failed" }
Copy-Item "$sdl\bin\SDL2.dll" (Split-Path $out) -Force
if ($Run) { & $out @RunArgs }

