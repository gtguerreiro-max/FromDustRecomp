# Run in an x64 Visual Studio developer shell. Requires the existing XenosRecomp build.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$xenos = Join-Path $root 'XenosRecomp'
$build = Join-Path $xenos 'build'
$output = Join-Path $root 'build\predicate-tests'
$source = Join-Path $root 'tests\xenos_predicate_codegen.cpp'
$entry = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt') | Where-Object { $_ -match '^CMAKE_CXX_COMPILER:[^=]+=' }
if (!$entry) { throw 'Configure/build XenosRecomp first' }
$compiler = $entry.Substring($entry.IndexOf('=') + 1)
New-Item -ItemType Directory -Path $output -Force | Out-Null
& cmake --build $build --config Release --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'XenosRecomp build failed' }
$includes = @('XenosRecomp', 'thirdparty\fmt\include', 'thirdparty\dxc-bin\inc',
    'thirdparty\smol-v\source', 'thirdparty\xxHash', 'thirdparty\zstd\lib') | ForEach-Object { '/I' + (Join-Path $xenos $_) }
$generator = Join-Path $output 'generate.exe'
$object = Join-Path $output 'generate.obj'
$arguments = @('/nologo', '/c', '/std:c++20', '/EHsc', '/MD', '/DGENERATE_PREDICATE_TEST') + $includes + @($source, ('/Fo' + $object))
& $compiler @arguments
if ($LASTEXITCODE -ne 0) { throw 'Predicate generator build failed' }
& $compiler /nologo /MD $object (Join-Path $build 'XenosRecomp\CMakeFiles\XenosRecomp.dir\shader_recompiler.cpp.obj') (Join-Path $build 'thirdparty\fmt\fmt.lib') ('/Fe' + $generator)
if ($LASTEXITCODE -ne 0) { throw 'Predicate generator link failed' }
& $generator | Set-Content -LiteralPath (Join-Path $output 'predicate_generated.h') -Encoding ascii
if ($LASTEXITCODE -ne 0) { throw 'Predicate generation failed' }
$runner = Join-Path $output 'test.exe'
& $compiler /nologo /std:c++20 /EHsc /MD ('/I' + $output) $source ('/Fo' + (Join-Path $output 'test.obj')) ('/Fe' + $runner)
if ($LASTEXITCODE -ne 0) { throw 'Generated expression build failed' }
& $runner
if ($LASTEXITCODE -ne 0) { throw 'Generated predicate semantics failed' }
