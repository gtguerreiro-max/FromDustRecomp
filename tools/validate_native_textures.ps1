param(
    [ValidateRange(20,1800)][int]$RunSeconds = 120,
    [ValidateRange(1,1799)][int]$CaptureAt = 75,
    [string]$StartSchedule = '10:0010:600',
    [string]$OutputDirectory = '',
    [switch]$NativeFrame,
    [string]$Catalog = '',
    [switch]$Screenshot
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if($CaptureAt -ge $RunSeconds-10){throw 'Leave at least ten seconds after capture for GPU completion'}
if(!$OutputDirectory){$OutputDirectory=Join-Path $root 'build\native-texture-validation'}
$output=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force | Out-Null
$exe=Join-Path $root 'build\FromDust.exe'
if(!(Test-Path -LiteralPath $exe)){throw 'Build FromDust with FD_NATIVE_RENDERER=ON first'}
$variables=@('FD_RENDERER','FD_HEADLESS','FD_EXIT_SECONDS','FD_FRAME_CAPTURE','FD_NATIVE_TEXTURE_CAPTURE',
    'FD_AUTOPRESS','FD_SETTINGS_FILE','FD_SETTINGS_OPEN','FD_SETTINGS_TAB','FD_CAPTURE','FD_CAPTURE_SECONDS',
    'FD_NATIVE_SHADER_CATALOG','FD_NATIVE_GEOMETRY_CAPTURE','FD_NATIVE_GEOMETRY_REPORT','FD_NATIVE_AUDIT_REPORT',
    'FD_NATIVE_FRAME_CAPTURE','FD_NATIVE_GEOMETRY_MIN_INDEXED','FD_NATIVE_GEOMETRY_MIN_PATCHED',
    'FD_PROFILE','FD_PROFILE_DELAY','FD_PROFILE_THREAD','FD_FUNC_COUNTS')
$previous=@{}
foreach($name in $variables){
    $previous[$name]=[Environment]::GetEnvironmentVariable($name,'Process')
    Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
}
try{
    $settings=Join-Path $output 'settings.ini'
    [IO.File]::WriteAllText($settings,"[Graphics]`nScale=1`nVSync=0`nFrameLimit=30`nAnisotropy=-1`n")
    $env:FD_RENDERER='d3d12';$env:FD_HEADLESS='1'
    $env:FD_EXIT_SECONDS=[string]$RunSeconds;$env:FD_FRAME_CAPTURE=[string]$CaptureAt
    $env:FD_NATIVE_TEXTURE_CAPTURE='1';$env:FD_AUTOPRESS=$StartSchedule;$env:FD_SETTINGS_FILE=$settings
    if($NativeFrame){
        if(!$Catalog){$Catalog=Join-Path $root 'build\native-shaders-live\catalog.bin'}
        $env:FD_NATIVE_SHADER_CATALOG=(Resolve-Path -LiteralPath $Catalog).Path
        $env:FD_NATIVE_FRAME_CAPTURE='1'
        $env:FD_NATIVE_AUDIT_REPORT=Join-Path $output 'live-shader-audit.json'
        $env:FD_NATIVE_GEOMETRY_REPORT=Join-Path $output 'live-geometry.json'
    }
    $started=Get-Date
    $process=Start-Process -FilePath $exe -ArgumentList ('"'+(Join-Path $root 'game')+'"') -WorkingDirectory $output -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $output 'stdout.log') -RedirectStandardError (Join-Path $output 'stderr.log')
    $process.WaitForExit()
    if($process.ExitCode){throw "Native texture capture runtime failed ($($process.ExitCode)); see $output"}
    foreach($file in @('native-texture-report.json','native-textures.rgba','native-cubes.rgba','native-mips.rgba',
        'xenia_regs.bin','gpu_textures.bin','gpu_mips.bin')){
        $path=Join-Path $output $file
        if(!(Test-Path -LiteralPath $path) -or (Get-Item -LiteralPath $path).LastWriteTime -lt $started){throw "Missing or stale native texture result: $path"}
    }
    & python (Join-Path $PSScriptRoot 'validate_native_texture_capture.py') $output --json (Join-Path $output 'validation-textures.json')
    if($LASTEXITCODE){throw 'Native memory texture reference comparison failed'}
    if($Screenshot -or $NativeFrame){
        $reference=Join-Path $output 'frame_reference.bin'
        if(!(Test-Path -LiteralPath $reference) -or (Get-Item -LiteralPath $reference).LastWriteTime -lt $started){throw 'Missing synchronized frame reference'}
        & python (Join-Path $PSScriptRoot 'frame_reference.py') $reference (Join-Path $output 'scene.png')
        if($LASTEXITCODE){throw 'Cannot decode synchronized frame reference'}
    }
    if($NativeFrame){
        $path=Join-Path $output 'native-frame-report.json'
        if(!(Test-Path -LiteralPath $path) -or (Get-Item -LiteralPath $path).LastWriteTime -lt $started){throw 'Missing or stale native live frame report'}
        $frame=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
        if(!$frame.complete -or !$frame.passed -or !$frame.cpu_frame_serial -or $frame.cpu_frame_serial -ne $frame.gpu_frame_serial -or $frame.debug_errors){throw 'Native live frame execution failed'}
        $image=Join-Path $output 'native-live-frame\frame.png'
        if(!(Test-Path -LiteralPath $image) -or (Get-Item -LiteralPath $image).LastWriteTime -lt $started){throw 'Missing or stale native live frame image'}
        & python (Join-Path $PSScriptRoot 'img_compare.py') $image (Join-Path $output 'scene.png') --strict-size --require-opaque `
            --min-psnr 45 --max-outlier-percent 0.1 --json (Join-Path $output 'validation-native-frame.json')
        if($LASTEXITCODE){throw 'Native live frame differs from the synchronized reference'}
    }
    Write-Output 'Native texture acquisition passed on the GPU consuming thread. Interactive rendering still uses Xenia.'
}
finally{foreach($name in $variables){[Environment]::SetEnvironmentVariable($name,$previous[$name],'Process')}}
