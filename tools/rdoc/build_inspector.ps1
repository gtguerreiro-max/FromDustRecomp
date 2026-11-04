# Run in an x64 Visual Studio developer shell. All downloaded files stay in build/.
param([switch]$DownloadHeaders)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$sdk = Join-Path $root 'build\renderdoc-sdk-v1.47'
$renderdoc = Join-Path $root 'third_party\renderdoc_dl\RenderDoc_1.47_64\renderdoc.dll'
New-Item -ItemType Directory -Force -Path $sdk | Out-Null
if ($DownloadHeaders) {
    $headers = Invoke-RestMethod 'https://api.github.com/repos/baldurk/renderdoc/contents/renderdoc/api/replay?ref=v1.47'
    foreach ($header in $headers) {
        if ($header.name.EndsWith('.h')) {
            Invoke-WebRequest -Uri $header.download_url -OutFile (Join-Path $sdk $header.name)
        }
    }
}
if (!(Test-Path -LiteralPath (Join-Path $sdk 'renderdoc_replay.h'))) { throw 'Use -DownloadHeaders to fetch the matching official SDK headers.' }
foreach ($tool in @('dumpbin.exe','lib.exe','cl.exe')) {
    if (!(Get-Command $tool -ErrorAction SilentlyContinue)) { throw 'Run in an x64 Visual Studio developer shell.' }
}
$exports = & dumpbin.exe /exports $renderdoc | ForEach-Object {
    if ($_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\w+)') { $Matches[1] }
}
@('LIBRARY renderdoc.dll','EXPORTS') + $exports | Set-Content (Join-Path $sdk 'renderdoc.def')
& lib.exe /nologo /machine:x64 "/def:$sdk\renderdoc.def" "/out:$sdk\renderdoc.lib"
if ($LASTEXITCODE -ne 0) { throw 'Import library build failed' }
& cl.exe /nologo /EHsc /std:c++17 /MD /DRENDERDOC_PLATFORM_WIN32 "/I$sdk" (Join-Path $PSScriptRoot 'replay_inspect.cpp') "/Fo$sdk\inspect.obj" "/Fe$sdk\replay_inspect.exe" (Join-Path $sdk 'renderdoc.lib')
if ($LASTEXITCODE -ne 0) { throw 'Inspector build failed' }
Copy-Item -LiteralPath $renderdoc -Destination (Join-Path $sdk 'renderdoc.dll') -Force
Write-Output (Join-Path $sdk 'replay_inspect.exe')
