param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateSet(26, 27, 28)][int]$Protocol = 28,
    [string]$EngineRoot = $(if ($env:UE_ROOT) { $env:UE_ROOT } else { Join-Path $env:ProgramFiles 'Epic Games\UE_5.8' }),
    [ValidateRange(1024, 65534)][int]$Port = 39682,
    [ValidateSet('Presets', 'Baseline', 'History100', 'Fixed70History100')][string]$Mode = 'Presets'
)

$ErrorActionPreference = 'Stop'
$editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor.exe'
$project = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\MinecraftUEViewer.uproject')).Path
$python = Get-Command python -CommandType Application | Select-Object -First 1
$ports = if ($Protocol -lt 27) { @($Port, ($Port + 1)) } else { @($Port) }
if (Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort $ports -State Listen -ErrorAction SilentlyContinue) {
    throw "Fixture port is in use ($($ports -join ', '))."
}
if (Test-Path -LiteralPath $OutputDirectory) { throw "Output directory already exists: $OutputDirectory" }
$output = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
$log = Join-Path $output 'renderer.log'
$quality = Join-Path $output 'quality.txt'
$profile = (New-Item -ItemType Directory -Path (Join-Path $output 'renderer-user')).FullName
$arguments = '"{0}" -game -RenderOffscreen -unattended -windowed -ResX=2560 -ResY=1440 -ForceRes -NoSplash -NoLiveCoding -NoSound -NoScreenMessages -NoPauseWhenUnfocused -NoCrashDialog -BridgeTrace -BridgeRegionTrace -BridgeQualityTrace -BridgeHideDebugPanel -BridgePort={1} -BridgeQualityFile="{2}" -AbsLog="{3}" -UserDir="{4}" -LogCmds="LogMinecraftBridgeEnvironment VeryVerbose" -ini:Engine:[/Script/Engine.Engine]:GameViewportClientClassName=/Script/Engine.GameViewportClient' -f $project, $Port, $quality, $log, $profile
if ($Mode -eq 'History100') { $arguments += ' -ExecCmds="r.TSR.History.ScreenPercentage 100"' }
if ($Mode -eq 'Fixed70History100') { $arguments += ' -ExecCmds="r.TSR.History.ScreenPercentage 100,r.ScreenPercentage 70"' }
$viewer = Start-Process -FilePath $editor -ArgumentList $arguments -WindowStyle Hidden -PassThru
try {
    $deadline = [DateTime]::UtcNow.AddMinutes(3)
    while (!(Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort $Port -OwningProcess $viewer.Id -State Listen -ErrorAction SilentlyContinue)) {
        if ($viewer.HasExited -or [DateTime]::UtcNow -ge $deadline) { throw "UE listener failed. See $log" }
        Start-Sleep -Milliseconds 400
    }
    $driver = Join-Path $PSScriptRoot 'validate_v23_renderer.py'
    $fixtureArgs = @($driver, $log, $quality, '--port', $Port, '--protocol', $Protocol)
    if ($Mode -ne 'Presets') { $fixtureArgs += @('--one-preset', 'Baseline') }
    & $python.Source @fixtureArgs | Tee-Object -FilePath (Join-Path $output 'results.txt')
    if ($LASTEXITCODE -ne 0) { throw "Renderer fixture failed. See $log" }
} finally {
    if (!$viewer.HasExited) { Stop-Process -InputObject $viewer }
}
