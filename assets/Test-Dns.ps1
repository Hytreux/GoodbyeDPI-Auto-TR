$ErrorActionPreference = 'Stop'
$script:guid = '{11111111-1111-1111-1111-111111111111}'
$script:adapterIndex = 4
$script:static4 = @()
$script:static6 = @('2001:db8::1','2001:db8::2')
$script:physical = $true
$script:splitRoute = $false
$script:noV6Route = $false
$script:failV6 = $false
$script:flushes = 0
function Get-NetRoute { param($DestinationPrefix) if ($DestinationPrefix -eq '::/0' -and $script:noV6Route) { return }; [pscustomobject]@{ InterfaceIndex = $(if ($DestinationPrefix -eq '::/0' -and $script:splitRoute) { 99 } else { $script:adapterIndex }); RouteMetric = 10; State = 'Alive' } }
function Get-NetIPInterface { param($InterfaceIndex,$AddressFamily) [pscustomobject]@{ ConnectionState = 'Connected'; InterfaceMetric = 15 } }
function Get-NetAdapter { param($InterfaceIndex,[switch]$IncludeHidden) [pscustomobject]@{ ifIndex = $script:adapterIndex; InterfaceGuid = $script:guid; Name = 'Fake Wi-Fi'; HardwareInterface = $script:physical } }
function Get-ItemProperty { param($LiteralPath,$Name) [pscustomobject]@{ NameServer = $(if ($LiteralPath -match '\\Tcpip6\\') { $script:static6 -join ',' } else { $script:static4 -join ',' }) } }
function Get-DnsClientServerAddress {
    param($InterfaceIndex,$AddressFamily)
    if ($InterfaceIndex -ne $script:adapterIndex) { throw 'Stale interface index used' }
    [pscustomobject]@{ Family = $AddressFamily; ServerAddresses = $(if ($AddressFamily -eq 'IPv4') { if ($script:static4.Count) { $script:static4 } else { @('192.0.2.53') } } else { $script:static6 }) }
}
function Set-DnsClientServerAddress {
    [CmdletBinding()] param([Parameter(ValueFromPipeline=$true)]$InputObject, [string[]]$ServerAddresses, [switch]$ResetServerAddresses)
    process {
        if ($InputObject.Family -eq 'IPv6' -and $script:failV6) { throw 'Simulated IPv6 failure' }
        $new = if ($ResetServerAddresses) { @() } else { @($ServerAddresses) }
        if ($InputObject.Family -eq 'IPv4') { $script:static4 = @($new) } else { $script:static6 = @($new) }
    }
}
function Clear-DnsClientCache { $script:flushes++ }
function Assert($condition, [string]$message) { if (-not $condition) { throw $message } }
$dnsSource = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'Dns.ps1') -Raw
$testSource = $dnsSource.Replace('[Console]::Error.WriteLine($_.Exception.Message); exit 1', 'throw')
$runner = [scriptblock]::Create($testSource)
$snapshot = (& $runner -Action Snapshot) | ConvertFrom-Json
Assert (-not $snapshot.V4Static) 'IPv4 DHCP mode was lost'
Assert ($snapshot.V6Static -and $snapshot.V6Servers.Count -eq 2) 'IPv6 static DNS was lost'
Assert ($snapshot.AdapterGuid -eq $script:guid -and $snapshot.Ipv6) 'Adapter or route mismatch'
'PASS: Capture DHCP IPv4 and static IPv6 independently'
$encoded = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(($snapshot | ConvertTo-Json -Compress)))
$null = & $runner -Action Apply -BackupBase64 $encoded
Assert (($script:static4 -join ',') -eq '1.1.1.1,1.0.0.1') 'Cloudflare IPv4 mismatch'
Assert (($script:static6 -join ',') -eq '2606:4700:4700::1111,2606:4700:4700::1001') 'Cloudflare IPv6 mismatch'
'PASS: Apply and verify both Cloudflare address families'
$script:adapterIndex = 21
$null = & $runner -Action Restore -BackupBase64 $encoded
Assert ($script:static4.Count -eq 0) 'IPv4 was not reset to automatic DNS'
Assert (($script:static6 -join ',') -eq '2001:db8::1,2001:db8::2') 'IPv6 static order not restored'
'PASS: Restore by stable adapter GUID after interface index changes'
$script:static4 = @('9.9.9.9','149.112.112.112')
$snapshot2 = (& $runner -Action Snapshot) | ConvertFrom-Json
$encoded2 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(($snapshot2 | ConvertTo-Json -Compress)))
$null = & $runner -Action Apply -BackupBase64 $encoded2
$null = & $runner -Action Restore -BackupBase64 $encoded2
Assert (($script:static4 -join ',') -eq '9.9.9.9,149.112.112.112') 'Static IPv4 not restored'
'PASS: Preserve original static IPv4 resolver ordering'
$script:failV6 = $true
$failed = $false
try { $null = & $runner -Action Apply -BackupBase64 $encoded2 } catch { $failed = $true }
Assert $failed 'Partial DNS failure was hidden'
$script:failV6 = $false
$null = & $runner -Action Restore -BackupBase64 $encoded2
Assert (($script:static4 -join ',') -eq '9.9.9.9,149.112.112.112') 'Partial update not recoverable'
'PASS: Partial update can be restored from unchanged backup'
$script:physical = $false
$failed = $false
try { $null = & $runner -Action Snapshot } catch { $failed = $true }
Assert $failed 'Virtual adapter was accepted'
'PASS: Refuse VPN/virtual active adapter'
$script:physical = $true
$script:splitRoute = $true
$failed = $false
try { $null = & $runner -Action Snapshot } catch { $failed = $true }
Assert $failed 'Split IPv4/IPv6 routes were accepted'
'PASS: Refuse different active IPv4 and IPv6 adapters'
$script:splitRoute = $false
$script:noV6Route = $true
$script:static4 = @()
$script:static6 = @()
$snapshot3 = (& $runner -Action Snapshot) | ConvertFrom-Json
Assert (-not $snapshot3.Ipv6 -and -not $snapshot3.ManageV6) 'Empty IPv6 DNS object was treated as usable IPv6'
$encoded3 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(($snapshot3 | ConvertTo-Json -Compress)))
$null = & $runner -Action Apply -BackupBase64 $encoded3
Assert (($script:static4 -join ',') -eq '1.1.1.1,1.0.0.1') 'IPv4 Cloudflare was not applied without IPv6'
Assert ($script:static6.Count -eq 0) 'IPv6 DNS changed without an IPv6 default route'
$null = & $runner -Action Restore -BackupBase64 $encoded3
'PASS: Skip IPv6 DNS when no IPv6 default route exists'
Assert ($script:flushes -ge 5) 'DNS cache invalidation missing'
'PASS: DNS cache cleared after changes and recovery'
'9/9 DNS adapter tests passed; no real network changes.'
