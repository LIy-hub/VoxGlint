param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$EngineRoot = $(if ($env:UE_ROOT) { $env:UE_ROOT } else { Join-Path $env:ProgramFiles 'Epic Games\UE_5.8' }),
    [switch]$ExpectPollution
)
$ErrorActionPreference = 'Stop'
$editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor.exe'
$project = (Resolve-Path (Join-Path $PSScriptRoot '..\MinecraftUEViewer.uproject')).Path
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Output directory exists' }
$output = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
$profile = Join-Path $output 'User Settings'
$config = (New-Item -ItemType Directory -Path (Join-Path $profile 'Saved\Config\WindowsEditor') -Force).FullName
$ini = Join-Path $config 'GameUserSettings.ini'
@'
[ScalabilityGroups]
sg.ResolutionQuality=0
sg.ViewDistanceQuality=2
sg.AntiAliasingQuality=3
sg.ShadowQuality=3
sg.GlobalIlluminationQuality=3
sg.ReflectionQuality=2
sg.PostProcessQuality=2
sg.TextureQuality=2
sg.EffectsQuality=2
sg.FoliageQuality=2
sg.ShadingQuality=2
sg.LandscapeQuality=2
[/Script/Engine.GameUserSettings]
Version=5
FullscreenMode=2
LastConfirmedFullscreenMode=2
bUseVSync=False
bUseDynamicResolution=False
ResolutionSizeX=800
ResolutionSizeY=450
LastUserConfirmedResolutionSizeX=800
LastUserConfirmedResolutionSizeY=450
'@ | Set-Content -Encoding ascii $ini
$quality = Join-Path $output 'quality.txt'
$status = $quality + '.effective'
function Pick([string]$Value, [System.Diagnostics.Process]$Viewer) {
    [IO.File]::WriteAllText($quality, $Value)
    $until = [DateTime]::UtcNow.AddSeconds(30)
    do {
        if ($Viewer.HasExited) { throw "UE quit while waiting for $Value" }
        if (Test-Path -LiteralPath $status) {
            $actual = (Get-Content -LiteralPath $status -Raw).Trim()
            if ($actual.StartsWith("$Value ")) { return $actual }
        }
        Start-Sleep -Milliseconds 100
    } until ([DateTime]::UtcNow -ge $until)
    throw "Unobserved quality: $Value"
}
function Start-Viewer([string]$Tag, [int]$Port) {
    $log = Join-Path $output "$Tag.log"
    $arguments = '"{0}" -game -RenderOffscreen -unattended -windowed -ResX=800 -ResY=450 -ForceRes -NoSplash -NoLiveCoding -NoSound -NoScreenMessages -NoCrashDialog -BridgePort={1} -UserDir="{2}" -BridgeQualityFile="{3}" -AbsLog="{4}"' -f $project, $Port, $profile, $quality, $log
    return Start-Process -FilePath $editor -ArgumentList $arguments -WindowStyle Hidden -PassThru
}
function Quit-Normally([System.Diagnostics.Process]$Viewer, [int]$Port) {
    # Reset v24, Window/singleWindow=1, Close. The production Close handler requests a normal engine exit.
    [byte[]]$frames = @(0,0,0,17,1, 0x4d,0x43,0x55,0x45, 0,0,0,24, 0,0,0,0,0,0,0,1,
                      0,0,0,14,12, 0,0,0,0,0,0,0,0, 3,32,1,194,1,
                      0,0,0,1,13)
    $sock = [Net.Sockets.TcpClient]::new('127.0.0.1', $Port)
    try {
        $sock.GetStream().Write($frames, 0, $frames.Length)
        if (!$Viewer.WaitForExit(30000) -or $Viewer.ExitCode -ne 0) { throw "UE did not exit normally ($($Viewer.ExitCode))" }
    } finally { $sock.Dispose() }
}
function Record([string]$Label, [string]$Value) {
    "$Label`n$Value" | Tee-Object -FilePath (Join-Path $output 'results.txt') -Append | Write-Host
}
[IO.File]::WriteAllText($quality, 'Baseline')
$first = Start-Viewer 'first' 39862
try {
    $baseline = Pick Baseline $first
    Record 'first baseline' $baseline
    if ($baseline -notmatch 'Baseline HWRT=1 MegaLights=1' -or $baseline -notmatch 'sg.ReflectionQuality=2') { throw 'Unexpected baseline' }
    foreach ($preset in 'Performance','Balanced','High','Ultra') {
        Record "selected $preset" (Pick $preset $first)
        $restored = Pick Baseline $first
        Record "after $preset" $restored
        if (!$ExpectPollution -and $restored -cne $baseline) { throw "Baseline changed on returning from $preset" }
    }
    Record 'balanced at normal quit' (Pick Balanced $first)
    Quit-Normally $first 39862
} finally { if (!$first.HasExited) { Stop-Process -InputObject $first } }
Record 'saved groups' ((Get-Content -LiteralPath $ini | Select-String '^sg\.').Line -join "`n")
[IO.File]::WriteAllText($quality, 'Baseline')
$second = Start-Viewer 'second' 39872
try {
    # Wait for this process to write its own status before reading it.
    $until = [DateTime]::UtcNow.AddSeconds(45)
    $secondLog = Join-Path $output 'second.log'
    do {
        if ($second.HasExited -or [DateTime]::UtcNow -ge $until) { throw 'UE restart did not initialize' }
        Start-Sleep -Milliseconds 150
    } until ((Test-Path $secondLog) -and (Select-String -LiteralPath $secondLog 'Minecraft UE bridge protocol 24 initialized'))
    $restarted = Pick Baseline $second
    Record 'baseline after restart' $restarted
    if (($restarted -cne $baseline) -ne [bool]$ExpectPollution) { throw 'Unexpected persistence comparison' }
    Quit-Normally $second 39872
    Record 'result' 'PASS'
} finally { if (!$second.HasExited) { Stop-Process -InputObject $second } }
