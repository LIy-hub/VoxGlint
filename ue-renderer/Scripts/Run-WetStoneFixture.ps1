param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateRange(1024,65534)][int]$Port = 40716,
    [string]$EngineRoot = $(if ($env:UE_ROOT) { $env:UE_ROOT } else { Join-Path $env:ProgramFiles 'Epic Games\UE_5.8' }),
    [Parameter(Mandatory)][string]$MinecraftJar,
    [ValidateRange(800,3840)][int]$ResX = 1536,
    [ValidateRange(450,2160)][int]$ResY = 864,
    [string]$Eye = '0.5,65.85,-6.6',
    [string]$Look = '0.3,63.8,2.6',
    [double]$Fov = 48,
    [double]$Sun = 0.93,
    [double]$RainSeconds = 8,
    [ValidateRange(0,40)][int]$ExposureBias = 20,
    [switch]$DryOnly,
    [switch]$Footsteps,
    [switch]$EdgeView,
    [switch]$FlatToggle,
    [switch]$MotionOff,
    [ValidateRange(0,8)][int]$PerfBatches = 0,
    [switch]$ClearCoatNormal
)

$ErrorActionPreference = 'Stop'
$project = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\MinecraftUEViewer.uproject')).Path
$editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor.exe'
$jar = (Resolve-Path -LiteralPath $MinecraftJar).Path
$python = Get-Command python -CommandType Application | Select-Object -First 1
if (Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort $Port -State Listen -ErrorAction SilentlyContinue) {
    throw "Weather sample port $Port is already in use."
}
if (Test-Path -LiteralPath $OutputDirectory) { throw "Output path already exists: $OutputDirectory" }
$output = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
$profile = (New-Item -ItemType Directory -Path (Join-Path $output 'renderer-user')).FullName
$quality = Join-Path $output 'quality.txt'
$log = Join-Path $output 'renderer.log'
Set-Content -LiteralPath $quality -Value 'High' -Encoding ascii
Set-Content -LiteralPath (Join-Path $output 'minecraftue-graphics.txt') -Encoding ascii -Value @(
    'exposureMode=0', "exposureBias=$ExposureBias", 'localExposure=0', 'rainWetness=2'
)
$options = '"{0}" -game -RenderOffscreen -unattended -windowed -ResX={1} -ResY={2} -ForceRes -d3d12 -sm6 -NoSound -NoSplash -NoLiveCoding -NoCrashDialog -NoScreenMessages -NoPauseWhenUnfocused -BridgeTrace -BridgeQualityTrace -BridgeHideDebugPanel -BridgeQuality=High -BridgePort={3} -BridgeWaterP2CaptureDir="{4}" -BridgeQualityFile="{5}" -AbsLog="{6}" -UserDir="{7}" -ini:Engine:[/Script/Engine.Engine]:GameViewportClientClassName=/Script/Engine.GameViewportClient' -f $project, $ResX, $ResY, $Port, $output, $quality, $log, $profile
if ($ClearCoatNormal) {
    $options += ' -ini:Engine:[/Script/Engine.RendererSettings]:r.ClearCoatNormal=True'
}
if ($FlatToggle) {
    $options += ' -BridgeQualityTraceCsv="{0}"' -f (Join-Path $output 'frame-timings.csv')
}
if ($MotionOff) {
    $options += ' -ExecCmds="mcue.TerrainPuddles.Motion 0"'
}
$viewer = Start-Process -FilePath $editor -ArgumentList $options -WindowStyle Hidden -PassThru
try {
    $deadline = [DateTime]::UtcNow.AddMinutes(10)
    while (!(Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort $Port -OwningProcess $viewer.Id -State Listen -ErrorAction SilentlyContinue)) {
        if ($viewer.HasExited -or [DateTime]::UtcNow -ge $deadline) { throw "UE listener failed. See $log" }
        Start-Sleep -Milliseconds 400
    }
    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    $argsList = @((Join-Path $PSScriptRoot 'validate_wet_stone.py'), $output, '--port', $Port, '--jar', $jar,
        "--eye=$Eye", "--look=$Look", '--fov', $Fov.ToString($culture), '--sun', $Sun.ToString($culture),
        '--rain-seconds', $RainSeconds.ToString($culture))
    if ($DryOnly) { $argsList += '--dry-only' }
    if ($Footsteps) { $argsList += '--footsteps' }
    if ($EdgeView) { $argsList += '--edge-view' }
    if ($FlatToggle) { $argsList += '--flat-toggle' }
    if ($MotionOff) { $argsList += '--motion-off' }
    if ($PerfBatches -gt 0) { $argsList += @('--perf-batches', $PerfBatches) }
    & $python.Source @argsList | Tee-Object -FilePath (Join-Path $output 'results.txt')
    if ($LASTEXITCODE -ne 0) { throw "Wet stone sample capture failed. See $log" }
} finally {
    if (!$viewer.HasExited) { Stop-Process -InputObject $viewer }
}
