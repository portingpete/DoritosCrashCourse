param([int]$Jobs=6,[string]$Character='cj')
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $projectRoot
function Invoke-Checked([string]$Executable,[string[]]$Arguments) {
    & $Executable @Arguments
    if($LASTEXITCODE -ne 0) { throw "$Executable failed with exit code $LASTEXITCODE" }
}
$pythonExe=(& py -3 -c 'import sys; print(sys.executable)').Trim()
if($LASTEXITCODE -ne 0) { throw 'Python 3.11+ is required for asset conversion' }
Invoke-Checked $pythonExe @('tools/prepare_engine_assets.py','--character',$Character)
Invoke-Checked 'cmake' @('-S','.','-B','build_native/engine','-G','Visual Studio 17 2022','-A','x64','-T','ClangCL','-DDORITOS_ENGINE_ONLY=ON',"-DPython3_EXECUTABLE=$pythonExe")
Invoke-Checked 'cmake' @('--build','build_native/engine','--config','Release','--parallel',"$Jobs")
Invoke-Checked 'ctest' @('--test-dir','build_native/engine','-C','Release','--output-on-failure')
Write-Host 'Independent native previews ready. Use --course-preview to inspect static courses, or --course-play for basic movement. Full gameplay is still being rewritten.'
