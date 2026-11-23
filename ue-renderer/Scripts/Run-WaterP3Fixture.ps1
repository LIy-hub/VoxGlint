param(
    [Parameter(Mandatory)][string]$Output,
    [ValidateSet('High','Ultra')][string]$Quality = 'High',
    [ValidateSet('motion','storm')][string]$Scenario = 'motion',
    [ValidateSet(0,8,9,10)][int]$DebugView = 0,
    [ValidateRange(1024,65534)][int]$Port = 39844,
    [ValidateRange(50,100)][int]$RenderPercent = 75,
    [int]$OffsetX = 0,
    [int]$OffsetZ = 0,
    [ValidateRange(0,60)][int]$PerfSeconds = 0,
    [string]$ExtraCVars = '',
    [string]$EngineRoot = $(if ($env:UE_ROOT) { $env:UE_ROOT } else { Join-Path $env:ProgramFiles 'Epic Games\UE_5.8' }),
    [switch]$RayTracing,
    [switch]$D3DDebug
)
$ErrorActionPreference = 'Stop'
$out = (New-Item -ItemType Directory -Path $Output).FullName
$project = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../MinecraftUEViewer.uproject')).Path
$wire = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../Source/MinecraftUEViewer/Private/BridgeWire.h') -Raw
$version = [regex]::Match($wire, 'constexpr uint32 Version = (\d+);').Groups[1].Value
if (!$version) { throw 'Could not read the current bridge protocol version.' }
$engine = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor.exe'
$cvars = "mc.Water.Debug $DebugView,r.ScreenPercentage $RenderPercent"
if ($ExtraCVars) { $cvars = "$cvars,$ExtraCVars" }
$argsLine = '"{0}" -game -RenderOffscreen -unattended -multiprocess -windowed -ResX=1536 -ResY=864 -ForceRes -d3d12 -sm6 -NoSound -NoSplash -NoLiveCoding -NoCrashDialog -NoScreenMessages -NoPauseWhenUnfocused -BridgeTrace -BridgeQualityTrace -BridgeHideDebugPanel -BridgeWaterP3Trace -BridgeQuality={1} -BridgePort={2} -BridgeWaterP2CaptureDir="{3}" -BridgeQualityTraceCsv="{3}/performance.csv" -AbsLog="{3}/renderer.log" -UserDir="{3}/profile" -ExecCmds="{4}" -ini:Engine:[/Script/Engine.Engine]:GameViewportClientClassName=/Script/Engine.GameViewportClient' -f $project,$Quality,$Port,$out,$cvars
$qualityFile = Join-Path $out 'quality.txt'
Set-Content -LiteralPath $qualityFile -Value $Quality -Encoding utf8
Set-Content -LiteralPath (Join-Path $out 'minecraftue-graphics.txt') -Value @('exposureMode=0', 'localExposure=0') -Encoding utf8
$argsLine += (' -BridgeQualityFile="{0}"' -f $qualityFile)
if (!$RayTracing) { $argsLine += ' -noraytracing -ini:Engine:[/Script/Engine.RendererSettings]:r.RayTracing=0 -ini:Engine:[/Script/Engine.RendererSettings]:r.PathTracing=0' }
if ($D3DDebug) { $argsLine += ' -d3ddebug -rhivalidation' }
if ($PerfSeconds -gt 0) { $argsLine += ' -csvGpuStats -csvCompression=0 -BridgeGpuEventFrames=420' }
$viewer = Start-Process -FilePath $engine -ArgumentList $argsLine -WindowStyle Hidden -PassThru
try {
    $deadline = [DateTime]::UtcNow.AddMinutes(10)
    while (!(Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort $Port -OwningProcess $viewer.Id -State Listen -ErrorAction SilentlyContinue)) {
        if ($viewer.HasExited -or [DateTime]::UtcNow -ge $deadline) { throw "Renderer initialization failed. See $out\renderer.log" }
        Start-Sleep -Milliseconds 400
    }
    & python (Join-Path $PSScriptRoot 'validate_water_p3.py') $out --port $Port --protocol $version --scenario $Scenario `
        --x-offset $OffsetX --z-offset $OffsetZ --perf-seconds $PerfSeconds | Tee-Object -FilePath (Join-Path $out 'results.txt')
    if ($LASTEXITCODE -ne 0) { throw "P3 replay failed. See $out\renderer.log" }
} finally {
    if (!$viewer.HasExited) { Stop-Process -InputObject $viewer }
}
