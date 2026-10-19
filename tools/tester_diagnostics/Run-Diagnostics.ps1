$ErrorActionPreference = 'Stop'
# A PowerShell 7 parent can pass incompatible module paths to Windows
# PowerShell 5. Use this host's inbox modules for the diagnostic process only.
$env:PSModulePath = Join-Path $PSHOME 'Modules'
Write-Host 'Doritos Crash Course - lightweight diagnostics'
Write-Host 'Startup progress appears here; the game opens in a separate window.'
$packageRoot = Split-Path -Parent $PSScriptRoot
$sessionId = 'session-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')
$session = Join-Path $packageRoot "logs\diagnostics\$sessionId"
$exe = Join-Path $packageRoot 'bin\DoritosNative.exe'
$run = [ordered]@{
    SessionId = $sessionId
    StartedUtc = [DateTime]::UtcNow.ToString('o')
    FrameReportInterval = 300
    HardwarePollingDuringGame = $false
    Screenshots = $false
    AudioCapture = $false
    VerboseTracing = $false
    AutomaticUploads = $false
}
$stdout = $null
$stderr = $null
$process = $null
$gameExit = 1
$video = Join-Path $packageRoot 'saves\video-settings.ini'
try {
    Write-Host '[1/3] Checking extracted files and preparing logs...'
    New-Item -ItemType Directory -Path $session -Force | Out-Null
    foreach ($required in @('bin\DoritosNative.exe','game\default.xex','game\data.pak','BUILD.json')) {
        if (-not (Test-Path -LiteralPath (Join-Path $packageRoot $required) -PathType Leaf)) {
            throw ('Missing ' + $required + '. Extract the entire ZIP, open the extracted folder in File Explorer, then double-click Play with Diagnostics.cmd.')
        }
    }
    $run.ExecutableSHA256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    $run | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $session 'run.json') -Encoding UTF8
    Copy-Item -LiteralPath (Join-Path $packageRoot 'BUILD.json') -Destination (Join-Path $session 'BUILD.json')
    if (Test-Path -LiteralPath $video) { Copy-Item -LiteralPath $video -Destination (Join-Path $session 'video-settings-before.ini') }

    Write-Host '[2/3] Collecting hardware details once. This may take a few seconds.'
    $hardware = [ordered]@{ CollectedUtc = [DateTime]::UtcNow.ToString('o') }
    try {
        Write-Host '  Reading Windows version...'
        $osInfo = Get-CimInstance Win32_OperatingSystem -OperationTimeoutSec 10
        $hardware.OS = [ordered]@{ Name = $osInfo.Caption; Version = $osInfo.Version; Build = $osInfo.BuildNumber; Architecture = $osInfo.OSArchitecture }
        Write-Host '  Reading CPU details...'
        $hardware.CPU = @(Get-CimInstance Win32_Processor -OperationTimeoutSec 10 | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors)
        Write-Host '  Reading memory capacity...'
        $hardware.TotalMemoryBytes = (Get-CimInstance Win32_ComputerSystem -OperationTimeoutSec 10).TotalPhysicalMemory
        Write-Host '  Reading graphics driver details...'
        $hardware.GPU = @(Get-CimInstance Win32_VideoController -OperationTimeoutSec 10 | Select-Object Name,DriverVersion,DriverDate)
    } catch {
        $hardware.CollectionError = $_.Exception.Message
        Write-Host '  Some hardware details were unavailable. Continuing with the game.'
    }
    $hardware | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $session 'system.json') -Encoding UTF8

    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $exe
    $info.WorkingDirectory = $packageRoot
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.Arguments = '--game-dir "' + (Join-Path $packageRoot 'game') + '" --shader-dir "' +
        (Join-Path $packageRoot 'assets\renderer') + '" --asset-override-dir "' + (Join-Path $packageRoot 'assets\override') + '"'
    # Limit diagnostics to the existing bounded frame report. Inherited developer
    # tracing/capture variables must not accidentally enable expensive work.
    foreach ($name in @($info.EnvironmentVariables.Keys)) {
        if ($name.StartsWith('DORITOS_', [StringComparison]::OrdinalIgnoreCase)) { $info.EnvironmentVariables.Remove($name) }
    }
    $info.EnvironmentVariables['DORITOS_FPS_REPORT'] = '1'
    $stdout = [IO.File]::Open((Join-Path $session 'stdout.log'), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    $stderr = [IO.File]::Open((Join-Path $session 'runtime.log'), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    Write-Host '[3/3] Starting the game in its own window...'
    if (-not $process.Start()) { throw 'Game process did not start.' }
    $run.ProcessId = $process.Id
    $run | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $session 'run.json') -Encoding UTF8
    $outCopy = $process.StandardOutput.BaseStream.CopyToAsync($stdout)
    $errCopy = $process.StandardError.BaseStream.CopyToAsync($stderr)
    Write-Host ('Game started. Process ID: ' + $process.Id)
    Write-Host ('Runtime log: ' + (Join-Path $session 'runtime.log'))
    Write-Host 'Keep this window open while playing. When finished, close the game normally.'
    # Kernel wait and asynchronous pipe copies: no game-time polling loop.
    $process.WaitForExit()
    $outCopy.GetAwaiter().GetResult()
    $errCopy.GetAwaiter().GetResult()
    $gameExit = $process.ExitCode
    $run.ExitCode = $gameExit
    $exitHex = [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$gameExit), 0).ToString('X8')
    Write-Host ('Game closed. Exit code: ' + $gameExit + ' (0x' + $exitHex + ').')
    if ($gameExit -ne 0) { Write-Host 'The game stopped with an error. Collect the diagnostic report and include this exit code.' -ForegroundColor Yellow }
} catch {
    $run.LauncherError = $_.Exception.Message
    Write-Host ('Diagnostic launch error: ' + $_.Exception.Message) -ForegroundColor Red
} finally {
    if ($null -ne $stdout) { $stdout.Dispose() }
    if ($null -ne $stderr) { $stderr.Dispose() }
    if ($null -ne $process) { $process.Dispose() }
    $run.EndedUtc = [DateTime]::UtcNow.ToString('o')
    try {
        $run | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $session 'run.json') -Encoding UTF8
        if (Test-Path -LiteralPath $video) { Copy-Item -LiteralPath $video -Destination (Join-Path $session 'video-settings-after.ini') }
    } catch { Write-Host ('Could not save diagnostic details: ' + $_.Exception.Message) -ForegroundColor Red }
}
Write-Host ('Diagnostic folder: ' + $session)
Write-Host 'Run Collect Diagnostic Report.cmd to make the attachment.'
exit $gameExit
