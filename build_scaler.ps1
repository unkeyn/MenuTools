# MenuTools & Fullscreen Scaler Automated Build Script
$ErrorActionPreference = "Stop"

$MinGW = "C:\Users\unkeyn\scoop\apps\mingw-mstorsjo-llvm-msvcrt\current\bin"
$Clang64 = Join-Path $MinGW "x86_64-w64-mingw32-clang++.exe"
$Clang32 = Join-Path $MinGW "i686-w64-mingw32-clang++.exe"
$Windres = Join-Path $MinGW "llvm-windres.exe"
$Strip = Join-Path $MinGW "llvm-strip.exe"

$Root = $PSScriptRoot

Write-Host "== Step 1: Generate Anime4K Shaders ==" -ForegroundColor Cyan
& uv run python (Join-Path $Root "Scaler/generate_anime4k.py")
if ($LASTEXITCODE -ne 0) { throw "generate_anime4k.py failed" }

Write-Host "== Step 2: Compile HLSL Shaders ==" -ForegroundColor Cyan
$compiler = Join-Path $Root "Scaler/compile_shaders.exe"
$shaders = @(
    @{ hlsl = "Nearest_CS.hlsl"; out = "Nearest_CS.h"; var = "g_Nearest_CS" },
    @{ hlsl = "Bicubic_CS.hlsl"; out = "Bicubic_CS.h"; var = "g_Bicubic_CS" },
    @{ hlsl = "Lanczos_CS.hlsl"; out = "Lanczos_CS.h"; var = "g_Lanczos_CS" },
    @{ hlsl = "FSR_EASU_CS.hlsl"; out = "FSR_EASU_CS.h"; var = "g_FsrEasu_CS" },
    @{ hlsl = "FSR_RCAS_CS.hlsl"; out = "FSR_RCAS_CS.h"; var = "g_FsrRcas_CS" },
    @{ hlsl = "Anime4K_3D_Pass1_CS.hlsl"; out = "Anime4K_3D_Pass1_CS.h"; var = "g_Anime4K_3D_Pass1_CS" },
    @{ hlsl = "Anime4K_3D_Pass2_CS.hlsl"; out = "Anime4K_3D_Pass2_CS.h"; var = "g_Anime4K_3D_Pass2_CS" },
    @{ hlsl = "Anime4K_3D_Pass3_CS.hlsl"; out = "Anime4K_3D_Pass3_CS.h"; var = "g_Anime4K_3D_Pass3_CS" },
    @{ hlsl = "Anime4K_3D_AA_Pass1_CS.hlsl"; out = "Anime4K_3D_AA_Pass1_CS.h"; var = "g_Anime4K_3D_AA_Pass1_CS" },
    @{ hlsl = "Anime4K_3D_AA_Pass2_CS.hlsl"; out = "Anime4K_3D_AA_Pass2_CS.h"; var = "g_Anime4K_3D_AA_Pass2_CS" },
    @{ hlsl = "Anime4K_3D_AA_Pass3_CS.hlsl"; out = "Anime4K_3D_AA_Pass3_CS.h"; var = "g_Anime4K_3D_AA_Pass3_CS" },
    @{ hlsl = "Anime4K_Final_CS.hlsl"; out = "Anime4K_Final_CS.h"; var = "g_Anime4K_Final_CS" }
)

foreach ($s in $shaders) {
    $hlslPath = Join-Path $Root ("Scaler/Shaders/" + $s.hlsl)
    $outPath = Join-Path $Root ("Scaler/Shaders/" + $s.out)
    & $compiler $hlslPath "main" "cs_5_0" $outPath $s.var
    if ($LASTEXITCODE -ne 0) { throw "Failed to compile $($s.hlsl)" }
}

Write-Host "== Step 3: Compile Windows Resources ==" -ForegroundColor Cyan
& $Windres -I "$Root" -I "$Root\MenuTools" "$Root\MenuTools\MenuTools.rc" --target=pe-x86-64 -O coff -o "$Root\MenuTools.res"
& $Windres -I "$Root" -I "$Root\MenuTools" "$Root\MenuTools\MenuTools.rc" --target=pe-i386 -O coff -o "$Root\MenuTools32.res"

Write-Host "== Step 4: Compile Hook DLLs ==" -ForegroundColor Cyan
& $Clang64 -shared -O2 -static "-Wl,--gc-sections" -I "$Root" -I "$Root\MenuToolsHook" -DUNICODE -D_UNICODE -D_WIN64 `
    "$Root\MenuToolsHook\MenuTools.cpp" "$Root\MenuToolsHook\MenuToolsHook.cpp" `
    "$Root\MenuCommon\TrayIcon.cpp" "$Root\MenuToolsHook\MenuToolsHook64.def" `
    -o "$Root\MenuToolsHook64.dll" -luser32 -lshell32 -lcomctl32
if ($LASTEXITCODE -ne 0) { throw "MenuToolsHook64.dll failed" }

& $Clang32 -shared -O2 -static "-Wl,--gc-sections" -I "$Root" -I "$Root\MenuToolsHook" -DUNICODE -D_UNICODE -D_WIN32 `
    "$Root\MenuToolsHook\MenuTools.cpp" "$Root\MenuToolsHook\MenuToolsHook.cpp" `
    "$Root\MenuCommon\TrayIcon.cpp" "$Root\MenuToolsHook\MenuToolsHook.def" `
    -o "$Root\MenuToolsHook.dll" -luser32 -lshell32 -lcomctl32
if ($LASTEXITCODE -ne 0) { throw "MenuToolsHook.dll failed" }

Write-Host "== Step 5: Compile MenuTools Executables ==" -ForegroundColor Cyan
& $Clang64 -O2 -static "-Wl,--gc-sections" -mwindows -municode -DUNICODE -D_UNICODE -I "$Root" -I "$Root\MenuTools" -I "$Root\MenuCommon" `
    "$Root\MenuTools\MenuTools.cpp" "$Root\MenuTools\Hooks.cpp" "$Root\MenuTools\Startup.cpp" `
    "$Root\MenuTools\TaskbarVolume.cpp" "$Root\MenuTools\stdafx.cpp" "$Root\MenuCommon\TrayIcon.cpp" `
    "$Root\MenuTools.res" -o "$Root\MenuTools64.exe" -luser32 -lshell32 -lcomctl32 -lole32 -loleaut32 -lgdi32 -lshlwapi
if ($LASTEXITCODE -ne 0) { throw "MenuTools64.exe failed" }

& $Clang32 -O2 -static "-Wl,--gc-sections" -mwindows -municode -DUNICODE -D_UNICODE -I "$Root" -I "$Root\MenuTools" -I "$Root\MenuCommon" `
    "$Root\MenuTools\MenuTools.cpp" "$Root\MenuTools\Hooks.cpp" "$Root\MenuTools\Startup.cpp" `
    "$Root\MenuTools\TaskbarVolume.cpp" "$Root\MenuTools\stdafx.cpp" "$Root\MenuCommon\TrayIcon.cpp" `
    "$Root\MenuTools32.res" -o "$Root\MenuTools.exe" -luser32 -lshell32 -lcomctl32 -lole32 -loleaut32 -lgdi32 -lshlwapi
if ($LASTEXITCODE -ne 0) { throw "MenuTools.exe failed" }

Write-Host "== Step 6: Compile MenuToolsScaler.exe ==" -ForegroundColor Cyan
& $Clang64 -std=c++17 -municode -mwindows -D_UNICODE -DUNICODE -O2 -static "-Wl,--gc-sections" -I "$Root" -I "$Root\Scaler" -I "$Root\MenuCommon" `
    "$Root\Scaler\ScalerMain.cpp" -o "$Root\MenuToolsScaler.exe" `
    -ld3d11 -ldxgi -ldwmapi -lruntimeobject -lole32 -loleaut32 -luser32 -lgdi32 -ladvapi32 -lshlwapi
if ($LASTEXITCODE -ne 0) { throw "MenuToolsScaler.exe failed" }

Copy-Item "$Root\MenuToolsScaler.exe" "$Root\Scaler\MenuToolsScaler.exe" -Force

Write-Host "== Step 7: Strip Binaries & Assert Size Gate ==" -ForegroundColor Cyan
$files = @(
    "$Root\MenuTools.exe",
    "$Root\MenuTools64.exe",
    "$Root\MenuToolsHook.dll",
    "$Root\MenuToolsHook64.dll",
    "$Root\MenuToolsScaler.exe"
)

foreach ($f in $files) {
    & $Strip --strip-all $f
}

$total = 0
foreach ($f in $files) {
    $len = (Get-Item $f).Length
    $total += $len
    Write-Host ("{0,-22} : {1,9:N0} bytes ({2:N2} KiB)" -f (Split-Path $f -Leaf), $len, ($len / 1024))
}
Write-Host "----------------------------------------------------"
Write-Host ("{0,-22} : {1,9:N0} bytes ({2:N2} MiB)" -f "TOTAL", $total, ($total / (1024*1024)))
$limit = 3145728 # 3 MiB
if ($total -le $limit) {
    Write-Host ("PASS: Strict Size Gate ({0} <= {1} bytes)" -f $total, $limit) -ForegroundColor Green
} else {
    Write-Error ("FAIL: Strict Size Gate exceeded ({0} > {1} bytes)" -f $total, $limit)
}
