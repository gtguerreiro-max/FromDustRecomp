# Regenerates analysis + C++ for one module.
# Usage: .\recomp.ps1 [-Module Default] [-LoadAddress 82000000]
param(
    [string]$Module = "Default",
    [string]$LoadAddress = "82000000"
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$py = "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe"
$cfg = "$root\config"

& $py "$root\tools\find_switches.py" "$root\dump\$Module.bin" $LoadAddress "$cfg\${Module}_switch_tables.toml"
if ($LASTEXITCODE) { exit $LASTEXITCODE }

# Base config + generated frameless-function bounds (stays inside [main]).
Get-Content "$cfg\$Module.base.toml", "$cfg\${Module}_switch_tables_functions.toml" |
    Set-Content "$cfg\$Module.toml" -Encoding utf8

New-Item -ItemType Directory -Force "$root\ppc\$Module" | Out-Null
Remove-Item "$root\ppc\$Module\*" -Force -ErrorAction SilentlyContinue
& "$root\XenonRecomp\build\XenonRecomp\XenonRecomp.exe" "$cfg\$Module.toml" "$root\XenonRecomp\XenonUtils\ppc_context.h" *> "$root\ppc\${Module}_recomp.log"

# Every module defines its own _xstart, register save/restore helpers and
# mapping table; suffix them so all modules can link into one executable.
if ($Module -ne "Default") {
    $tag = $Module -replace '[^A-Za-z0-9]', '_'
    $pattern = '(_xstart|__(?:save|rest)(?:gprlr|fpr|vmx)_\d+|PPCFuncMappings)(?=\W)'
    foreach ($file in Get-ChildItem "$root\ppc\$Module" -Include *.cpp, *.h -Recurse) {
        $text = [IO.File]::ReadAllText($file.FullName)
        $new = [regex]::Replace($text, $pattern, "`$1_$tag")
        if ($new -ne $text) { [IO.File]::WriteAllText($file.FullName, $new) }
    }
}

$log = Get-Content "$root\ppc\${Module}_recomp.log"
"unrecognized instructions: $(($log | Select-String '^Unrecognized').Count)"
"switch errors:             $(($log | Select-String 'Switch case').Count)"
