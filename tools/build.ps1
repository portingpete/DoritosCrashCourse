param(
    [switch]$LoaderOnly,
    [switch]$AllowIncomplete,
    [string]$GeneratorCache = '',
    [string]$SourceShaderCapture = '',
    [string]$CharacterAssets = '',
    [int]$Jobs = 8
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $projectRoot
function Invoke-Checked([string]$Executable, [string[]]$Arguments) {
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Executable failed with exit code $LASTEXITCODE" }
}
# Run the encrypted-XEX gate before dependency downloads or generation.
$pythonExe = (& py -3 -c 'import sys; print(sys.executable)').Trim()
if ($LASTEXITCODE -ne 0) { throw 'Python 3.11+ is required' }
Invoke-Checked $pythonExe @('tools/inspect_xex.py', 'game/default.xex', '--require-supported')
if (-not $LoaderOnly) {
    if (-not $CharacterAssets) {
        $preparedCharacter = & $pythonExe 'tools/character_importer.py' prepare --character cj --pak 'game/data.pak'
        if ($LASTEXITCODE -ne 0) { throw 'Bundled CJ preparation failed; the supported original game/data.pak is required.' }
        $CharacterAssets = ($preparedCharacter | ConvertFrom-Json).character_assets
    }
    if (-not (Test-Path -LiteralPath $CharacterAssets -PathType Leaf)) {
        throw 'Prepared CJ assets are missing. Export the fitted character with tools/blender_character.py and tools/cal3d_character.py before building.'
    }
    # Verify and derive menu assets and the owned character declaration before building.
    # The original package stays untouched; Launch.cmd uses this fixed private root.
    try {
        Invoke-Checked $pythonExe @('tools/prepare_offline_menus.py', '--pak', 'game/data.pak', '--output', 'build_native/offline_menu_override')
        Invoke-Checked $pythonExe @('tools/prepare_native_character.py', '--pak', 'game/data.pak', '--character-assets', $CharacterAssets, '--output', 'build_native/offline_menu_override')
        Invoke-Checked $pythonExe @('tools/prepare_native_materials.py', '--pak', 'game/data.pak', '--character-assets', $CharacterAssets, '--output', 'build_native/offline_menu_override')
        Invoke-Checked $pythonExe @('tools/prepare_native_textures.py', '--pak', 'game/data.pak', '--character-assets', $CharacterAssets, '--output', 'build_native/offline_menu_override')
    } catch {
        throw "Native asset preparation failed. The supported original game/data.pak is required. $($_.Exception.Message)"
    }
}
$setupArguments = @('tools/setup_generator.py')
if ($GeneratorCache) { $setupArguments += @('--source-cache', $GeneratorCache) }
Invoke-Checked $pythonExe $setupArguments
$buildRoot = Join-Path $projectRoot 'build_native'
$bootstrapRoot = Join-Path $buildRoot 'loader'
$commonArguments = @('-G', 'Visual Studio 17 2022', '-A', 'x64', '-T', 'ClangCL', "-DPython3_EXECUTABLE=$pythonExe")
$generatorRoot = Join-Path $projectRoot 'third_party/XenonRecomp'
$generatorBuild = Join-Path $generatorRoot 'build'
Invoke-Checked 'cmake' @('-S', $generatorRoot, '-B', $generatorBuild, '-G', 'Visual Studio 17 2022', '-A', 'x64')
Invoke-Checked 'cmake' @('--build', $generatorBuild, '--config', 'Release', '--target', 'XenonRecomp', 'XenonAnalyse', '--parallel', "$Jobs")
Invoke-Checked 'cmake' (@('-S', $projectRoot, '-B', $bootstrapRoot, '-DDORITOS_LOADER_ONLY=ON') + $commonArguments)
Invoke-Checked 'cmake' @('--build', $bootstrapRoot, '--config', 'Release', '--parallel', "$Jobs")
$inspectExe = Join-Path $bootstrapRoot 'Release/NativeXexInspect.exe'
Invoke-Checked $inspectExe @('game/default.xex', '--output', 'build_native/game_image.bin', '--metadata', 'build_native/game_image.json')
if ($LoaderOnly) {
    Invoke-Checked 'ctest' @('--test-dir', $bootstrapRoot, '-C', 'Release', '--output-on-failure')
    return
}
Invoke-Checked $pythonExe @('tools/prepare_renderer.py')
Invoke-Checked $pythonExe @('tools/renderer_inventory.py')
$shaderCompiler = Join-Path $bootstrapRoot 'Release/NativeShaderCompile.exe'
$shaderJobs = [Math]::Min(8, [Math]::Max(1, $Jobs))
$shaderArguments = @('tools/compile_title_variants.py', '--compiler', $shaderCompiler, '--jobs', "$shaderJobs", '--avatar-pixel')
if (-not $SourceShaderCapture) {
    $preparedSource = Join-Path $buildRoot 'source_shader_inputs/multilayer_vs_8388614_0_75/capture.json'
    if (Test-Path -LiteralPath $preparedSource -PathType Leaf) { $SourceShaderCapture = $preparedSource }
}
if ($SourceShaderCapture) { $shaderArguments += @('--source-capture', $SourceShaderCapture) }
$shaderResult = & $pythonExe @shaderArguments
if ($LASTEXITCODE -ne 0) { throw 'Offline native shader variant compilation failed' }
$shaderReport = ($shaderResult | ConvertFrom-Json).report
if (-not $shaderReport) { throw 'Native shader compiler returned no verification report' }
if ($SourceShaderCapture) {
    Invoke-Checked $pythonExe @('tools/prepare_native_shader_overlay.py', '--pak', 'game/data.pak', '--manifest', $shaderReport, '--output', 'build_native/offline_menu_override')
} elseif (Test-Path -LiteralPath 'build_native/offline_menu_override/data/art/dxshaders/precache_xenon.bsh') {
    throw 'The existing source shader overlay requires its original capture; pass -SourceShaderCapture or restore the prepared source input.'
}
$generatorExe = Join-Path $generatorBuild 'XenonRecomp/Release/XenonRecomp.exe'
$analyserExe = Join-Path $generatorBuild 'XenonAnalyse/Release/XenonAnalyse.exe'
$generationArguments = @('tools/recompile.py', '--output', 'build_native/generated', '--generator', $generatorExe, '--analyser', $analyserExe, '--image', 'build_native/game_image.bin', '--metadata', 'build_native/game_image.json')
if ($AllowIncomplete) { $generationArguments += '--allow-incomplete' }
Invoke-Checked $pythonExe $generationArguments
$nativeArguments = @('-S', $projectRoot, '-B', (Join-Path $buildRoot 'native'), '-DDORITOS_LOADER_ONLY=OFF') + $commonArguments
$nativeArguments += if ($AllowIncomplete) { '-DDORITOS_ALLOW_INCOMPLETE=ON' } else { '-DDORITOS_ALLOW_INCOMPLETE=OFF' }
$nativeArguments += "-DDORITOS_NATIVE_SHADER_MANIFEST=$shaderReport"
Invoke-Checked 'cmake' $nativeArguments
Invoke-Checked 'cmake' @('--build', (Join-Path $buildRoot 'native'), '--config', 'Release', '--parallel', "$Jobs")
Invoke-Checked 'ctest' @('--test-dir', (Join-Path $buildRoot 'native'), '-C', 'Release', '--output-on-failure')
if (-not $SourceShaderCapture) {
    Write-Warning 'Initial build complete. CJ still needs the first-run source shader capture and rebuild described in docs/BUILDING.md.'
}
