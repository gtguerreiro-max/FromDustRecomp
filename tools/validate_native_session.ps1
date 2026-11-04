param(
    [ValidateRange(10,1800)][int]$RunSeconds=90,
    [ValidateRange(1,1799)][int]$CaptureAt=60,
    [string]$StartSchedule='10:0010:600',
    [string]$OutputDirectory='',
    [bool]$VSync=$true,
    [ValidateSet(0,30,60,120)][int]$FrameLimit=30,
    [bool]$DebugLayer=$true,
    [ValidateRange(1,10000)][int]$MinimumShaders=1,
    [switch]$RenderDoc,
    [switch]$Visible
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(!$OutputDirectory){$OutputDirectory=Join-Path $root 'build\native-session'}
$output=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force|Out-Null
$variables=@('FD_RENDERER','FD_HEADLESS','FD_HIDE_WINDOW','FD_EXIT_SECONDS','FD_AUTOPRESS',
    'FD_SETTINGS_FILE','FD_SETTINGS_OPEN','FD_SETTINGS_TAB','FD_CAPTURE','FD_CAPTURE_SECONDS',
    'FD_FRAME_CAPTURE','FD_NATIVE_TEXTURE_CAPTURE','FD_NATIVE_FRAME_CAPTURE','FD_NATIVE_GEOMETRY_CAPTURE',
    'FD_NATIVE_GEOMETRY_MIN_INDEXED','FD_NATIVE_GEOMETRY_MIN_PATCHED','FD_NATIVE_GEOMETRY_REPORT',
    'FD_NATIVE_AUDIT_REPORT','FD_NATIVE_SHADER_CATALOG','FD_REPLAY_DEBUG','FD_REPLAY_CAPTURE')
$previous=@{}
foreach($name in $variables){
    $previous[$name]=[Environment]::GetEnvironmentVariable($name,'Process')
    Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
}
try{
    $env:FD_RENDERER='native'
    if($DebugLayer){$env:FD_REPLAY_DEBUG='1'}
    if($RenderDoc){
        if(!$Visible -or !$env:FD_NATIVE_DUMP_AT -or [int]$env:FD_NATIVE_DUMP_AT -le 0 -or [int]$env:FD_NATIVE_DUMP_AT -ge $RunSeconds){
            throw 'RenderDoc requires -Visible and FD_NATIVE_DUMP_AT within the bounded session'
        }
        $env:FD_REPLAY_CAPTURE='1'
    }
    $env:FD_HEADLESS=if($Visible){'0'}else{'1'}
    $env:FD_EXIT_SECONDS=[string]$RunSeconds
    $env:FD_AUTOPRESS=$StartSchedule
    $env:FD_NATIVE_SHADER_CATALOG=Join-Path $root 'build\native-shaders-live\catalog.bin'
    $env:FD_NATIVE_AUDIT_REPORT=Join-Path $output 'native-session.json'
    $env:FD_NATIVE_GEOMETRY_REPORT=Join-Path $output 'geometry.json'
    $env:FD_CAPTURE=Join-Path $output 'scene.png';$env:FD_CAPTURE_SECONDS=[string]$CaptureAt
    $env:FD_SETTINGS_FILE=Join-Path $output 'settings.ini'
    [IO.File]::WriteAllText($env:FD_SETTINGS_FILE,"[Graphics]`nScale=1`nVSync=$([int]$VSync)`nFrameLimit=$FrameLimit`nAnisotropy=-1`n")
    [ordered]@{run_seconds=$RunSeconds;capture_at=$CaptureAt;start_schedule=$StartSchedule;
        vsync=$VSync;frame_limit=$FrameLimit;debug_layer=$DebugLayer;visible=[bool]$Visible;renderdoc=[bool]$RenderDoc;minimum_shaders=$MinimumShaders;
        dump_at=$env:FD_NATIVE_DUMP_AT;render_profile=$env:FD_NATIVE_PROFILE;
        executable_sha256=(Get-FileHash -LiteralPath (Join-Path $root 'build\FromDust.exe') -Algorithm SHA256).Hash
    }|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $output 'session-options.json') -Encoding utf8
    $launchFile=Join-Path $root 'build\FromDust.exe'
    $launchArguments='"'+(Join-Path $root 'game')+'"'
    if($RenderDoc){
        $launchFile=Join-Path $root 'third_party\renderdoc_dl\RenderDoc_1.47_64\renderdoccmd.exe'
        if(!(Test-Path -LiteralPath $launchFile)){throw 'The matching RenderDoc console tool is missing'}
        $launchArguments=@('capture','-w','-c',('"'+(Join-Path $output 'native-live')+'"'),'-d',('"'+$output+'"'),
            ('"'+(Join-Path $root 'build\FromDust.exe')+'"'),('"'+(Join-Path $root 'game')+'"'))
    }
    $elapsed=[Diagnostics.Stopwatch]::StartNew()
    $process=Start-Process -FilePath $launchFile -ArgumentList $launchArguments `
        -WorkingDirectory $output -WindowStyle $(if($Visible -and !$RenderDoc){'Normal'}else{'Hidden'}) -PassThru -RedirectStandardOutput (Join-Path $output 'stdout.log') `
        -RedirectStandardError (Join-Path $output 'stderr.log')
    $process.WaitForExit()
    if($process.ExitCode){throw "Native session failed ($($process.ExitCode)): $output"}
    if($RenderDoc -and $elapsed.Elapsed.TotalSeconds -lt $RunSeconds-3){throw 'RenderDoc target exited before its bounded run completed'}
    $report=Get-Content -LiteralPath $env:FD_NATIVE_AUDIT_REPORT -Raw|ConvertFrom-Json
    if(!$report.native_rendering -or !$report.completed_frames -or $report.unresolved_draw_calls){throw 'Native session did not produce complete frames'}
    if($report.used_shaders.Count -lt $MinimumShaders){throw "Native session reached only $($report.used_shaders.Count) shaders, required $MinimumShaders"}
    if(!(Test-Path -LiteralPath $env:FD_CAPTURE)){throw 'Native session did not produce its screenshot'}
    $geometry=Get-Content -LiteralPath $env:FD_NATIVE_GEOMETRY_REPORT -Raw|ConvertFrom-Json
    if($geometry.failures -or $geometry.errors.Count){throw 'Native session recorded a geometry or renderer failure'}
    if($RenderDoc -and !(Get-ChildItem -LiteralPath $output -Filter 'native-live*.rdc')){throw 'RenderDoc did not produce the requested live GPU capture'}
    Write-Output "Native session: $($report.completed_frames) frames, $($report.cpu_draw_calls) draws, $output"
}
finally{foreach($name in $variables){[Environment]::SetEnvironmentVariable($name,$previous[$name],'Process')}}
