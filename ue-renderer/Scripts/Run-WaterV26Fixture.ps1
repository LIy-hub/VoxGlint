param(
    [string]$Project = (Join-Path $PSScriptRoot '../MinecraftUEViewer.uproject'),
    [string]$EngineRoot = $(if ($env:UE_ROOT) { $env:UE_ROOT } else { Join-Path $env:ProgramFiles 'Epic Games\UE_5.8' }),
    [Parameter(Mandatory)][string]$Name,
    [Parameter(Mandatory)][string]$Output,
    [ValidateRange(0,7)][int]$DebugView = 1,
    [ValidateRange(0,1)][int]$P1 = 1,
    [ValidateRange(1024,65534)][int]$Port = 39926,
    [int]$OffsetX = 0,
    [int]$OffsetZ = 0,
    [switch]$CheckV25,
    [ValidateSet(26,27,28)][int]$Protocol = 28,
    [switch]$RayTracing,
    [ValidateSet('glass-fall','stacked-water')][string]$Fixture = 'glass-fall'
)
$ErrorActionPreference = 'Stop'
$renderer = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor.exe'
New-Item -ItemType Directory -Path $Output -Force | Out-Null
$out = (New-Item -ItemType Directory -Path (Join-Path $Output $Name)).FullName
$shots = (New-Item -ItemType Directory -Path (Join-Path $out 'shots')).FullName
$profile = (New-Item -ItemType Directory -Path (Join-Path $out 'profile')).FullName
$log = Join-Path $out 'renderer.log'
$quality = Join-Path $out 'quality.txt'
Set-Content -LiteralPath $quality -Value 'High'
$projectPath=(Resolve-Path -LiteralPath $Project).Path
$viewerArgs = '"{0}" -game -RenderOffscreen -unattended -windowed -ResX=1536 -ResY=864 -ForceRes -d3d12 -sm6 -NoSplash -NoLiveCoding -NoSound -NoScreenMessages -NoPauseWhenUnfocused -NoCrashDialog -BridgeQuality=High -BridgeTrace -BridgeQualityTrace -BridgeRegionTrace -BridgeQualityTraceShots="{1}" -BridgeHideDebugPanel -BridgePort={2} -BridgeQualityFile="{3}" -AbsLog="{4}" -UserDir="{5}" -ExecCmds="mc.Water.P1 {6},mc.Water.Debug {7}" -ini:Engine:[/Script/Engine.Engine]:GameViewportClientClassName=/Script/Engine.GameViewportClient' -f $projectPath,$shots,$Port,$quality,$log,$profile,$P1,$DebugView
if (!$RayTracing) {
    $viewerArgs += ' -noraytracing -ini:Engine:[/Script/Engine.RendererSettings]:r.RayTracing=0 -ini:Engine:[/Script/Engine.RendererSettings]:r.PathTracing=0'
}
$fixtureDir = Join-Path $Output 'fixtures'
$driver = Join-Path $PSScriptRoot 'validate_water_v26.py'
if (!(Test-Path -LiteralPath (Join-Path $fixtureDir "$Fixture.bin"))) {
    & python $driver $fixtureDir
    if ($LASTEXITCODE -ne 0) { throw 'Could not generate the fixture binary' }
}
$viewer = Start-Process -FilePath $renderer -ArgumentList $viewerArgs -WindowStyle Hidden -PassThru
try {
    $deadline = [DateTime]::UtcNow.AddMinutes(12)
    do {
        if ($viewer.HasExited -or [DateTime]::UtcNow -gt $deadline) { throw "UE did not listen on $Port. See $log" }
        if (Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort $Port -State Listen -OwningProcess $viewer.Id -ErrorAction SilentlyContinue) { break }
        Start-Sleep -Milliseconds 400
    } while ($true)
    $driverOptions = @($driver, $fixtureDir, '--replay', $Fixture, '--port', $Port, '--log', $log, '--shots', $shots,
        '--offset-x', $OffsetX, '--offset-z', $OffsetZ, '--protocol', $Protocol)
    if ($CheckV25) { $driverOptions += '--check-v25' }
    & python @driverOptions | Tee-Object -FilePath (Join-Path $out 'result.txt')
    if ($LASTEXITCODE -ne 0) { throw "Water replay failed. See $log" }
} finally {
    if (!$viewer.HasExited) { Stop-Process -InputObject $viewer }
}

