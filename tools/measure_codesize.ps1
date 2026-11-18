# Estimates the ARM code size of the generated game code without a full build: compiles every Nth bucket file of
# src/nfsc/generated with the game-code flags of the clang build (-O2, Thumb-2, no LTO) and scales the sampled .text
# by the share of the generated source it covers.
# Usage: tools/measure_codesize.ps1 [-Every 25]
param(
    [int]$Every = 25
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')
if (-not $env:NFSU_LLVM) { $env:NFSU_LLVM = Get-Dependency 'llvm' }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$gen = Join-Path $repo 'src/nfsc/generated'
$out = Join-Path $repo 'work/codesize'
New-Item -ItemType Directory -Force $out | Out-Null

$sysroot = Join-Path $env:VITASDK 'arm-vita-eabi'
$cxxInclude = (Get-ChildItem (Join-Path $sysroot 'include/c++') -Directory | Select-Object -First 1).FullName
$clang = Join-Path $env:NFSU_LLVM 'bin/clang++.exe'
$readelf = Join-Path $env:NFSU_LLVM 'bin/llvm-readelf.exe'
$flags = @('--target=armv7a-none-eabihf', "--sysroot=$sysroot", '-isystem', $cxxInclude, '-isystem',
           (Join-Path $cxxInclude 'arm-vita-eabi'), '-mthumb', '-fshort-enums', '-fno-pic', '-fno-pie', '-femulated-tls',
           '-D__vita__=1', '-mcpu=cortex-a9', '-mfpu=neon', '-mfloat-abi=hard', '-ffp-contract=off', '-fno-math-errno',
           '-fno-exceptions', '-fno-asynchronous-unwind-tables', '-fno-unwind-tables', '-ffunction-sections',
           '-fdata-sections', '-std=gnu++17', '-O2', '-fno-slp-vectorize', '-w', '-DVITA', '-DWITH_MMX',
           '-I', (Join-Path $repo 'include'), '-I', (Join-Path $repo 'src/nfsc'), '-I', $gen, '-c')

$all = @(Get-ChildItem $gen -Filter 'nfsc.*.cpp' | Sort-Object Name)
$sample = @(for ($i = 0; $i -lt $all.Count; $i += $Every) { $all[$i] })
Write-Host "Compiling $($sample.Count) of $($all.Count) bucket files..."
$started = Get-Date
$ErrorActionPreference = 'Continue'
$failed = 0
foreach ($f in $sample) {
    $obj = Join-Path $out ($f.BaseName + '.o')
    & $clang @flags $f.FullName -o $obj 2>&1 | Select-Object -First 5 | ForEach-Object { Write-Host "  $_" }
    if ($LASTEXITCODE -ne 0) { $failed++; Write-Host "FAILED: $($f.Name)" }
}
$text = 0
foreach ($o in Get-ChildItem $out -Filter '*.o') {
    foreach ($line in & $readelf -S --wide $o.FullName) {
        if ($line -match '\]\s+\.text\S*\s+PROGBITS\s+\S+\s+\S+\s+([0-9a-f]+)') { $text += [Convert]::ToInt64($Matches[1], 16) }
    }
}
$sampleBytes = ($sample | Measure-Object Length -Sum).Sum
$allBytes = ($all | Measure-Object Length -Sum).Sum
$estimate = $text * $allBytes / $sampleBytes
'{0} files compiled ({1} failed) in {2:N0} s; sample .text {3:N1} MB for {4:N1} MB of C++' -f $sample.Count, $failed,
    ((Get-Date) - $started).TotalSeconds, ($text / 1MB), ($sampleBytes / 1MB)
'Estimated ARM code for all {0} files ({1:N0} MB of C++): {2:N0} MB' -f $all.Count, ($allBytes / 1MB), ($estimate / 1MB)
