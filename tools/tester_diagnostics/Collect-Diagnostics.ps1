$ErrorActionPreference = 'Stop'
$env:PSModulePath = Join-Path $PSHOME 'Modules'
Write-Host 'Preparing the diagnostic report from the latest session...'
$packageRoot = Split-Path -Parent $PSScriptRoot
$sessions = Join-Path $packageRoot 'logs\diagnostics'
if (-not (Test-Path -LiteralPath $sessions)) { throw 'Run Play with Diagnostics.cmd first.' }
$session = Get-ChildItem -LiteralPath $sessions -Directory -Filter 'session-*' |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $session) { throw 'No diagnostic session was found.' }
if ($session.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Diagnostic session must be a regular directory.' }
$reports = Join-Path $packageRoot 'reports'
New-Item -ItemType Directory -Path $reports -Force | Out-Null
$output = Join-Path $reports ('Tester-Report-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff') + '.zip')
Add-Type -AssemblyName System.IO.Compression
$stream = [IO.File]::Open($output, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
$zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $false)
try {
    foreach ($name in @('system.json','run.json','runtime.log','stdout.log','video-settings-before.ini','video-settings-after.ini','BUILD.json')) {
        $source = Join-Path $session.FullName $name
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { continue }
        if ((Get-Item -LiteralPath $source).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Diagnostic files must not be links.' }
        $diagnosticInput = [IO.File]::Open($source, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        $entry = $zip.CreateEntry($name, [IO.Compression.CompressionLevel]::Optimal)
        $target = $entry.Open()
        try { $diagnosticInput.CopyTo($target) } finally { $diagnosticInput.Dispose(); $target.Dispose() }
    }
} finally { $zip.Dispose(); $stream.Dispose() }
Write-Host ('Report ready: ' + $output)
Write-Host 'Attach this ZIP with the steps that caused the problem.'
