# Test loop on the Vita over the network (vitacompanion: FTP on 1337, commands on 1338).
#   vita.ps1 deploy          close the app, upload build-release-clang/NFSCVita.self as ux0:app/NFSCVITA0/eboot.bin
#   vita.ps1 launch          close the app and start it
#   vita.ps1 log [-Out f]    fetch ux0:data/NFSCVITA/nfscvita.log (default work\vita_latest.log)
#   vita.ps1 run [-Seconds n] deploy + launch, wait n seconds (default 90), fetch the log
# Only code changes go this way; a changed sce_sys or new files in the VPK need a VPK install.
param(
    [Parameter(Mandatory = $true)][ValidateSet("deploy", "launch", "log", "run", "close")][string]$Action,
    [string]$Vita = "",
    [string]$TitleId = "NFSCVITA0",
    [int]$Seconds = 90,
    [string]$Out = "",
    [string]$Self = ""   # another build's .self (default build-release-clang)
)
$ErrorActionPreference = "Stop"
# The Vita's address: -Vita, else $env:VITA_IP (set it in tools/env.local.ps1, which git ignores).
if (-not $Vita) {
    $localEnv = Join-Path $PSScriptRoot 'env.local.ps1'
    if (Test-Path $localEnv) { . $localEnv }
    $Vita = $env:VITA_IP
}
if (-not $Vita) { throw "No Vita address: pass -Vita <ip> or set `$env:VITA_IP (e.g. in tools/env.local.ps1)" }
$root = Split-Path -Parent $PSScriptRoot
$ftp = "ftp://${Vita}:1337"

function Send-Command([string]$command) {
    $client = New-Object Net.Sockets.TcpClient
    # A hung plugin accepts nothing: fail after 5 s instead of waiting forever.
    if (-not $client.ConnectAsync($Vita, 1338).Wait(5000)) { $client.Close(); throw "vita: command port 1338 does not answer" }
    $client.SendTimeout = 5000
    $bytes = [Text.Encoding]::ASCII.GetBytes("$command`n")
    $stream = $client.GetStream()
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
    Start-Sleep -Milliseconds 500
    $client.Close()
    Write-Host "vita: $command"
}

function Deploy {
    $self = if ($Self) { (Resolve-Path $Self).Path } else { Join-Path $root "build-release-clang\NFSCVita.self" }
    if (-not (Test-Path $self)) { throw "missing $self" }
    Send-Command "destroy"
    Start-Sleep -Seconds 2
    & curl.exe -s --connect-timeout 5 -m 300 -T $self "$ftp/ux0:/app/$TitleId/eboot.bin"
    if ($LASTEXITCODE -ne 0) { throw "upload failed (curl $LASTEXITCODE)" }
    $listing = & curl.exe -s --connect-timeout 5 -m 30 "$ftp/ux0:/app/$TitleId/"
    $line = $listing | Where-Object { $_ -match "eboot\.bin" }
    $size = (Get-Item $self).Length
    if (-not ($line -match "\s$size\s")) { throw "uploaded eboot.bin size mismatch: $line (expected $size)" }
    Write-Host "vita: eboot.bin uploaded ($size bytes)"
}

function Remove-Log {
    # Keep a copy of the log first: it may be the user's own session (a test launch once lost one, 2026-11-24).
    $kept = Join-Path $root ("work\vita_kept_" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".log")
    & curl.exe -s --connect-timeout 5 -m 60 -o $kept "$ftp/ux0:/data/NFSCVITA/nfscvita.log"
    if ((Test-Path $kept) -and (Get-Item $kept).Length -eq 0) { Remove-Item $kept }
    & curl.exe -s --connect-timeout 5 -m 30 -Q "DELE /ux0:/data/NFSCVITA/nfscvita.log" "$ftp/" | Out-Null
}

function Log-Exists {
    $listing = & curl.exe -s --connect-timeout 5 -m 30 "$ftp/ux0:/data/NFSCVITA/"
    return [bool]($listing | Where-Object { $_ -match "nfscvita\.log" })
}

# The old log is deleted first, so a log after the launch proves the game started (a crash dialog left on the
# screen can stop the launch).
function Launch {
    Send-Command "destroy"
    Start-Sleep -Seconds 2
    Remove-Log
    Send-Command "launch $TitleId"
    Start-Sleep -Seconds 15
    if (-not (Log-Exists)) { Write-Warning "vita: no log 15 s after the launch: the game did not start" }
}

function Fetch-Log {
    $target = if ($Out) { $Out } else { Join-Path $root "work\vita_latest.log" }
    & curl.exe -s --connect-timeout 5 -m 60 -o $target "$ftp/ux0:/data/NFSCVITA/nfscvita.log"
    if ($LASTEXITCODE -ne 0) { throw "log fetch failed (curl $LASTEXITCODE)" }
    Write-Host "vita: log -> $target ($((Get-Item $target).Length) bytes)"
}

switch ($Action) {
    "deploy" { Deploy }
    "launch" { Launch }
    "close"  { Send-Command "destroy" }
    "log"    { Fetch-Log }
    "run"    { Deploy; Launch; Start-Sleep -Seconds $Seconds; Fetch-Log }
}
