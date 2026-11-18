# Compiles the port's Cg programs on the Vita with its own compiler (libshacccg.suprx in ur0:data) and fetches the
# .gxp files into shaders/gxp/. Port of NFSUG2_VITA's tools/shadercomp/vita_compile.py (MIT).
# Usage: tools/shadercomp.ps1 [-Only name,name...] [-Compare] [-Vita <ip>]
#   -Only     compile only these programs (default: every program cmake/shaders.cmake lists)
#   -Compare  write into work/gxp_compare/ instead of shaders/gxp/ and report which binaries differ
# The installed eboot.bin must contain the compile mode (src/platform/shadercomp.cpp); the script uploads the Cg
# sources and shaders/compile.txt, starts the app (which compiles and exits), waits for compile.log to end, then
# downloads out/<name>.gxp.
param(
    [string[]]$Only,
    [switch]$Compare,
    [string]$Vita = "",
    [string]$TitleId = "NFSCVITA0",
    [int]$TimeoutSeconds = 300
)
$ErrorActionPreference = "Stop"
# The Vita's address: -Vita, else $env:VITA_IP (set it in tools/env.local.ps1, which git ignores).
if (-not $Vita) {
    $localEnv = Join-Path $PSScriptRoot 'env.local.ps1'
    if (Test-Path $localEnv) { . $localEnv }
    $Vita = $env:VITA_IP
}
if (-not $Vita) { throw "No Vita address: pass -Vita <ip> or set `$env:VITA_IP (e.g. in tools/env.local.ps1)" }
. (Join-Path $PSScriptRoot 'env.ps1')
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ftp = "ftp://${Vita}:1337"
$remote = "ux0:/data/NFSCVITA/shaders"

function Send-Command([string]$command) {
    $client = New-Object Net.Sockets.TcpClient
    if (-not $client.ConnectAsync($Vita, 1338).Wait(5000)) { $client.Close(); throw "vita: command port 1338 does not answer" }
    $bytes = [Text.Encoding]::ASCII.GetBytes("$command`n")
    $stream = $client.GetStream()
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
    Start-Sleep -Milliseconds 500
    $client.Close()
}

function Ftp-Put([string]$local, [string]$path) {
    & curl.exe -s --connect-timeout 5 -m 60 --ftp-create-dirs -T $local "$ftp/$path"
    if ($LASTEXITCODE -ne 0) { throw "upload of $local failed (curl $LASTEXITCODE)" }
}

# The manifest: one line per program, "<name> <profile> <source> [DEFINES...]" (cmake prints them as messages).
$listScript = Join-Path $PSScriptRoot 'shadercomp_list.cmake'
$lines = & cmd /c "cmake -DREPO=$($repo -replace '\\','/') -P `"$listScript`" 2>&1" |
    ForEach-Object { "$_".Trim() } | Where-Object { $_ -match '^\w+ sce_[vf]p_psp2 ' }
if ($Only) {
    $Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })    # powershell -File passes "a,b" whole
    $lines = $lines | Where-Object { $Only -contains ($_ -split ' ')[0] }
}
if (-not $lines) { throw "no programs to compile" }
$names = $lines | ForEach-Object { ($_ -split ' ')[0] }
Write-Host "shadercomp: $($names.Count) programs"

$tmp = Join-Path $repo 'work\shadercomp'
New-Item -ItemType Directory -Force $tmp | Out-Null
$manifest = Join-Path $tmp 'compile.txt'
Set-Content -Path $manifest -Value ($lines -join "`n") -Encoding ascii

Send-Command "destroy"
Start-Sleep -Seconds 2
foreach ($cg in Get-ChildItem (Join-Path $repo 'shaders') -Filter *.cg) { Ftp-Put $cg.FullName "$remote/src/$($cg.Name)" }
& curl.exe -s --connect-timeout 5 -m 30 -Q "DELE /$remote/compile.log" "$ftp/" 2>$null | Out-Null
Ftp-Put $manifest "$remote/compile.txt"
Send-Command "launch $TitleId"

$log = Join-Path $tmp 'compile.log'
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$done = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 3
    & curl.exe -s --connect-timeout 5 -m 30 -o $log "$ftp/$remote/compile.log" 2>$null
    if ($LASTEXITCODE -eq 0 -and (Test-Path $log) -and (Select-String -Path $log -Pattern '^done ' -Quiet)) { $done = $true; break }
}
if (Test-Path $log) { Get-Content $log | Where-Object { $_ -match '^(compiler|version|failed|error|done)|  (error|warning)' } }
if (-not $done) { throw "shadercomp: no 'done' line in compile.log after $TimeoutSeconds s" }

$out = if ($Compare) { Join-Path $repo 'work\gxp_compare' } else { Join-Path $repo 'shaders\gxp' }
New-Item -ItemType Directory -Force $out | Out-Null
$differ = @()
foreach ($n in $names) {
    $target = Join-Path $out "$n.gxp"
    & curl.exe -s --connect-timeout 5 -m 30 -o $target "$ftp/$remote/out/$n.gxp"
    if ($LASTEXITCODE -ne 0) { Write-Warning "shadercomp: $n.gxp not fetched"; continue }
    if ($Compare) {
        $shipped = Join-Path $repo "shaders\gxp\$n.gxp"
        if (-not (Test-Path $shipped)) { $differ += "$n (new)" }
        elseif ((Get-FileHash $shipped).Hash -ne (Get-FileHash $target).Hash) { $differ += $n }
    }
}
if ($Compare) { Write-Host "shadercomp: $($differ.Count) differ from shaders/gxp: $($differ -join ', ')" }
Write-Host "shadercomp: gxp files in $out"
