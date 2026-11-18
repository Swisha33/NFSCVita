# Configures the clang release build (build-release-clang) without building, for building single targets:
#   tools/configure.ps1; cmake --build build-release-clang --target nfsu_runtime
# tools/build.ps1 does the same configuration and then builds the VPK.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')
if (-not $env:NFSU_LLVM) { $env:NFSU_LLVM = Get-Dependency 'llvm' }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = Join-Path $repo 'build-release-clang'
$ErrorActionPreference = 'Continue'
cmake -S $repo -B $build -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$repo/cmake/vita-clang.cmake" -DCMAKE_BUILD_TYPE=Release `
      -DNFSU_COMPILER=clang -DNFSU_LTO=ON -DNFSU_PGO_GENERATE=OFF -DNFSU_PGO_USE= -DNFSU_PGO_LIST=
if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
