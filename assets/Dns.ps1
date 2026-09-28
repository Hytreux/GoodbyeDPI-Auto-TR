param([ValidateSet('Snapshot','Apply','Restore')][string]$Action, [string]$BackupBase64)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding
function Get-StaticDns([string]$Guid, [string]$Protocol) {
    $key = 'HKLM:\SYSTEM\CurrentControlSet\Services\' + $Protocol + '\Parameters\Interfaces\' + $Guid
    $entry = Get-ItemProperty -LiteralPath $key -Name NameServer -ErrorAction SilentlyContinue
    $value = if ($entry) { [string]($entry.NameServer -join ',') } else { '' }
    return @($value -split '[,;\s]+' | Where-Object { $_ })
}
try {
    if ($Action -eq 'Snapshot') {
        $routes = @(Get-NetRoute -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue | Where-Object { $_.State -eq 'Alive' })
        $ranked = @($routes | ForEach-Object {
            $ipif = Get-NetIPInterface -InterfaceIndex $_.InterfaceIndex -AddressFamily IPv4
            if ($ipif.ConnectionState -eq 'Connected') {
                [pscustomobject]@{ Index = $_.InterfaceIndex; Cost = [int]$_.RouteMetric + [int]$ipif.InterfaceMetric }
            }
        } | Sort-Object Cost)
        if ($ranked.Count -eq 0) { throw 'Etkin IPv4 internet baglantisi bulunamadi.' }
        $adapter = Get-NetAdapter -InterfaceIndex $ranked[0].Index
        if (-not $adapter.HardwareInterface) { throw 'Etkin baglanti sanal/VPN ag karti. Normal Wi-Fi veya Ethernet baglantisina gecin.' }
        $v6routes = @(Get-NetRoute -DestinationPrefix '::/0' -ErrorAction SilentlyContinue | Where-Object { $_.State -eq 'Alive' } | ForEach-Object {
            $ipif = Get-NetIPInterface -InterfaceIndex $_.InterfaceIndex -AddressFamily IPv6
            if ($ipif.ConnectionState -eq 'Connected') {
                [pscustomobject]@{ Index = $_.InterfaceIndex; Cost = [int]$_.RouteMetric + [int]$ipif.InterfaceMetric }
            }
        } | Sort-Object Cost)
        if ($v6routes.Count -gt 0 -and $v6routes[0].Index -ne $adapter.ifIndex) {
            throw 'IPv4 ve IPv6 farkli ag kartlarindan cikiyor. Bu surum tek etkin internet baglantisini destekler.'
        }
        $guid = ([guid]$adapter.InterfaceGuid).ToString('B')
        $v4 = @(Get-StaticDns $guid 'Tcpip')
        $v6 = @(Get-StaticDns $guid 'Tcpip6')
        # Windows can expose an empty IPv6 DNS object even when this adapter has
        # no usable IPv6 default route. Only configure IPv6 DNS when IPv6 can
        # actually carry internet traffic on the selected adapter.
        $manageV6 = ($v6routes.Count -gt 0) -and (@(Get-DnsClientServerAddress -InterfaceIndex $adapter.ifIndex -AddressFamily IPv6 -ErrorAction SilentlyContinue).Count -gt 0)
        [pscustomobject]@{
            AdapterGuid = $guid; Index = [int]$adapter.ifIndex; Name = [string]$adapter.Name
            Ipv6 = ($v6routes.Count -gt 0); ManageV6 = $manageV6; V4Static = ($v4.Count -gt 0); V4Servers = @($v4)
            V6Static = ($v6.Count -gt 0); V6Servers = @($v6)
        } | ConvertTo-Json -Compress
    } else {
        $backup = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($BackupBase64)) | ConvertFrom-Json
        $adapter = @(Get-NetAdapter -IncludeHidden | Where-Object { ([guid]$_.InterfaceGuid) -eq ([guid]$backup.AdapterGuid) })
        if ($adapter.Count -ne 1) { throw 'Yedeklenen ag karti bulunamadi. Yedek korunuyor; karti yeniden baglayip geri almayi deneyin.' }
        $failures = @()
        foreach ($family in @('IPv4','IPv6')) {
            if ($family -eq 'IPv6' -and -not $backup.ManageV6) { continue }
            try {
                $client = Get-DnsClientServerAddress -InterfaceIndex $adapter[0].ifIndex -AddressFamily $family
                if ($Action -eq 'Apply') {
                    # IPv6 is never disabled. Its resolver configuration is backed up independently.
                    $servers = if ($family -eq 'IPv4') { @('1.1.1.1','1.0.0.1') } else { @('2606:4700:4700::1111','2606:4700:4700::1001') }
                    $client | Set-DnsClientServerAddress -ServerAddresses $servers -ErrorAction Stop
                    $actual = @(Get-DnsClientServerAddress -InterfaceIndex $adapter[0].ifIndex -AddressFamily $family | Select-Object -ExpandProperty ServerAddresses)
                    if (($actual -join ',') -ne ($servers -join ',')) { throw ('DNS ayari dogrulanamadi. Beklenen=' + ($servers -join ',') + ' Okunan=' + ($actual -join ',')) }
                } else {
                    $isStatic = if ($family -eq 'IPv4') { $backup.V4Static } else { $backup.V6Static }
                    $servers = if ($family -eq 'IPv4') { @($backup.V4Servers) } else { @($backup.V6Servers) }
                    if ($isStatic) { $client | Set-DnsClientServerAddress -ServerAddresses $servers -ErrorAction Stop }
                    else { $client | Set-DnsClientServerAddress -ResetServerAddresses -ErrorAction Stop }
                    $protocol = if ($family -eq 'IPv4') { 'Tcpip' } else { 'Tcpip6' }
                    $restored = @(Get-StaticDns $backup.AdapterGuid $protocol)
                    if ($isStatic -and ($restored -join ',') -ne ($servers -join ',')) { throw 'Statik DNS geri yuklemesi dogrulanamadi.' }
                    if (-not $isStatic -and $restored.Count -gt 0) { throw 'Otomatik DNS geri yuklemesi dogrulanamadi.' }
                }
            } catch { $failures += ($family + ': ' + $_.Exception.Message) }
        }
        Clear-DnsClientCache
        if ($failures.Count -gt 0) { throw ($failures -join ' / ') }
        Write-Output 'OK'
    }
} catch { [Console]::Error.WriteLine($_.Exception.Message); exit 1 }
