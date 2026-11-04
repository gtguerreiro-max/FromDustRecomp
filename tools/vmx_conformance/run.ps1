param([string[]]$Modules=@('GalileoSimulatorWater_9_9'),[string]$Name='test_99')
$ErrorActionPreference='Stop'
$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$out=Join-Path $root 'build\vmx_conformance'
New-Item -ItemType Directory -Path $out -Force|Out-Null
python (Join-Path $PSScriptRoot 'gen.py') (Join-Path $out "$Name.cpp") @($Modules|ForEach-Object{Join-Path $root "ppc\$_"})
if($LASTEXITCODE){throw 'generation failed'}
$vcvars='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$inc="/I `"$root\ppc\$($Modules[0])`" /I `"$root\XenonRecomp\thirdparty\simde`" /I `"$PSScriptRoot`""
cmd /c "call `"$vcvars`" >nul 2>&1 && cd /d `"$out`" && clang-cl /nologo /O2 /std:c++20 /EHsc -march=x86-64-v3 -Wno-unused-label -Wno-unused-variable $inc $Name.cpp /Fe:$Name.exe > $Name.build.log 2>&1"
if($LASTEXITCODE){Get-Content (Join-Path $out "$Name.build.log") -Tail 30; throw 'build failed'}
& (Join-Path $out "$Name.exe")
exit $LASTEXITCODE
