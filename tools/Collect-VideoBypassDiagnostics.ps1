param(
    [ValidateRange(1, 600)][int]$Seconds = 120,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../dist/video-bypass-diagnostics')
)
$ErrorActionPreference = 'Stop'
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
$runPath = Join-Path $outputRoot ([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $runPath -Force | Out-Null
# Read-only observations. No packet bodies, URLs, browser profiles, credentials,
# settings, drivers, services or routes are changed by this collector.
$errors = [Collections.Generic.List[string]]::new()
function Save-Observation([string]$Name, [scriptblock]$Read) {
    try { & $Read | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runPath "$Name.json") -Encoding utf8 }
    catch {
        $errors.Add("${Name}: $($_.Exception.Message)")
        if ($Name -like 'routes-*') {
            route.exe print | Set-Content -LiteralPath (Join-Path $runPath "$Name.native.txt") -Encoding utf8
        } elseif ($Name -eq 'addresses') {
            ipconfig.exe /all | Set-Content -LiteralPath (Join-Path $runPath 'addresses.native.txt') -Encoding utf8
        }
    }
}
Save-Observation 'versions' {
    foreach ($name in @('AmneziaVPN', 'AmneziaVPN-service')) {
        Get-Process -Name $name -ErrorAction SilentlyContinue | ForEach-Object {
            [pscustomobject]@{ Name=$_.ProcessName; PID=$_.Id; Path=$_.Path; FileVersion=$(if ($_.Path) { [Diagnostics.FileVersionInfo]::GetVersionInfo($_.Path).FileVersion }) }
        }
    }
}
Save-Observation 'routes-before' { Get-NetRoute | Select-Object AddressFamily,DestinationPrefix,NextHop,InterfaceIndex,RouteMetric,State }
Save-Observation 'addresses' { Get-NetIPAddress | Select-Object IPAddress,InterfaceIndex,InterfaceAlias,AddressFamily,AddressState,PrefixOrigin }
Save-Observation 'rutube-dns' { Get-DnsClientCache | Where-Object { $_.Entry -match '(^|\.)(rutube\.ru|rtbcdn\.ru)$' } | Select-Object Entry,RecordType,Data,TimeToLive }
$rows = [Collections.Generic.List[object]]::new()
$deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
while ([DateTime]::UtcNow -lt $deadline) {
    $firefox = @(Get-Process firefox -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)
    if ($firefox.Count) {
        try {
            Get-NetTCPConnection -ErrorAction Stop | Where-Object { $_.OwningProcess -in $firefox } | ForEach-Object {
                $rows.Add([pscustomobject]@{ UTC=[DateTime]::UtcNow.ToString('o'); PID=$_.OwningProcess; LocalAddress=$_.LocalAddress; LocalPort=$_.LocalPort; RemoteAddress=$_.RemoteAddress; RemotePort=$_.RemotePort; State=$_.State.ToString() })
            }
        } catch {
            if (!$errors.Contains('TCP CIM unavailable; using filtered native netstat')) {
                $errors.Add('TCP CIM unavailable; using filtered native netstat')
            }
            netstat.exe -ano -p TCP | ForEach-Object {
                $parts = $_.Trim() -split '\s+'
                if ($parts.Count -eq 5 -and $parts[0] -eq 'TCP' -and [int]$parts[4] -in $firefox) {
                    $rows.Add([pscustomobject]@{ UTC=[DateTime]::UtcNow.ToString('o'); PID=[int]$parts[4]; LocalAddress=$parts[1]; LocalPort=''; RemoteAddress=$parts[2]; RemotePort=''; State=$parts[3] })
                }
            }
        }
    }
    Start-Sleep -Milliseconds 1000
}
if ($rows.Count) { $rows | Export-Csv -LiteralPath (Join-Path $runPath 'firefox-tcp.csv') -Encoding utf8 -NoTypeInformation }
Save-Observation 'routes-after' { Get-NetRoute | Select-Object AddressFamily,DestinationPrefix,NextHop,InterfaceIndex,RouteMetric,State }
$logPaths = @(
    (Join-Path $env:ProgramData 'AmneziaVPN/log/AmneziaVPN-service.log'),
    (Join-Path $env:APPDATA 'AmneziaVPN.ORG/AmneziaVPN/log/AmneziaVPN.log')
)
foreach ($logPath in $logPaths) {
    if (Test-Path -LiteralPath $logPath) {
        Get-Content -LiteralPath $logPath -Tail 3000 | Where-Object {
            $_.Length -lt 1500 -and $_ -match 'healthcheck|IPv6 bypass|Ipv6 Conversation|IOCTL_.*(timeout|failed)|Split tunnel.*(failed|unresponsive)|app bypass.*unavailable|preserving session'
        } | Select-Object -Last 200 | Set-Content -LiteralPath (Join-Path $runPath ([IO.Path]::GetFileName($logPath) + '.selected.txt')) -Encoding utf8
    }
}
$errors | Set-Content -LiteralPath (Join-Path $runPath 'collector-errors.txt') -Encoding utf8
Write-Output "Saved read-only diagnostics: $runPath"
if ($errors.Count) { Write-Output 'Some CIM observations were unavailable. Native fallbacks and collector-errors.txt describe the limits.' }
Write-Output 'TCP samples do not capture HTTP/3 (QUIC) or the failed video URL. Use Firefox Network tools to record the failed request separately.'
