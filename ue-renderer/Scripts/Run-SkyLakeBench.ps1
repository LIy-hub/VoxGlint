param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateSet(23, 24, 25, 26, 27, 28)][int]$Protocol = 28,
    [string]$EngineRoot = $(if ($env:UE_ROOT) { $env:UE_ROOT } else { Join-Path $env:ProgramFiles 'Epic Games\UE_5.8' }),
    [ValidateRange(1024, 65534)][int]$Port = 39782,
    [int]$ResX = 1920,
    [int]$ResY = 1080,
    [switch]$PresetsOnly,
    [switch]$HighOnly,
    [ValidateRange(0, 1)][int]$WaterP1 = 1,
    [ValidateRange(1, 5)][int]$Rounds = 3,
    [switch]$RasterOnly,
    [switch]$GpuEvents
)

$ErrorActionPreference = 'Stop'
$editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor.exe'
$project = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\MinecraftUEViewer.uproject')).Path
$python = Get-Command python -CommandType Application | Select-Object -First 1
$ports = if ($Protocol -lt 27) { @($Port, ($Port + 1)) } else { @($Port) }
if (Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort $ports -State Listen -ErrorAction SilentlyContinue) {
    throw "Benchmark port is in use ($($ports -join ', '))."
}
if (Test-Path -LiteralPath $OutputDirectory) { throw "Output already exists: $OutputDirectory" }
$output = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
$shots = (New-Item -ItemType Directory -Path (Join-Path $output 'shots')).FullName
$log = Join-Path $output 'renderer.log'
$quality = Join-Path $output 'quality.txt'
$profile = (New-Item -ItemType Directory -Path (Join-Path $output 'renderer-user')).FullName

$arguments = '"{0}" -game -RenderOffscreen -unattended -windowed -ResX={1} -ResY={2} -ForceRes -NoSplash -NoLiveCoding -NoSound -NoScreenMessages -NoPauseWhenUnfocused -NoCrashDialog -BridgeQualityTrace -BridgeQualityTraceShots="{3}" -BridgeHideDebugPanel -BridgePort={4} -BridgeQualityFile="{5}" -AbsLog="{6}" -UserDir="{7}" -ini:Engine:[/Script/Engine.Engine]:GameViewportClientClassName=/Script/Engine.GameViewportClient' -f $project, $ResX, $ResY, $shots, $Port, $quality, $log, $profile
if ($Protocol -ge 26) {
    $arguments += ' -d3d12 -sm6 -ExecCmds="mc.Water.P1 {0}" -BridgeQualityTraceCsv="{1}"' -f $WaterP1, (Join-Path $output 'frame-timings.csv')
}
if ($GpuEvents) {
    if ($Protocol -lt 26 -or !$HighOnly) { throw '-GpuEvents requires protocol 26 or newer and -HighOnly.' }
    $arguments += ' -csvGpuStats -csvCompression=0 -BridgeGpuEventFrames={0}' -f (240 * $Rounds - 30)
}
if ($RasterOnly) {
    $arguments += ' -noraytracing -ini:Engine:[/Script/Engine.RendererSettings]:r.RayTracing=0 -ini:Engine:[/Script/Engine.RendererSettings]:r.PathTracing=0'
}
$viewer = Start-Process -FilePath $editor -ArgumentList $arguments -WindowStyle Hidden -PassThru
try {
    # A cold shader cache has to compile the water material and its mesh-pass permutations.
    $deadline = [DateTime]::UtcNow.AddMinutes(10)
    while (!(Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort $Port -OwningProcess $viewer.Id -State Listen -ErrorAction SilentlyContinue)) {
        if ($viewer.HasExited -or [DateTime]::UtcNow -ge $deadline) { throw "UE listener failed. See $log" }
        Start-Sleep -Milliseconds 400
    }
    if (Select-String -LiteralPath $log -Pattern 'M_McWater\.uasset: Failed to compile Material|Failed to compile Material /Game/Bridge/M_McWater' -Quiet) {
        throw "The water material failed to compile. See $log"
    }
    $driver = Join-Path $PSScriptRoot 'bench_sky_lake.py'
    $benchArgs = @($driver, $log, $quality, $shots, '--port', $Port, '--protocol', $Protocol)
    if ($PresetsOnly) { $benchArgs += '--presets-only' }
    if ($HighOnly) { $benchArgs += @('--high-only', '--rounds', $Rounds) }
    & $python.Source @benchArgs | Tee-Object -FilePath (Join-Path $output 'results.txt')
    if ($LASTEXITCODE -ne 0) { throw "Sky/Lake benchmark failed. See $log" }
} finally {
    if (!$viewer.HasExited) { Stop-Process -InputObject $viewer }
}
