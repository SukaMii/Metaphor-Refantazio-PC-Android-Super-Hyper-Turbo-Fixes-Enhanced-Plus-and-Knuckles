# Builds MetaphorCPU (DLL/ASI), the launcher and the tests with the Zig C toolchain
# installed in .venv (pip install -r requirements.txt). Output goes to .\build
# -RunTests runs the tests; the ones that need the game are skipped without it: the signature scan
# (-GameExe <path to METAPHOR.exe>, or the METAPHOR_EXE environment variable) and the shader tests (the game's
# shaders in tests\data, see its README)
param([switch]$RunTests, [string[]]$GameExe = @("$env:METAPHOR_EXE" -split ';'))

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$py = Join-Path $root '.venv\Scripts\python.exe'
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null

$common = @('-target', 'x86_64-windows-gnu', '-O2', '-Wall', '-Wextra', '-Wno-unused-parameter',
            '-Wno-cast-function-type', '-fno-sanitize=undefined')
$core = 'config.c', 'log.c', 'sigs.c', 'patch.c', 'timing.c', 'spinlock.c', 'threads.c', 'profiler.c', 'sites.c',
         'frames.c', 'telemetry.c', 'sampler.c', 'ab.c', 'jobsync.c', 'gpuprobe.c', 'capture.c', 'effects.c',
         'meshcensus.c', 'farmesh.c', 'adaptive.c', 'dxbc.c', 'temporal.c', 'motion.c',
         'shadowcull.c',
         'waitprof.c' |
    ForEach-Object { Join-Path $root "src\$_" }

function Zig([string[]]$zigArgs) {
    # zig writes warnings to stderr; keep PowerShell 5.1 from turning them into errors
    $ErrorActionPreference = 'Continue'
    & $py -m ziglang cc @common @zigArgs 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "zig cc failed: $zigArgs" }
}

# meshoptimizer (third_party, MIT) for the simpler far meshes: C++ without exceptions or the C++
# library; meshopt_glue.cpp gives it operator new / delete, so everything links with zig cc
function ZigCxx([string[]]$zigArgs) {
    $ErrorActionPreference = 'Continue'
    & $py -m ziglang c++ -target x86_64-windows-gnu -O2 -fno-exceptions -fno-rtti -nostdinc++ @zigArgs 2>&1 |
        ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "zig c++ failed: $zigArgs" }
}
$mo = Join-Path $root 'third_party\meshoptimizer\src'
$moObj = "$out\obj_meshopt"
New-Item -ItemType Directory -Force $moObj | Out-Null
ZigCxx @('-c', "$mo\simplifier.cpp", '-o', "$moObj\simplifier.o")
ZigCxx @('-c', "$mo\allocator.cpp", '-o', "$moObj\allocator.o")
ZigCxx @('-c', "$root\src\meshopt_glue.cpp", '-o', "$moObj\glue.o")
$meshopt = "$moObj\simplifier.o", "$moObj\allocator.o", "$moObj\glue.o"

# the replacement shaders, compiled to DXBC and embedded (src/shaders_gen.h; Windows' d3dcompiler_47)
& $py "$root\tools\compile_shaders.py" "$root\src"
if ($LASTEXITCODE -ne 0) { throw 'shader compile failed' }

Write-Host 'Building MetaphorCPU.dll'
Zig (@('-shared', '-o', "$out\MetaphorCPU.dll") + $core + $meshopt + @("$root\src\core.c", "$root\src\dllmain.c"))
Copy-Item "$out\MetaphorCPU.dll" "$out\MetaphorCPU.asi" -Force
Copy-Item "$root\config\MetaphorCPU.ini" "$out\MetaphorCPU.ini" -Force
New-Item -ItemType Directory -Force "$out\presets" | Out-Null
Remove-Item "$out\presets\*.ini" -Force # a mirror of presets\: renamed or retired presets must not linger
Copy-Item "$root\presets\*.ini" "$out\presets" -Force

Write-Host 'Building MetaphorCPU_Launcher.exe'
Zig @('-o', "$out\MetaphorCPU_Launcher.exe", "$root\launcher\launcher.c", '-lshell32')

Write-Host 'Building tests'
Zig (@('-o', "$out\scan_test.exe", "$root\tests\scan_test.c", "$root\tests\test_stubs.c") +
     ($core | Where-Object { $_ -match 'config|log|sigs|patch|shadowcull' }))
Zig (@('-o', "$out\hook_test.exe", "$root\tests\hook_test.c", '-DMCPU_TESTS') +
     ($core | Where-Object { $_ -match 'config|log|sigs|patch|shadowcull' }))
Zig (@('-o', "$out\lock_test.exe", "$root\tests\lock_test.c", "$root\tests\test_stubs.c") +
     ($core | Where-Object { $_ -match 'config|log|timing|spinlock|sites|waitprof' }))
Zig @('-o', "$out\host_test.exe", "$root\tests\host_test.c", '-lwinmm')
Zig @('-o', "$out\timer_probe.exe", "$root\tests\timer_probe.c")
Zig @('-o', "$out\frame_test.exe", "$root\tests\frame_test.c", '-ld3d11', '-lwinmm')
Zig @('-o', "$out\ab_test.exe", "$root\tests\ab_test.c", '-lwinmm')
Zig (@('-o', "$out\sem_test.exe", "$root\tests\sem_test.c", "$root\tests\test_stubs.c", '-lwinmm') +
     ($core | Where-Object { $_ -match 'config|log|sites|timing|jobsync|spinlock|waitprof' }))
Zig (@('-o', "$out\preset_test.exe", "$root\tests\preset_test.c", "$root\tests\test_stubs.c") +
     ($core | Where-Object { $_ -match 'config|log' }))
Zig (@('-o', "$out\capture_test.exe", "$root\tests\capture_test.c", "$root\tests\test_stubs.c") +
     ($core | Where-Object { $_ -match 'config|log' }))
Zig (@('-o', "$out\census_test.exe", "$root\tests\census_test.c", "$root\tests\test_stubs.c", '-ld3d11') +
     ($core | Where-Object { $_ -match 'config|log' }))
Zig (@('-o', "$out\fx_test.exe", "$root\tests\fx_test.c", "$root\tests\test_stubs.c", "$root\src\dxbc.c", '-ld3d11') +
     ($core | Where-Object { $_ -match 'config|log' }))
Zig (@('-o', "$out\config_test.exe", "$root\tests\config_test.c", "$root\tests\test_stubs.c") +
     ($core | Where-Object { $_ -match 'config|log' }))
Zig (@('-o', "$out\telemetry_parse_test.exe", "$root\tests\telemetry_parse_test.c", "$root\tests\test_stubs.c") +
     ($core | Where-Object { $_ -match 'config|log|sites|sigs|sampler|timing|waitprof' }))
Zig (@('-o', "$out\farmesh_test.exe", "$root\tests\farmesh_test.c", "$root\tests\test_stubs.c", '-ld3d11') +
     $meshopt + ($core | Where-Object { $_ -match 'config|log|meshcensus' }))
Zig (@('-o', "$out\adaptive_test.exe", "$root\tests\adaptive_test.c", "$root\tests\test_stubs.c") +
     ($core | Where-Object { $_ -match 'config|log' }))
# offline tool: simplification of census meshes
Zig (@('-o', "$out\simplify_eval.exe", "$root\tools\simplify_eval.c") + $meshopt)
# launcher end-to-end test: mock game + a winmm proxy that is only usable after main() starts
New-Item -ItemType Directory -Force "$out\mockgame" | Out-Null
Zig @('-shared', '-o', "$out\mockgame\winmm.dll", "$root\tests\mock_winmm.c")
Zig @('-o', "$out\mockgame\mock_game.exe", "$root\tests\mock_game.c", '-lwinmm')

Get-ChildItem $out -Include *.pdb, *.lib -Recurse | Remove-Item -Force
Write-Host "Done -> $out"

if ($RunTests) {
    $gameShaders = Test-Path "$root\tests\data\ps_5b1af0481406dae1.dxbc"
    $exes = @($GameExe | Where-Object { $_ -and (Test-Path $_) })
    if ($exes.Count) { foreach ($e in $exes) { & "$out\scan_test.exe" $e } }
    else { Write-Host "SKIP scan_test: METAPHOR.exe not found (pass -GameExe <path>)" }
    if (-not $gameShaders) { Write-Host 'SKIP fx_test and the stabilise frame test: the game''s shaders are not in tests\data (see tests\data\README.md)' }
    & "$out\hook_test.exe"
    & "$out\host_test.exe" "$out\MetaphorCPU.dll" "$root\tests\host_test.ini"
    & "$out\host_test.exe" "$out\MetaphorCPU.dll" "$root\tests\host_test_pc.ini" pc
    & "$out\ab_test.exe" "$out\MetaphorCPU.dll" "$root\tests\ab_test.ini"
    & "$out\ab_test.exe" "$out\MetaphorCPU.dll" "$root\tests\ab_test_pc.ini" pc
    & "$out\ab_test.exe" "$out\MetaphorCPU.dll" "$root\tests\ab_test_overall.ini"
    & "$out\ab_test.exe" "$out\MetaphorCPU.dll" "$root\tests\ab_test_features.ini"
    & "$out\frame_test.exe" "$out\MetaphorCPU.dll" "$root\tests\frame_test.ini"
    & "$out\frame_test.exe" "$out\MetaphorCPU.dll" "$root\tests\frame_test_occlusion.ini" occlusion
    & "$out\frame_test.exe" "$out\MetaphorCPU.dll" "$root\tests\frame_test_overall.ini" overall
    & "$out\frame_test.exe" "$out\MetaphorCPU.dll" "$root\tests\frame_test_features.ini" features
    & "$out\frame_test.exe" "$out\MetaphorCPU.dll" "(written by the test)" shaders
    if ($gameShaders) { & "$out\frame_test.exe" "$out\MetaphorCPU.dll" "$root\tests\frame_test_stabilise.ini" stabilise }
    & "$out\frame_test.exe" "$out\MetaphorCPU.dll" "$root\tests\frame_test_compute.ini" compute
    Get-ChildItem "$out\MetaphorCPU_compare folder\MetaphorCPU_capture_*" -Directory -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
    & "$out\frame_test.exe" "$out\MetaphorCPU.dll" "$root\tests\frame_test_capture.ini" capture
    Get-ChildItem "$out\MetaphorCPU_compare folder\MetaphorCPU_compare_*" -Directory -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
    & "$out\frame_test.exe" "$out\MetaphorCPU.dll" "$root\tests\frame_test_compare.ini" capture
    # the mock checks the Android-profile hooks (on native Windows the PC profile leaves the timer alone)
    $env:METAPHORCPU_INI = "$root\tests\host_test.ini"
    & "$out\MetaphorCPU_Launcher.exe" --game "$out\mockgame\mock_game.exe" --dll "$out\MetaphorCPU.dll" --wait
    Remove-Item Env:\METAPHORCPU_INI
    & "$out\capture_test.exe"
    if ($gameShaders) { & "$out\fx_test.exe" "$root\tests\data" }
    Get-ChildItem "$out\MetaphorCPU_meshes_*" -Directory -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
    & "$out\census_test.exe"
    $census = Get-ChildItem "$out\MetaphorCPU_meshes_*" -Directory | Select-Object -Last 1
    $eval = & "$out\simplify_eval.exe" $census.FullName | Out-String
    Write-Host $eval
    # the test grid (flat, 162 triangles): its border stays when locked (22%), it all but vanishes when free
    if ($eval -match '162\s+[\d.]+\s+\|\s+50 m\s+22%\s+1%') { Write-Host 'PASS (0 failures) simplify_eval' }
    else { Write-Host 'FAIL (1 failures) simplify_eval' }
    & "$out\farmesh_test.exe"
    & "$out\adaptive_test.exe"
    & "$out\telemetry_parse_test.exe"
    & "$out\config_test.exe"
    & "$out\lock_test.exe" 2 8 100000
}
