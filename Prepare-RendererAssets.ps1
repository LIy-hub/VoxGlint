param(
    [string]$EngineRoot = $(if ($env:UE_ROOT) { $env:UE_ROOT } else { Join-Path $env:ProgramFiles 'Epic Games\UE_5.8' }),
    [string]$LunarInputDirectory,
    [switch]$SkipEditorBuild
)

$ErrorActionPreference = 'Stop'
$projectDirectory = Join-Path $PSScriptRoot 'ue-renderer'
$project = Join-Path $projectDirectory 'MinecraftUEViewer.uproject'
$content = Join-Path $projectDirectory 'Content\Bridge'
$materialAssets = @('M_EpicCloudLayers', 'MI_EpicCloudLayers', 'M_McSkyDome', 'T_WaterFoamLace', 'T_WaterFoamMicro')
$niagaraAssets = @('NS_McUnderwaterPlankton', 'NS_McWaterBubbles', 'NS_McWaterfallMist', 'NS_McRainCrown', 'NS_McWaterEntrySplash')

$buildMaterials = [bool]$LunarInputDirectory
foreach ($name in $materialAssets) {
    $buildMaterials = $buildMaterials -or !(Test-Path -LiteralPath (Join-Path $content "$name.uasset") -PathType Leaf)
}
$buildNiagara = $false
foreach ($name in $niagaraAssets) {
    $buildNiagara = $buildNiagara -or !(Test-Path -LiteralPath (Join-Path $content "$name.uasset") -PathType Leaf)
}
if (!$buildMaterials -and !$buildNiagara) { return }

if ($LunarInputDirectory) {
    $LunarInputDirectory = (Resolve-Path -LiteralPath $LunarInputDirectory).Path
    foreach ($name in 'T_McMoonAlbedo_4K.png', 'T_McMoonNormal_4K.png') {
        if (!(Test-Path -LiteralPath (Join-Path $LunarInputDirectory $name) -PathType Leaf)) {
            throw "Missing $name in $LunarInputDirectory. Run tools/lunar_moon/prepare_lunar_data.py first."
        }
    }
}

$editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
if (!(Test-Path -LiteralPath $editor -PathType Leaf)) {
    throw "UnrealEditor-Cmd.exe not found. Set UE_ROOT or pass -EngineRoot to locate UE 5.8.4."
}
if (!$SkipEditorBuild) {
    & (Join-Path $EngineRoot 'Engine\Build\BatchFiles\Build.bat') MinecraftUEViewerEditor Win64 Development "-Project=$project" -WaitMutex -NoHotReload -NoHotReloadFromIDE
    if ($LASTEXITCODE -ne 0) { throw "Unreal Editor build failed with exit code $LASTEXITCODE." }
}

$commonArgs = @($project, '-unattended', '-nosplash', '-nosound', '-nop4', '-NullRHI', '-NoLiveCoding',
    '-NoCrashDialog', '-stdout', '-AllowStdOutLogVerbosity')
if ($buildMaterials) {
    $pythonScript = Join-Path $projectDirectory 'Scripts\prepare_renderer_assets.py'
    $originalLunarInput = [Environment]::GetEnvironmentVariable('MCUE_LUNAR_INPUT')
    try {
        [Environment]::SetEnvironmentVariable('MCUE_LUNAR_INPUT', $LunarInputDirectory)
        & $editor @commonArgs '-run=pythonscript' "-script=$pythonScript" '-EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities'
        if ($LASTEXITCODE -ne 0) { throw "Renderer material generation failed with exit code $LASTEXITCODE." }
    } finally {
        [Environment]::SetEnvironmentVariable('MCUE_LUNAR_INPUT', $originalLunarInput)
    }
}
if ($buildNiagara) {
    & $editor @commonArgs '-run=BridgeWaterAssets'
    if ($LASTEXITCODE -ne 0) { throw "Niagara generation failed with exit code $LASTEXITCODE." }
}

foreach ($name in $materialAssets + $niagaraAssets) {
    if (!(Test-Path -LiteralPath (Join-Path $content "$name.uasset") -PathType Leaf)) {
        throw "Renderer asset was not generated: $name"
    }
}
Write-Host 'Renderer assets prepared.'
