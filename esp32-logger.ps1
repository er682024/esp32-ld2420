# ============================================================
#  ESP32 LD2420 — Data Logger
#  Salva i dati da /data ogni minuto in file JSON + CSV
# ============================================================

param(
    [string]$Link     = "http://192.168.1.131",
    [string]$OutDir   = "$PSScriptRoot\logs",
    [int]   $Interval = 60,          # secondi tra un campione e l'altro
    [string]$User     = "admin",
    [string]$Pass     = "esp32admin"
)

$b64  = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("${User}:${Pass}"))
$headers = @{ Authorization = "Basic $b64" }

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }

$csvFile = Join-Path $OutDir "esp32_log.csv"
if (-not (Test-Path $csvFile)) 
{
    $csvHeader = "timestamp,uptime_s,presence,motion,static_presence,presence_ms,absence_ms,motion_ms,static_ms,dynamic_ms,change_ms,presence_total_ms,absence_total_ms,presence_pct,min_dist,max_dist,wifi_ssid,wifi_rssi,wifi_quality,wifi_channel"
    $csvHeader | Out-File -FilePath $csvFile -Encoding UTF8
}

Write-Host "ESP32 Logger avviato $Link/data"
Write-Host "Output: $OutDir"
Write-Host "Intervallo: ${Interval}s   [Ctrl+C per fermare]`n"

while ($true) 
{
    $ts = Get-Date -Format "yyyy-MM-dd HH:mm:ss"

    try 
    {
        $resp = Invoke-RestMethod -Uri "$Link/data" -Headers $headers -TimeoutSec 10

        $day     = Get-Date -Format "yyyy-MM-dd"
        $jsonDir = Join-Path $OutDir $day
        if (-not (Test-Path $jsonDir)) 
        {
             New-Item -ItemType Directory -Path $jsonDir | Out-Null 
        }

        $jsonFile = Join-Path $jsonDir ("$( (Get-Date -Format 'HH-mm-ss') ).json")
        $resp | ConvertTo-Json -Depth 5 | Out-File -FilePath $jsonFile -Encoding UTF8

        $csv = "$ts,$($resp.uptime_s),$($resp.presence),$($resp.motion),$($resp.static_presence),$($resp.presence_ms),($resp.absence_ms),$($resp.motion_ms),$($resp.static_ms),$($resp.dynamic_ms),$($resp.change_ms),$($resp.presence_total_ms),$($resp.absence_total_ms),$($resp.presence_pct),$($resp.min_dist),$($resp.max_dist),$($resp.wifi.ssid),$($resp.wifi.rssi),$($resp.wifi.quality),$($resp.wifi.channel)"
        $csv | Out-File -FilePath $csvFile -Append -Encoding UTF8

        $pres  = if ($resp.presence)        { "PRESENTE" } else { "assente" }
        $mot   = if ($resp.motion)          { " MOV"     } else { ""        }
        $stat  = if ($resp.static_presence) { " STAT"    } else { ""        }
        
        #    Write-Host "[$ts]  {0,-10}  {1}{2}  uptime={3}s  rssi={4}dBm  pct={5:F1}%" -f $pres, $mot, $stat, $resp.uptime_s, $resp.wifi.rssi, $resp.presence_pct
        Write-Host $ts  $pres, $mot, $stat, "Uptime:", $resp.uptime_s, "RSSI:", $resp.wifi.rssi, "PresPerc:", $resp.presence_pct, "%"
    } 
    catch 
    {
        Write-Host "EXCEPTION" # $_.Exception.Message
        $_
    }

    Start-Sleep -Seconds $Interval
}
