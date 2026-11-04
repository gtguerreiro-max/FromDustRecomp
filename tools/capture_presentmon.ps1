param(
    [ValidateRange(5,120)][int]$Seconds=30,
    [string]$OutputDirectory='',
    [switch]$Elevated
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$exe=Join-Path $root 'build/debug-tools/PresentMon.exe'
if(!(Test-Path -LiteralPath $exe)){throw 'Falta o PresentMon oficial em build/debug-tools/PresentMon.exe'}
$signature=Get-AuthenticodeSignature -LiteralPath $exe
if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Intel Corporation'){
    throw 'A assinatura Intel do PresentMon nao foi validada'
}
if(!$OutputDirectory){$OutputDirectory=Join-Path $root 'build/presentmon'}
$output=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force|Out-Null
$csv=Join-Path $output 'presentmon.csv'
$session='FromDustDebug-'+[DateTime]::Now.ToString('yyyyMMddHHmmss')
$arguments=@('--process_name','FromDust.exe','--timed',[string]$Seconds,
    '--terminate_after_timed','--terminate_on_proc_exit','--no_console_stats',
    '--no_track_input','--session_name',$session,'--output_file',$csv)
if($Elevated){
    # Windows asks the user for administrative consent; do not change group
    # membership or any security setting to obtain trace access.
    $launchArguments=$arguments.Clone();$launchArguments[-1]='"'+$csv+'"'
    $process=Start-Process -FilePath $exe -ArgumentList $launchArguments -Verb RunAs -WindowStyle Hidden -PassThru
    $process.WaitForExit()
    if($process.ExitCode){throw "A captura PresentMon falhou: $($process.ExitCode)"}
}else{
    & $exe @arguments
    if($LASTEXITCODE){throw 'A captura ETW requer privilegios de administrador do Windows'}
}
if(!(Test-Path -LiteralPath $csv)){throw 'O PresentMon nao produziu a captura'}
Write-Output "Captura PresentMon concluida: $csv"
