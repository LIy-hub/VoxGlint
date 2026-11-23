param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$Project = (Join-Path $PSScriptRoot '..\MinecraftUEViewer.uproject'),
    [string]$EngineRoot = $(if ($env:UE_ROOT) { $env:UE_ROOT } else { Join-Path $env:ProgramFiles 'Epic Games\UE_5.8' }),
    [ValidateRange(1024,65535)][int]$Port = 39957,
    [string[]]$Captures = @()
)

$ErrorActionPreference = 'Stop'
$editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor.exe'
$projectPath = (Resolve-Path -LiteralPath $Project).Path
if (Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort $Port -State Listen -ErrorAction SilentlyContinue) {
    throw "Fixture port $Port is in use."
}
if (Test-Path -LiteralPath $OutputDirectory) { throw "Output directory already exists: $OutputDirectory" }
$output = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
$profile = (New-Item -ItemType Directory -Path (Join-Path $output 'profile')).FullName
$log = Join-Path $output 'renderer.log'
$arguments = '"{0}" -game -RenderOffscreen -unattended -windowed -ResX=854 -ResY=480 -ForceRes -d3d12 -sm6 -noraytracing -NoSplash -NoLiveCoding -NoSound -NoCrashDialog -BridgeTrace -BridgeRegionTrace -BridgePort={1} -AbsLog="{2}" -UserDir="{3}" -LogCmds="LogMinecraftBridgeEnvironment VeryVerbose" -ini:Engine:[/Script/Engine.RendererSettings]:r.RayTracing=0 -ini:Engine:[/Script/Engine.RendererSettings]:r.PathTracing=0 -ini:Engine:[/Script/Engine.Engine]:GameViewportClientClassName=/Script/Engine.GameViewportClient' -f $projectPath, $Port, $log, $profile
$viewer = Start-Process -FilePath $editor -ArgumentList $arguments -WindowStyle Hidden -PassThru
$driverProcess = $null
try {
    $deadline = [DateTime]::UtcNow.AddMinutes(6)
    do {
        $listeners = @(Get-NetTCPConnection -LocalAddress 127.0.0.1 -OwningProcess $viewer.Id -State Listen -ErrorAction SilentlyContinue)
        if ($listeners | Where-Object LocalPort -eq $Port) { break }
        if ($viewer.HasExited -or [DateTime]::UtcNow -gt $deadline) { throw "UE listener failed. See $log" }
        Start-Sleep -Milliseconds 400
    } while ($true)
    $listeners | Select-Object LocalAddress,LocalPort,State,OwningProcess | Export-Csv (Join-Path $output 'listener.csv') -NoTypeInformation
    if ($listeners.Count -ne 1) { throw 'Renderer opened more than one IPv4 loopback listener.' }
    $driver = Join-Path $PSScriptRoot 'validate_v27_shm.py'
    $python = (Get-Command python -CommandType Application | Select-Object -First 1).Source
    $driverArgs = '"{0}" --port {1} --log "{2}"' -f $driver, $Port, $log
    if ($Captures.Count) { $driverArgs += ' --captures ' + (($Captures | ForEach-Object { '"{0}"' -f (Resolve-Path -LiteralPath $_).Path }) -join ' ') }
    $driverProcess = Start-Process -FilePath $python -ArgumentList $driverArgs -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $output 'result.txt') -RedirectStandardError (Join-Path $output 'stderr.txt')
    $observed = $false
    while (!$driverProcess.HasExited) {
        $connections = @(Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort $Port -OwningProcess $viewer.Id -State Established -ErrorAction SilentlyContinue)
        if ($connections.Count -gt 1) { throw 'Renderer accepted more than one bridge connection.' }
        if ($connections.Count -eq 1 -and !$observed) {
            $observed = $true
            $connections | Select-Object LocalAddress,LocalPort,RemoteAddress,RemotePort,State,OwningProcess |
                Export-Csv (Join-Path $output 'connection.csv') -NoTypeInformation
        }
        Start-Sleep -Milliseconds 150
    }
    Get-Content -LiteralPath (Join-Path $output 'result.txt')
    if ($driverProcess.ExitCode -ne 0) { throw "v27 fixture failed. See $output" }
    if (!$observed) { throw 'The fixture did not sample an established connection.' }
} finally {
    if ($driverProcess -and !$driverProcess.HasExited) { Stop-Process -InputObject $driverProcess }
    if (!$viewer.HasExited) { Stop-Process -InputObject $viewer }
}
