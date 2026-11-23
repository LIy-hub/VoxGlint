param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateRange(1024, 65534)][int]$Port = 40688,
    [string]$EngineRoot = $(if ($env:UE_ROOT) { $env:UE_ROOT } else { Join-Path $env:ProgramFiles 'Epic Games\UE_5.8' }),
    [string]$FabricGolden = '',
    [string]$FabricLightningGolden = '',
    [switch]$D3D12
)

$ErrorActionPreference = 'Stop'
$viewerExe = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor.exe'
$project = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\MinecraftUEViewer.uproject')).Path
$python = Get-Command python -CommandType Application | Select-Object -First 1
if (Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort $Port -State Listen -ErrorAction SilentlyContinue) {
    throw "Weather fixture port $Port is in use."
}
if (Test-Path -LiteralPath $OutputDirectory) { throw "Output directory already exists: $OutputDirectory" }
$output = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
$log = Join-Path $output 'renderer.log'
$profile = (New-Item -ItemType Directory -Path (Join-Path $output 'renderer-user')).FullName
$quality = Join-Path $output 'quality.txt'
$graphics = Join-Path $output 'minecraftue-graphics.txt'
$render = if ($D3D12) { '-RenderOffscreen -d3d12 -sm6 -windowed -ResX=854 -ResY=480 -ForceRes -BridgeWeatherProbeGpu' } else { '-nullrhi' }
$launch = '"{0}" -game {1} -unattended -NoSplash -NoLiveCoding -NoSound -NoCrashDialog -NoPauseWhenUnfocused -BridgeWeatherProbeDir="{2}" -BridgePort={3} -AbsLog="{4}" -UserDir="{5}" -BridgeQualityFile="{6}" -ini:Engine:[/Script/Engine.Engine]:GameViewportClientClassName=/Script/Engine.GameViewportClient' -f $project, $render, $output, $Port, $log, $profile, $quality
$viewer = Start-Process -FilePath $viewerExe -ArgumentList $launch -WindowStyle Hidden -PassThru
try {
    $deadline = [DateTime]::UtcNow.AddMinutes(5)
    while (!(Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort $Port -OwningProcess $viewer.Id -State Listen -ErrorAction SilentlyContinue)) {
        if ($viewer.HasExited -or [DateTime]::UtcNow -ge $deadline) { throw "UE listener failed. See $log" }
        Start-Sleep -Milliseconds 400
    }
    $arguments = @((Join-Path $PSScriptRoot 'validate_weather_data.py'), $output, '--port', $Port, '--graphics-file', $graphics)
    if ($FabricGolden) { $arguments += @('--fabric-golden', $FabricGolden) }
    if ($FabricLightningGolden) { $arguments += @('--fabric-lightning-golden', $FabricLightningGolden) }
    if ($D3D12) { $arguments += '--read-gpu' }
    & $python.Source @arguments | Tee-Object -FilePath (Join-Path $output 'results.txt')
    if ($LASTEXITCODE -ne 0) { throw "Weather data replay failed. See $log and weather-replay.json." }
} finally {
    if (!$viewer.HasExited) { Stop-Process -InputObject $viewer }
}
