param([switch]$NoOpen)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build\fd_replay\fd_replay.exe'
$catalog = Join-Path $root 'build\native-shaders\catalog.bin'
$names = @('gameplay-bright-complete', 'gameplay-breath-complete', 'base-submission-mips', 'title-base-complete')
$labels = @('Cenario iluminado', 'Cenario e sopro', 'Cenario inicial', 'Titulo')
$output = Join-Path $root 'build\teste-render-nativo'
foreach ($path in @($exe, $catalog)) {
    if (!(Test-Path -LiteralPath $path)) { throw "Falta o ficheiro: $path" }
}
$directories = @($names | ForEach-Object { Join-Path $root "build\${_}-replay" })
foreach ($index in 0..($names.Count - 1)) {
    foreach ($path in @((Join-Path $directories[$index] 'replay.bin'), (Join-Path $root "build\rdoc\$($names[$index])\reference.png"))) {
        if (!(Test-Path -LiteralPath $path)) { throw "Falta o ficheiro de teste: $path" }
    }
}
New-Item -ItemType Directory -Path $output -Force | Out-Null
Write-Host 'A renderizar quatro frames guardados com o motor nativo...'
Write-Host 'Este teste compara imagens. O jogo interativo ainda usa Xenia.'
$started = Get-Date
$previousDebug = $env:FD_REPLAY_DEBUG
try {
    $env:FD_REPLAY_DEBUG = '1'
    $arguments = @('--shader-catalog', ('"' + $catalog + '"'))
    $arguments += @($directories | ForEach-Object { '"' + $_ + '"' })
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $output 'render.log') -RedirectStandardError (Join-Path $output 'device.log')
    $process.WaitForExit()
    if ($process.ExitCode) { throw "O render falhou ($($process.ExitCode)). Consulta $output\render.log e device.log" }
}
finally { $env:FD_REPLAY_DEBUG = $previousDebug }
$frames = @(foreach ($index in 0..($names.Count - 1)) {
    $png = Join-Path $directories[$index] 'frame.png'
    $reportPath = Join-Path $directories[$index] 'native-report.json'
    foreach ($path in @($png, $reportPath)) {
        if (!(Test-Path -LiteralPath $path) -or (Get-Item -LiteralPath $path).LastWriteTime -lt $started) { throw "Resultado em falta ou antigo: $path" }
    }
    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    if (!$report.shader_catalog -or !$report.debug_active -or !$report.full_frame -or !$report.game_draws -or $report.debug_errors) { throw "A validacao do dispositivo falhou: $reportPath" }
    $reference = Join-Path $root "build\rdoc\$($names[$index])\reference.png"
    & python (Join-Path $PSScriptRoot 'validate_effect_regions.py') $png $reference `
        (Join-Path $directories[$index] 'replay-index.json') --json (Join-Path $directories[$index] 'validation-effects.json') | Out-Host
    if ($LASTEXITCODE) { throw "A comparacao de efeitos falhou: $($names[$index])" }
    [ordered]@{
        label = $labels[$index]
        native = 'data:image/png;base64,' + [Convert]::ToBase64String([IO.File]::ReadAllBytes($png))
        reference = 'data:image/png;base64,' + [Convert]::ToBase64String([IO.File]::ReadAllBytes($reference))
    }
})
$json = ConvertTo-Json -InputObject $frames -Compress
$html = @'
<!doctype html><html lang="pt"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>From Dust — teste do render nativo</title>
<style>body{margin:24px;background:#131820;color:#e9edf4;font:16px system-ui}h1{font-size:24px}p{max-width:850px;line-height:1.5;color:#bdc9d9}select,button{font:inherit;padding:9px;margin:0 8px 16px 0;background:#273244;color:white;border:1px solid #637087;border-radius:6px}.images{display:grid;grid-template-columns:1fr 1fr;gap:16px}figure{margin:0}figcaption{margin-bottom:8px}img{width:100%;display:block;background:black}small{color:#bdc9d9}@media(max-width:800px){.images{grid-template-columns:1fr}}</style>
<h1>Teste do render nativo</h1>
<p>Quatro frames guardados, renderizados agora no PC. Compara a luz, cores, agua e desfocagem. Esta demonstracao ainda nao permite jogar com o render nativo; o jogo interativo continua a usar Xenia.</p>
<select id="scene" aria-label="Cenario"></select><button id="swap">Trocar os lados</button>
<div class="images"><figure><figcaption id="leftLabel"></figcaption><img id="left" alt="Primeira imagem da comparacao"></figure><figure><figcaption id="rightLabel"></figcaption><img id="right" alt="Segunda imagem da comparacao"></figure></div>
<p><small>Referencia: captura do Xenia. Nativo: motor C++/Direct3D 12 com o catalogo de shaders do projeto. Ha diferencas residuais entre ambos. Os ficheiros originais mantem-se em build.</small></p>
<script>const frames=__FRAMES__;const scene=document.getElementById('scene');let swapped=false;frames.forEach((f,i)=>scene.add(new Option(f.label,i)));function show(){const f=frames[Number(scene.value)];document.getElementById('left').src=swapped?f.reference:f.native;document.getElementById('right').src=swapped?f.native:f.reference;document.getElementById('leftLabel').textContent=swapped?'Referencia (Xenia)':'Render nativo';document.getElementById('rightLabel').textContent=swapped?'Render nativo':'Referencia (Xenia)';}scene.onchange=show;document.getElementById('swap').onclick=()=>{swapped=!swapped;show()};show();</script></html>
'@
$result = Join-Path $output 'comparacao.html'
[IO.File]::WriteAllText($result, $html.Replace('__FRAMES__', $json), [Text.UTF8Encoding]::new($false))
Write-Host "Teste concluido: $result"
if (!$NoOpen) { Start-Process -FilePath $result }
