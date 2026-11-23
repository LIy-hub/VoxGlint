param(
    [Parameter(Mandatory)][string]$Output,
    [ValidateSet('flat','slope','occlusion','glass-fall','stacked-water','dry','shallow','reflection-marked','reflection-clear','tunnel')][string]$Fixture = 'flat',
    [ValidateSet('ocean','warm','swamp')][string]$Biome = 'ocean',
    [ValidateSet('Performance','Balanced','High','Ultra')][string]$Quality = 'High',
    [ValidateRange(1024,65534)][int]$Port = 39835,
    [ValidateRange(0,6)][int]$DebugView = 0,
    [ValidateRange(0,1)][int]$P2 = 1,
    [ValidateRange(0,5)][int]$PerfRounds = 0,
    [ValidateRange(50,100)][int]$RenderPercent = 75,
    [int]$OffsetX = 0,
    [int]$OffsetZ = 0,
    [string]$Weather = 'NoonClear',
    [string]$CaptureRegex = '.*',
    [string]$EngineRoot = $(if ($env:UE_ROOT) { $env:UE_ROOT } else { Join-Path $env:ProgramFiles 'Epic Games\UE_5.8' }),
    [string]$ExtraCVars = '',
    [switch]$D3DDebug,
    [switch]$FixedExposure,
    [switch]$Probe,
    [switch]$Bubbles,
    [switch]$Motion,
    [switch]$GpuEvents,
    [switch]$RayTracing
)
$ErrorActionPreference = 'Stop'
$out = (New-Item -ItemType Directory -Path $Output).FullName
$project = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../MinecraftUEViewer.uproject')).Path
$engine = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor.exe'
$cvars = "mc.Water.P2 $P2,mc.Water.P2.Debug $DebugView,r.ScreenPercentage $RenderPercent"
if ($ExtraCVars) { $cvars = "$cvars,$ExtraCVars" }
$line = '"{0}" -game -RenderOffscreen -unattended -windowed -ResX=1536 -ResY=864 -ForceRes -d3d12 -sm6 -NoSound -NoSplash -NoLiveCoding -NoCrashDialog -NoScreenMessages -NoPauseWhenUnfocused -BridgeTrace -BridgeQualityTrace -BridgeRegionTrace -BridgeHideDebugPanel -BridgeQuality={1} -BridgePort={2} -BridgeWaterP2CaptureDir="{3}" -BridgeQualityTraceCsv="{3}/performance.csv" -AbsLog="{3}/renderer.log" -UserDir="{3}/profile" -ExecCmds="{4}" -ini:Engine:[/Script/Engine.Engine]:GameViewportClientClassName=/Script/Engine.GameViewportClient' -f $project,$Quality,$Port,$out,$cvars
if ($FixedExposure) {
    $details = Join-Path $out 'minecraftue-graphics.txt'
    Set-Content -LiteralPath $details -Value @('exposureMode=0', 'localExposure=0') -Encoding utf8
    $qualityFile = Join-Path $out 'quality.txt'
    Set-Content -LiteralPath $qualityFile -Value $Quality -Encoding utf8
    $line += ' -BridgeQualityFile="{0}"' -f $qualityFile
}
if ($Probe) { $line += ' -BridgeWaterP2Probe' }
if ($GpuEvents) { $line += (' -csvGpuStats -csvCompression=0 -BridgeGpuEventFrames={0}' -f (240 * $PerfRounds)) }
if ($D3DDebug) { $line += ' -d3ddebug' }
if (!$RayTracing) {
    $line += ' -noraytracing -ini:Engine:[/Script/Engine.RendererSettings]:r.RayTracing=0 -ini:Engine:[/Script/Engine.RendererSettings]:r.PathTracing=0'
}
$viewer = Start-Process -FilePath $engine -ArgumentList $line -WindowStyle Hidden -PassThru
try {
    $deadline = [DateTime]::UtcNow.AddMinutes(10)
    while (!(Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort $Port -OwningProcess $viewer.Id -State Listen -ErrorAction SilentlyContinue)) {
        if ($viewer.HasExited -or [DateTime]::UtcNow -ge $deadline) { throw "Renderer initialization failed. See $out\renderer.log" }
        Start-Sleep -Milliseconds 400
    }
    $fixtureArgs = @($out, '--fixture', $Fixture, '--biome', $Biome, '--port', $Port, '--weather', $Weather,
        '--x-offset', $OffsetX, '--z-offset', $OffsetZ, '--capture-regex', $CaptureRegex, '--perf-rounds', $PerfRounds)
    if ($Bubbles) { $fixtureArgs += '--bubbles' }
    if ($Motion) { $fixtureArgs += '--motion' }
    if ($Fixture -eq 'dry' -and $P2 -eq 1 -and $DebugView -eq 3) { $fixtureArgs += '--verify-dry-depth' }
    & python (Join-Path $PSScriptRoot 'validate_water_p2.py') @fixtureArgs |
        Tee-Object -FilePath (Join-Path $out 'results.txt')
    if ($LASTEXITCODE -ne 0) { throw "Replay failed. See $out\renderer.log" }
} finally {
    if (!$viewer.HasExited) { Stop-Process -InputObject $viewer }
}
