param(
    [string]$EngineRoot = $(if ($env:UE_ROOT) { $env:UE_ROOT } else { Join-Path $env:ProgramFiles 'Epic Games\UE_5.8' }),
    [switch]$Build,
    [switch]$Offline,
    [string]$MinecraftArgs,
    [switch]$BridgeTrace,
    [switch]$BridgeTerrainLighting,
    [switch]$BridgeHardwareRayTracing,
    [switch]$BridgeNoHardwareRayTracing,
    [switch]$BridgeGradientNormals,
    [switch]$BridgeReleaseShortcut,
    [switch]$BridgeHideDebugPanel,
    [switch]$BridgeCpuHud,
    [ValidateSet('Baseline', 'Performance', 'Balanced', 'High', 'Ultra', '0', '1', '2', '3')][string]$BridgeQuality = 'Baseline',
    [ValidateRange(10, 250)][int]$BridgeHudMaxFps = 165,
    [ValidateSet('Off', 'OnReady', 'OnRequest')][string]$PerformanceCapture = 'Off'
)

$ErrorActionPreference = 'Stop'
$project = Join-Path $PSScriptRoot 'ue-renderer\MinecraftUEViewer.uproject'
$editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor.exe'
if (-not (Test-Path -LiteralPath $editor -PathType Leaf)) {
    throw "找不到 UE 编辑器：$editor"
}
$bridgePort = 39182
$occupiedPorts = @(Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort $bridgePort -State Listen -ErrorAction SilentlyContinue |
    Select-Object -ExpandProperty LocalPort -Unique)
if ($occupiedPorts.Count) {
    throw "桥接端口 $($occupiedPorts -join '、') 已被占用。请先退出占用端口的窗口。"
}
if ($Build) {
    & (Join-Path $EngineRoot 'Engine\Build\BatchFiles\Build.bat') MinecraftUEViewerEditor Win64 Development "-Project=$project" -WaitMutex -NoHotReload -NoHotReloadFromIDE
    if ($LASTEXITCODE -ne 0) { throw 'UE 编译失败。' }
}
& (Join-Path $PSScriptRoot 'Prepare-RendererAssets.ps1') -EngineRoot $EngineRoot -SkipEditorBuild:$Build

foreach ($variable in 'JAVA_HOME', 'GRADLE_USER_HOME') {
    if (-not [Environment]::GetEnvironmentVariable($variable)) {
        $value = [Environment]::GetEnvironmentVariable($variable, 'User')
        if ($value) { [Environment]::SetEnvironmentVariable($variable, $value) }
    }
}

$viewerArgs = '"{0}" -game -windowed -NoSplash -NoLoadingScreen -NoLiveCoding -BridgeWaitForMinecraft' -f $project
$instanceDir = Join-Path $PSScriptRoot 'fabric-bridge\run'
$viewerArgs += ' -BridgeQuality={0} -BridgeQualityFile="{1}" -BridgeAtlasCacheRoot="{2}"' -f $BridgeQuality,
    (Join-Path $instanceDir 'minecraftue-quality.txt'), (Join-Path $instanceDir 'minecraftue-atlas-cache')
if ($BridgeTrace) { $viewerArgs += ' -BridgeTrace' }
if ($BridgeTerrainLighting -or $BridgeHardwareRayTracing) { $viewerArgs += ' -dx12 -sm6' }
if ($BridgeNoHardwareRayTracing) { $viewerArgs += ' -noraytracing' }
if ($BridgeReleaseShortcut) { $viewerArgs += ' -BridgeReleaseShortcut' }
if ($BridgeCpuHud) { $viewerArgs += ' -BridgeCpuHud' }
$instanceSettings = Join-Path $instanceDir 'minecraftue-settings.properties'
$hudLimit = if ($PSBoundParameters.ContainsKey('BridgeHudMaxFps')) { $BridgeHudMaxFps } else { 0 }
$hidePanel = $BridgeHideDebugPanel.IsPresent
if (Test-Path -LiteralPath $instanceSettings -PathType Leaf) {
    foreach ($line in [IO.File]::ReadLines($instanceSettings)) {
        if ($line -match '^\s*hudMaxFps\s*=\s*([0-9]{1,3})\s*$') {
            $limit = [int]$Matches[1]
            if ($limit -ge 10 -and $limit -le 250) { $hudLimit = $limit }
        }
        if ($line -match '^\s*debugPanel\s*=\s*(on|off)\s*$') {
            $hidePanel = $Matches[1] -eq 'off'
        }
    }
}
if ($hidePanel) { $viewerArgs += ' -BridgeHideDebugPanel' }
if ($hudLimit -gt 0) { $viewerArgs += " -BridgeHudMaxFps=$hudLimit" }
if ($PerformanceCapture -ne 'Off') {
    $session = '{0}-{1}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'), ([guid]::NewGuid().ToString('N').Substring(0, 8))
    $profileDir = Join-Path $PSScriptRoot "ue-renderer\Saved\Profiling\BridgePerformance\$session"
    New-Item -ItemType Directory -Path $profileDir -Force | Out-Null
    $request = Join-Path $profileDir 'capture.request'
    if ($PerformanceCapture -eq 'OnReady') { New-Item -ItemType File -Path $request | Out-Null }
    $viewerArgs += ' -BridgePerformanceDir="{0}" -csvGpuStats -csvCompression=0' -f $profileDir
    Write-Host "性能采集目录 $profileDir"
    Write-Host "执行后切回游戏，3 秒后提交采集请求"
    Write-Host "Start-Sleep -Seconds 3; New-Item -ItemType File -Path '$request' -Force"
}
$viewer = Start-Process -FilePath $editor -ArgumentList $viewerArgs -PassThru
try {
    $deadline = [DateTime]::UtcNow.AddMinutes(8)
    while ($true) {
        $listeningPorts = @(Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort $bridgePort -State Listen -OwningProcess $viewer.Id -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty LocalPort -Unique)
        if ($listeningPorts.Count -eq 1) { break }
        if ($viewer.HasExited -or [DateTime]::UtcNow -ge $deadline) { throw 'UE 未能启动桥接端口 39182 的监听。' }
        Start-Sleep -Milliseconds 250
    }
    Push-Location (Join-Path $PSScriptRoot 'fabric-bridge')
    try {
        $gradleArgs = @('-PsingleWindow', '-PexternalRenderer', 'runClient')
        if ($Offline) { $gradleArgs += '--offline' }
        if ($BridgeGradientNormals) { $gradleArgs += '-PgradientNormals' }
        if ($MinecraftArgs) { $gradleArgs += "--args=$MinecraftArgs" }
        & .\gradlew.bat @gradleArgs
        if ($LASTEXITCODE -ne 0) { throw 'Minecraft 启动或运行失败。' }
    } finally {
        Pop-Location
    }
} finally {
    if (-not $viewer.HasExited -and -not $viewer.CloseMainWindow()) {
        Stop-Process -InputObject $viewer -ErrorAction SilentlyContinue
    }
}
