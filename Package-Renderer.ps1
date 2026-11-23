param(
    [string]$EngineRoot = $(if ($env:UE_ROOT) { $env:UE_ROOT } else { Join-Path $env:ProgramFiles 'Epic Games\UE_5.8' }),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'dist\minecraftue-renderer'),
    [string]$LunarInputDirectory
)

$ErrorActionPreference = 'Stop'
$uat = Join-Path $EngineRoot 'Engine\Build\BatchFiles\RunUAT.bat'
$project = Join-Path $PSScriptRoot 'ue-renderer\MinecraftUEViewer.uproject'
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
if (-not (Test-Path -LiteralPath $uat -PathType Leaf)) {
    throw "RunUAT.bat not found: $uat"
}
if (Test-Path -LiteralPath $output) {
    throw "Output already exists. Choose another -OutputDirectory: $output"
}

& (Join-Path $PSScriptRoot 'Prepare-RendererAssets.ps1') -EngineRoot $EngineRoot -LunarInputDirectory $LunarInputDirectory

$parent = Split-Path -Parent $output
New-Item -ItemType Directory -Path $parent -Force | Out-Null
$staging = Join-Path $parent ".ue-stage-$([Guid]::NewGuid().ToString('N'))"
& $uat BuildCookRun "-project=$project" '-target=MinecraftUEViewer' '-platform=Win64' '-clientconfig=Shipping' `
    -build -cook -stage -package -pak -iostore -compressed -prereqs -nodebuginfo -nop4 -unattended -utf8output `
    '-UbtArgs=-WaitMutex' "-stagingdirectory=$staging"
if ($LASTEXITCODE -ne 0) {
    throw "Unreal BuildCookRun failed with exit code $LASTEXITCODE. Staging: $staging"
}

$windows = Join-Path $staging 'Windows'
$exe = Join-Path $windows 'MinecraftUEViewer\Binaries\Win64\MinecraftUEViewer-Win64-Shipping.exe'
$containers = Join-Path $windows 'MinecraftUEViewer\Content\Paks'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf) -or
    -not (Get-ChildItem -LiteralPath $containers -Filter '*.utoc' -File)) {
    throw "Shipping executable or cooked IoStore content missing: $windows"
}

[IO.Directory]::Move($windows, $output)
if (-not (Get-ChildItem -LiteralPath $staging -Force | Select-Object -First 1)) {
    [IO.Directory]::Delete($staging)
}
Write-Host "Shipping renderer packaged: $output"
