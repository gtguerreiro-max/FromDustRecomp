#Requires -RunAsAdministrator
# Optional development component; run only with the user's UAC approval.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$capability = 'Tools.Graphics.DirectX~~~~0.0.1.0'
try {
    $before = Get-WindowsCapability -Online -Name $capability
    $restart = $false
    if ($before.State -ne 'Installed') {
        & "$env:SystemRoot\System32\dism.exe" /Online /Add-Capability "/CapabilityName:$capability" /NoRestart "/LogPath:$root\build\graphics-tools-install.log" > (Join-Path $root 'build\graphics-tools-progress.log') 2>&1
        $result = $LASTEXITCODE
        if ($result -notin @(0, 3010)) { throw "DISM failed ($result); see graphics-tools-install.log" }
        $restart = $result -eq 3010
    }
    $after = Get-WindowsCapability -Online -Name $capability
    [pscustomobject]@{ State = [string]$after.State; RestartNeeded = $restart } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'build\graphics-tools-result.json') -Encoding ascii
    if ($after.State -ne 'Installed') { throw 'Graphics Tools is not installed' }
}
catch {
    $_ | Out-String | Set-Content -LiteralPath (Join-Path $root 'build\graphics-tools-error.txt')
    exit 1
}
