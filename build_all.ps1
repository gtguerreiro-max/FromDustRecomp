# Rebuilds everything: XenonRecomp, the recompiled modules and FromDust.exe.
# Usage: .\build_all.ps1 [-SkipRecomp] [-NoGraphics]
# -SkipRecomp reuses the generated C++ modules; it still rebuilds changed code.
param([switch]$SkipRecomp, [switch]$NoGraphics)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$py = (Get-Command python -ErrorAction Stop).Source
$env:PATH += ";$env:LOCALAPPDATA\Microsoft\WinGet\Links;${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

function Invoke-VS([string]$dir, [string]$command, [string]$log) {
    cmd /c "call `"$vcvars`" >nul && cd /d `"$dir`" && $command > `"$log`" 2>&1"
    if ($LASTEXITCODE) {
        Select-String -Path $log -Pattern ' error|FAILED' | Select-Object -First 15 | ForEach-Object { $_.Line }
        throw "build failed: $command (see $log)"
    }
}

if (-not $SkipRecomp) {
    Invoke-VS "$root\XenonRecomp" "cmake --build build" "$root\build_xenonrecomp.log"
    $modules = [ordered]@{
        'Default' = '82000000'; 'GalileoKernel' = '88100000'; 'GalileoPathfind_9_9' = '88480000'
        'GalileoSimulatorWater' = '88500000'; 'GalileoSimulatorWater_9_9' = '89180000'
        'GalileoTexture' = '88300000'; 'dustai.x360.f' = '8A000000'
    }
    foreach ($m in $modules.Keys) {
        $r = & "$root\recomp.ps1" -Module $m -LoadAddress $modules[$m] 2>&1 | Select-String 'unrecognized|switch errors'
        "{0,-28} {1}" -f $m, (($r | ForEach-Object { $_.Line.Trim() }) -join ' | ')
    }
}

& $py "$root\tools\gen_import_stubs.py"
if ($LASTEXITCODE) { throw 'import stub generation failed' }
$gpuBuild = if ($NoGraphics) { 'OFF' } else { 'ON' }
Invoke-VS $root "cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release -DFD_XENIA_GPU=$gpuBuild -DPython3_EXECUTABLE=`"$py`"" "$root\configure_runtime.log"
Invoke-VS $root "cmake --build build --target FromDust" "$root\build_runtime.log"
"FromDust.exe built"
