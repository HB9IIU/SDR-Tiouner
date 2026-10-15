#Requires -RunAsAdministrator
param([string]$PackageDirectory = $PSScriptRoot)
$ErrorActionPreference = 'Stop'
$packageRoot = (Resolve-Path -LiteralPath $PackageDirectory).Path
$settings = @{}
Get-Content -LiteralPath (Join-Path $packageRoot 'multi-receiver.ini') | ForEach-Object {
    if ($_ -match '^\s*([a-z_]+)\s*=\s*(\d+)\s*$') { $settings[$Matches[1]] = [int]$Matches[2] }
}
function Setting([string]$Name, [int]$Default) {
    if ($settings.ContainsKey($Name)) { return $settings[$Name] }
    return $Default
}
$web = Setting 'http_port' 47321
$video = Setting 'video_port_base' 18900
$media = Setting 'ice_port_base' 18190
$count = Setting 'max_channels' 4
if ($count -lt 1 -or $count -gt 16) { throw 'max_channels must be between 1 and 16.' }
foreach ($port in @($web, $video, $media)) {
    if ($port -lt 1024 -or $port + $count -gt 65535) { throw 'Invalid configured LAN port range.' }
}
$app = Join-Path $packageRoot 'SDR-Tiouner.exe'
$mtx = Join-Path $packageRoot 'mediamtx.exe'
if (!(Test-Path -LiteralPath $app) -or !(Test-Path -LiteralPath $mtx)) { throw 'Run this helper from the SDR-Tiouner package folder.' }
$hash = [Security.Cryptography.SHA256]::Create()
try { $key = ([BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($packageRoot.ToLowerInvariant())))).Replace('-', '').Substring(0, 12) }
finally { $hash.Dispose() }
$rules = @(
    @{ Suffix='Web'; Program=$app; Protocol='TCP'; Ports=@($web) },
    @{ Suffix='Video'; Program=$mtx; Protocol='TCP'; Ports=@($video..($video+$count-1)) },
    @{ Suffix='Media'; Program=$mtx; Protocol='UDP'; Ports=@($media..($media+$count-1)) }
)
foreach ($rule in $rules) {
    $name = "SDR-Tiouner-LAN-$key-$($rule.Suffix)"
    Get-NetFirewallRule -Name $name -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    New-NetFirewallRule -Name $name -DisplayName "SDR-Tiouner LAN $($rule.Suffix) ($key)" -Direction Inbound -Action Allow -Profile Private -RemoteAddress LocalSubnet -Program $rule.Program -Protocol $rule.Protocol -LocalPort $rule.Ports | Out-Null
}
Write-Host 'LAN access enabled for this package on Private networks, from the local subnet.'
