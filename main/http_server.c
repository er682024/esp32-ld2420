// #include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "http_server.h"
#include "esp_timer.h"
#include <sys/time.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define TAG         "HTTP"
#define ADMIN_USER  "admin"
#define ADMIN_PASS  "esp32admin"   // ← cambia questa password

static httpd_handle_t server = NULL;

/* ------------------------------------------------------------------ */
/*  Dati condivisi — protetti da mutex                                 */
/* ------------------------------------------------------------------ */

static SemaphoreHandle_t s_data_mutex = NULL;
static bool          s_data_valid = false;
static char          s_time[32]   = "--:--:--";
static ld2420_state_t s_sensor_state;
static uint32_t      s_min_dist   = 0;
static uint32_t      s_max_dist   = 0;
static uint64_t total_presence_ms = 0;
static uint64_t total_absence_ms  = 0;
static uint32_t last_update_ms    = 0;


void http_server_update_data(const char *time_str, ld2420_state_t state,
                              uint32_t min, uint32_t max) {
    if (!s_data_mutex) return;
    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(s_time, time_str, sizeof(s_time) - 1);
        s_sensor_state = state;
        s_min_dist = min;
        s_max_dist = max;
        s_data_valid = true;
        xSemaphoreGive(s_data_mutex);
    }
}

/* ------------------------------------------------------------------ */
/*  URL percent-decode in-place                                        */
/* ------------------------------------------------------------------ */

static void url_decode(char *s) {
    char *src = s, *dst = s;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], '\0' };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* ------------------------------------------------------------------ */
/*  Basic Auth                                                          */
/* ------------------------------------------------------------------ */

static bool check_auth(httpd_req_t *req) {
    char auth[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) != ESP_OK) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32\"");
        httpd_resp_send(req, "Non autorizzato", -1);
        return false;
    }
    if (strncmp(auth, "Basic ", 6) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32\"");
        httpd_resp_send(req, "Non autorizzato", -1);
        return false;
    }
    char *encoded = auth + 6;
    unsigned char decoded[65] = {0};
    size_t decoded_len = 0;
    mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                          (unsigned char *)encoded, strlen(encoded));
    decoded[decoded_len] = '\0';
    char expected[64];
    snprintf(expected, sizeof(expected), "%s:%s", ADMIN_USER, ADMIN_PASS);
    if (strcmp((char *)decoded, expected) != 0) {
        ESP_LOGW(TAG, "Auth fallita per una richiesta a %s", req->uri);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32\"");
        httpd_resp_send(req, "Credenziali errate", -1);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/*  Favicon                                                             */
/* ------------------------------------------------------------------ */

static esp_err_t favicon_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  GET /data  (JSON — include SSID e qualità WiFi)                    */
/* ------------------------------------------------------------------ */
extern const char* get_build_date(void);

static esp_err_t data_handler(httpd_req_t *req) {
    char json[2048];
    char     ts[32];
    ld2420_state_t st = ld2420_get_state();
    uint32_t d_min = ld2420_get_min_distance();
    uint32_t d_max = ld2420_get_max_distance();
    uint32_t ms_presence = ld2420_ms_since_presence();
    uint32_t ms_absence = ld2420_ms_since_absence();
    uint32_t ms_motion = ld2420_ms_since_motion();
    uint32_t ms_static = ld2420_ms_since_static_presence();
    uint32_t ms_dynamic = ld2420_ms_since_dynamic_presence();
    uint32_t ms_change = ld2420_ms_since_state_change();

    get_time_str(ts, sizeof(ts));

    uint32_t uptime_s = (long long)(esp_timer_get_time() / 1000000LL);
    double presence_pct = 0.0;
    if (uptime_s > 0) {
        presence_pct = (double)total_presence_ms / (double)(uptime_s * 1000ULL) * 100.0;
    }

    /* WiFi info completo */
    wifi_ap_record_t ap_info = {0};
    char    ssid_str[33]   = "--";
    char    bssid_str[18]  = "--";
    int8_t  rssi           = 0;
    uint8_t channel        = 0;
    int     wifi_quality   = 0;
    const char *label = "N/A";
    const char *icon  = "--";

    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        strncpy(ssid_str, (char *)ap_info.ssid, sizeof(ssid_str) - 1);
        snprintf(bssid_str, sizeof(bssid_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 ap_info.bssid[0], ap_info.bssid[1], ap_info.bssid[2],
                 ap_info.bssid[3], ap_info.bssid[4], ap_info.bssid[5]);

        rssi    = ap_info.rssi;
        channel = ap_info.primary;

        int q = (rssi + 100) * 100 / 60;
        if (q < 0)   q = 0;
        if (q > 100) q = 100;
        wifi_quality = q;

        if      (rssi >= -50) { label = "Ottimo";   icon = "\U0001F4F6"; }
        else if (rssi >= -65) { label = "Buono";    icon = "\U0001F4F6"; }
        else if (rssi >= -75) { label = "Discreto"; icon = "\U0001F4F5"; }
        else if (rssi >= -85) { label = "Scarso";   icon = "\U0001F4F5"; }
        else                  { label = "Pessimo";  icon = "⚠️"; }
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    if (last_update_ms == 0) {
        last_update_ms = now_ms;
    } else {
        uint32_t delta = now_ms - last_update_ms;
        last_update_ms = now_ms;

        if (st.presence)
            total_presence_ms += delta;
        else
            total_absence_ms += delta;
    }

    snprintf(json, sizeof(json),
        "{"
        "\"time\":\"%s\","
        "\"uptime_s\":%" PRIu32 ","
        "\"presence\":%d,"
        "\"motion\":%d,"
        "\"static_presence\":%d,"

        "\"presence_ms\":%" PRIu32 ","
        "\"absence_ms\":%" PRIu32 ","
        "\"motion_ms\":%" PRIu32 ","
        "\"static_ms\":%" PRIu32 ","
        "\"dynamic_ms\":%" PRIu32 ","
        "\"change_ms\":%" PRIu32 ","

        "\"presence_total_ms\":%" PRIu64 ","
        "\"absence_total_ms\":%" PRIu64 ","
        "\"presence_pct\":%.2f,"

        "\"min_dist\":%lu,"
        "\"max_dist\":%lu,"

        "\"build_date\":\"%s\","

        "\"wifi\":{"
            "\"ssid\":\"%s\","
            "\"rssi\":%d,"
            "\"quality\":%d,"
            "\"channel\":%u,"
            "\"bssid\":\"%s\","
            "\"label\":\"%s\","
            "\"icon\":\"%s\""
            "}"
        "}",
        ts,
        uptime_s,
        st.presence,
        st.motion,
        st.static_presence,

        ms_presence,
        ms_absence,
        ms_motion,
        ms_static,
        ms_dynamic,
        ms_change,

        total_presence_ms,
        total_absence_ms,
        presence_pct,

        d_min,
        d_max,

        get_build_date(),

        ssid_str,
        rssi,
        wifi_quality,
        channel,
        bssid_str,
        label,
        icon
    );

    httpd_resp_set_type(req, "application/json");
    // httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json, -1);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  GET /monitor  (HTML dashboard — pagina completamente rifatta)       */
/* ------------------------------------------------------------------ */

static const char *HTML_MONITOR =
"<!DOCTYPE html><html lang='it'><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32 LD2420 Monitor</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:'Segoe UI',sans-serif;background:#0d1117;color:#e6edf3;"
"min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:2rem 1rem}"
"h1{font-size:1.4rem;color:#58a6ff;margin-bottom:0.3rem;letter-spacing:2px;text-transform:uppercase}"
"#clock{font-size:2.4rem;font-weight:bold;color:#f0f6fc;margin-bottom:0.3rem;letter-spacing:3px}"
"#uptime{font-size:0.75rem;color:#484f58;margin-bottom:1.6rem;letter-spacing:1px}"
"nav{display:flex;gap:0.6rem;margin-bottom:1.5rem;flex-wrap:wrap;justify-content:center}"
"nav a{display:flex;align-items:center;gap:0.4rem;padding:0.4rem 0.9rem;"
"background:#161b22;border:1px solid #30363d;border-radius:8px;"
"color:#8b949e;text-decoration:none;font-size:0.8rem;transition:all .2s}"
"nav a:hover{border-color:#58a6ff;color:#58a6ff}"
"nav a.active{border-color:#58a6ff;color:#58a6ff;background:#1a2233}"
/* ---- Layout principale ---- */
".section{width:100%;max-width:520px;margin-bottom:1.2rem}"
".section-title{font-size:0.72rem;color:#8b949e;text-transform:uppercase;"
"letter-spacing:1px;margin-bottom:0.7rem;display:flex;align-items:center;gap:0.4rem}"
/* ---- Card griglia 2 col ---- */
".grid2{display:grid;grid-template-columns:1fr 1fr;gap:1rem}"
".card{background:#161b22;border:1px solid #30363d;border-radius:14px;"
"padding:1.2rem;display:flex;flex-direction:column;align-items:center;"
"gap:0.3rem;transition:border-color .3s}"
".card:hover{border-color:#58a6ff}"
".card .icon{font-size:2rem}"
".card .label{font-size:0.7rem;color:#8b949e;text-transform:uppercase;letter-spacing:1px;text-align:center}"
".card .value{font-size:1.9rem;font-weight:bold;color:#f0f6fc}"
".card .unit{font-size:0.8rem;color:#8b949e}"
".card .sub{font-size:0.72rem;color:#8b949e;text-align:center}"
/* ---- Presence card speciale ---- */
"#pres-card.present{border-color:#3fb950;background:#0e2015}"
"#pres-card.absent{border-color:#484f58}"
"#pres-card .value{font-size:1.5rem}"
/* ---- Panel generico full-width ---- */
".panel{background:#161b22;border:1px solid #30363d;border-radius:14px;"
"padding:1.2rem 1.4rem;width:100%;max-width:520px;margin-bottom:1.2rem}"
".panel-header{display:flex;align-items:center;justify-content:space-between;margin-bottom:0.9rem}"
".panel-title{font-size:0.75rem;color:#8b949e;text-transform:uppercase;letter-spacing:1px}"
".badge{font-size:0.75rem;font-weight:bold;padding:0.2rem 0.7rem;"
"border-radius:20px;border:1px solid currentColor}"
/* ---- Range bar ---- */
".bar-wrap{background:#21262d;border-radius:6px;height:8px;margin:0.5rem 0}"
".bar{height:8px;border-radius:6px;transition:width .6s,background .6s}"
/* ---- Detail row ---- */
".detail{display:flex;justify-content:space-between;flex-wrap:wrap;gap:0.5rem;"
"margin-top:0.8rem;padding-top:0.8rem;border-top:1px solid #21262d}"
".d-item{display:flex;flex-direction:column;align-items:center;gap:0.15rem}"
".d-val{font-size:0.92rem;font-weight:bold;color:#e6edf3}"
".d-lbl{font-size:0.62rem;color:#8b949e;text-transform:uppercase;letter-spacing:0.5px}"
/* ---- WiFi big number ---- */
"#wifi-rssi-big{font-size:3rem;font-weight:bold;text-align:center;color:#58a6ff}"
"#wifi-unit{text-align:center;font-size:0.8rem;color:#8b949e;margin-bottom:0.4rem}"
/* ---- Booleans row ---- */
".bool-row{display:flex;gap:0.8rem;justify-content:center;flex-wrap:wrap;margin:0.5rem 0}"
".bool-pill{padding:0.3rem 0.9rem;border-radius:20px;font-size:0.8rem;font-weight:bold;"
"border:1px solid transparent}"
".bool-pill.on{border-color:#3fb950;color:#3fb950;background:#0e2015}"
".bool-pill.off{border-color:#484f58;color:#484f58;background:#161b22}"
/* ---- Status ---- */
".status{font-size:0.72rem;color:#484f58;margin-top:0.8rem}"
"</style></head><body>"
"<h1>&#x1F4E1; LD2420 Monitor</h1>"
"<nav>"
"  <a href='/monitor' class='active'>&#x1F4CA; Monitor</a>"
"  <a href='/config'>&#x2699;&#xFE0F; Config</a>"
"  <a href='/update'>&#x1F504; OTA</a>"
"</nav>"
"<div id='clock'>--:--:--</div>"
"<div id='uptime'>uptime: --</div>"
"<div id='build-date' style='font-size:0.7rem;color:#484f58;margin-bottom:1rem;letter-spacing:1px'>Build: --</div>"

/* ===== SEZIONE 1: Presenza ===== */
"<div class='section'>"
"<div class='section-title'>&#x1F464; Rilevamento Presenza</div>"
"<div id='pres-card' class='panel'>"
"  <div class='panel-header'>"
"    <span class='panel-title'>Stato</span>"
"    <span class='badge' id='pres-badge' style='color:#484f58'>--</span>"
"  </div>"
"  <div class='bool-row'>"
"    <span class='bool-pill' id='pill-pres'>&#x1F464; Presenza</span>"
"    <span class='bool-pill' id='pill-motion'>&#x1F3C3; Movimento</span>"
"    <span class='bool-pill' id='pill-static'>&#x1FA91; Statica</span>"
"  </div>"
"  <div class='detail'>"
"    <div class='d-item'><span class='d-val' id='v-pres-ms'>--</span><span class='d-lbl'>Da presenza</span></div>"
"    <div class='d-item'><span class='d-val' id='v-abs-ms'>--</span><span class='d-lbl'>Da assenza</span></div>"
"    <div class='d-item'><span class='d-val' id='v-mot-ms'>--</span><span class='d-lbl'>Da moto</span></div>"
"    <div class='d-item'><span class='d-val' id='v-stat-ms'>--</span><span class='d-lbl'>Da statica</span></div>"
"    <div class='d-item'><span class='d-val' id='v-chg-ms'>--</span><span class='d-lbl'>Da cambio</span></div>"
"    <div class='d-item'><span class='d-val' id='v-pres-tot'>--</span><span class='d-lbl'>Presenza Totale</span></div>"
"    <div class='d-item'><span class='d-val' id='v-pres-pct'>--</span><span class='d-lbl'>% Presenza Totale</span></div>"
"  </div>"
"</div>"
"</div>"

/* ===== SEZIONE 2: Range ===== */
"<div class='section'>"
"<div class='section-title'>&#x1F4CF; Range di Rilevamento</div>"
"<div class='grid2'>"
"  <div class='card'>"
"    <span class='icon'>&#x2B05;</span>"
"    <span class='label'>Distanza Min</span>"
"    <span class='value' id='v-min'>--</span>"
"    <span class='unit'>cm</span>"
"  </div>"
"  <div class='card'>"
"    <span class='icon'>&#x27A1;</span>"
"    <span class='label'>Distanza Max</span>"
"    <span class='value' id='v-max'>--</span>"
"    <span class='unit'>cm</span>"
"  </div>"
"</div>"
"</div>"

/* ===== SEZIONE 3: WiFi ===== */
"<div class='panel'>"
"  <div class='panel-header'>"
"    <span class='panel-title'>&#x1F4F6; WiFi</span>"
"    <span class='badge' id='wifi-badge' style='color:#58a6ff'>--</span>"
"  </div>"
"  <div id='wifi-ssid-row' style='text-align:center;margin-bottom:0.6rem;"
"font-size:1rem;font-weight:bold;color:#79c0ff'>--</div>"
"  <div id='wifi-rssi-big'>---</div>"
"  <div id='wifi-unit'>dBm</div>"
"  <div class='bar-wrap'><div class='bar' id='wifi-bar' style='width:0%;background:#484f58'></div></div>"
"  <div style='display:flex;justify-content:space-between;font-size:0.65rem;color:#484f58;margin-bottom:0.4rem'>"
"    <span>-100</span><span>-85</span><span>-70</span><span>-55</span><span>-40</span>"
"  </div>"
"  <div class='detail'>"
"    <div class='d-item'><span class='d-val' id='wifi-quality'>--</span><span class='d-lbl'>Qualità %</span></div>"
"    <div class='d-item'><span class='d-val' id='wifi-ch'>--</span><span class='d-lbl'>Canale</span></div>"
"    <div class='d-item'><span class='d-val' id='wifi-bssid' style='font-size:0.72rem'>--</span><span class='d-lbl'>BSSID</span></div>"
"  </div>"
"</div>"

"<div class='status' id='status'>Connessione...</div>"

"<script>"
/* ----- utility: ms → stringa leggibile ----- */
"function fmtMs(ms){"
"  if(ms===undefined||ms===null)return '--';"
"  const s=Math.floor(ms/1000),m=Math.floor(s/60),h=Math.floor(m/60);"
"  if(h>0)return h+'h '+String(m%60).padStart(2,'0')+'m';"
"  if(m>0)return m+'m '+String(s%60).padStart(2,'0')+'s';"
"  return s+'s '+(ms%1000)+'ms';"
"}"
/* ----- uptime ----- */
"function fmtUptime(u){"
"  const dd=Math.floor(u/86400),hh=Math.floor((u%86400)/3600),"
"  mm=Math.floor((u%3600)/60),ss=u%60;"
"  const p=[];"
"  if(dd>0)p.push(dd+'g');"
"  p.push(String(hh).padStart(2,'0')+'h');"
"  p.push(String(mm).padStart(2,'0')+'m');"
"  p.push(String(ss).padStart(2,'0')+'s');"
"  return p.join(' ');"
"}"
/* ----- pill helper ----- */
"function setPill(id,on,onLabel,offLabel){"
"  const el=document.getElementById(id);"
"  if(!el)return;"
"  el.className='bool-pill '+(on?'on':'off');"
"  if(on&&onLabel)el.textContent=onLabel;"
"  else if(!on&&offLabel)el.textContent=offLabel;"
"}"
/* ----- fetch principale ----- */
"async function fetchData(){"
"  try{"
"    const r=await fetch('/data');"
"    if(!r.ok){document.getElementById('status').textContent='Errore auth';return;}"
"    const d=await r.json();"
/* clock e uptime */
"    document.getElementById('clock').textContent=d.time||'--:--:--';"
"    if(d.uptime_s!==undefined)"
"      document.getElementById('uptime').textContent='uptime: '+fmtUptime(d.uptime_s);"
"    if(d.build_date){document.getElementById('build-date').textContent='Build: '+d.build_date;}"
/* presenza */
"    const pCard=document.getElementById('pres-card');"
"    const badge=document.getElementById('pres-badge');"
"    if(d.presence){"
"      pCard.classList.add('present');pCard.classList.remove('absent');"
"      badge.textContent='PRESENTE';badge.style.color='#3fb950';"
"    }else{"
"      pCard.classList.add('absent');pCard.classList.remove('present');"
"      badge.textContent='ASSENTE';badge.style.color='#484f58';"
"    }"
"    setPill('pill-pres',  d.presence,       '👤 Presenza',   '👤 Presenza');"
"    setPill('pill-motion',d.motion,          '🏃 Movimento',  '🏃 Movimento');"
"    setPill('pill-static',d.static_presence, '🦱 Statica',   '🦱 Statica');"
"   if (d.presence_pct !== undefined) {"
"       document.getElementById('v-pres-pct').textContent = d.presence_pct.toFixed(1) + '%';"
"   }"
"   if (d.presence_total_ms !== undefined) {"
"       document.getElementById('v-pres-tot').textContent = fmtMs(d.presence_total_ms);"
"   }"
/* timing */
"    document.getElementById('v-pres-ms').textContent=fmtMs(d.presence_ms);"
"    document.getElementById('v-abs-ms').textContent=fmtMs(d.absence_ms);"
"    document.getElementById('v-mot-ms').textContent=fmtMs(d.motion_ms);"
"    document.getElementById('v-stat-ms').textContent=fmtMs(d.static_ms);"
"    document.getElementById('v-chg-ms').textContent=fmtMs(d.change_ms);"
/* range */
"    document.getElementById('v-min').textContent=d.min_dist!==undefined?d.min_dist:'--';"
"    document.getElementById('v-max').textContent=d.max_dist!==undefined?d.max_dist:'--';"
/* WiFi */
"    if(d.wifi){"
"      const wf=d.wifi;"
"      const pct=Math.min(100,Math.max(0,(wf.rssi+100)*100/60));"
"      const col=wf.rssi>=-65?'#3fb950':wf.rssi>=-75?'#d29922':wf.rssi>=-85?'#e3700a':'#f85149';"
"      document.getElementById('wifi-ssid-row').textContent=wf.ssid||'--';"
"      document.getElementById('wifi-rssi-big').textContent=wf.rssi;"
"      document.getElementById('wifi-rssi-big').style.color=col;"
"      document.getElementById('wifi-badge').textContent=(wf.icon||'')+' '+wf.label;"
"      document.getElementById('wifi-badge').style.color=col;"
"      document.getElementById('wifi-bar').style.width=pct.toFixed(0)+'%';"
"      document.getElementById('wifi-bar').style.background=col;"
"      document.getElementById('wifi-quality').textContent=wf.quality+'%';"
"      document.getElementById('wifi-ch').textContent=wf.channel;"
"      document.getElementById('wifi-bssid').textContent=wf.bssid||'--';"
"    }"
"    document.getElementById('status').textContent='Aggiornato: '+new Date().toLocaleTimeString();"
"  }catch(e){"
"    document.getElementById('status').textContent='Errore connessione';"
"  }"
"}"
"fetchData();"
"setInterval(fetchData,2000);"
"</script>"
"</body></html>";

static esp_err_t monitor_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_MONITOR, -1);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  NVS — salvataggio credenziali                                      */
/* ------------------------------------------------------------------ */

static bool save_wifi_credentials(const char *ssid, const char *password) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open fallito: %s", esp_err_to_name(err));
        return false;
    }
    bool ok = true;
    if (nvs_set_str(nvs, "ssid",     ssid)     != ESP_OK) ok = false;
    if (ok && nvs_set_str(nvs, "password", password) != ESP_OK) ok = false;
    if (ok && nvs_commit(nvs)                   != ESP_OK) ok = false;
    nvs_close(nvs);
    if (ok) ESP_LOGI(TAG, "Credenziali salvate: SSID='%s'", ssid);
    return ok;
}

/* ------------------------------------------------------------------ */
/*  Task di reboot                                                      */
/* ------------------------------------------------------------------ */

static void reboot_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

/* ------------------------------------------------------------------ */
/*  Pagine HTML config WiFi                                             */
/* ------------------------------------------------------------------ */

static const char *HTML_PAGE =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 WiFi Config</title>"
    "<style>"
    "body{font-family:sans-serif;background:#1a1a2e;color:#eee;display:flex;"
    "justify-content:center;align-items:center;height:100vh;margin:0}"
    ".box{background:#16213e;padding:2rem;border-radius:12px;width:300px;"
    "box-shadow:0 4px 20px rgba(0,0,0,0.5)}"
    "h2{text-align:center;margin-bottom:1.5rem;color:#e94560}"
    "input{width:100%;padding:10px;margin:8px 0;border:none;border-radius:6px;"
    "background:#0f3460;color:#eee;box-sizing:border-box;font-size:14px}"
    "button{width:100%;padding:12px;background:#e94560;color:#fff;border:none;"
    "border-radius:6px;cursor:pointer;font-size:16px;margin-top:1rem}"
    "button:hover{background:#c73652}"
    ".msg{text-align:center;margin-top:1rem;color:#4ecca3}"
    "</style></head><body>"
    "<div class='box'>"
    "<h2>&#x1F4F6; WiFi Config</h2>"
    "<form method='POST' action='/save'>"
    "<input type='text'     name='ssid'     placeholder='Nome rete (SSID)' required><br>"
    "<input type='password' name='password' placeholder='Password WiFi'   required><br>"
    "<button type='submit'>Salva e Riavvia</button>"
    "</form>"
    "</div></body></html>";

static const char *HTML_OK =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Salvato</title>"
    "<style>body{font-family:sans-serif;background:#1a1a2e;color:#eee;"
    "display:flex;justify-content:center;align-items:center;height:100vh}"
    ".box{background:#16213e;padding:2rem;border-radius:12px;text-align:center}"
    "h2{color:#4ecca3}p{color:#aaa}</style></head><body>"
    "<div class='box'><h2>&#x2705; Salvato!</h2>"
    "<p>L'ESP32 si riavvierà e tenterà la connessione.</p></div>"
    "</body></html>";

static const char *HTML_ERR =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<title>Errore</title>"
    "<style>body{font-family:sans-serif;background:#1a1a2e;color:#eee;"
    "display:flex;justify-content:center;align-items:center;height:100vh}"
    ".box{background:#16213e;padding:2rem;border-radius:12px;text-align:center}"
    "h2{color:#e94560}p{color:#aaa}</style></head><body>"
    "<div class='box'><h2>&#x274C; Errore</h2>"
    "<p>Impossibile salvare le credenziali. Riprova.</p></div>"
    "</body></html>";

/* ------------------------------------------------------------------ */
/*  GET /                                                               */
/* ------------------------------------------------------------------ */

static esp_err_t get_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_PAGE, -1);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  POST /save                                                          */
/* ------------------------------------------------------------------ */

static esp_err_t post_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    char body[256] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[len] = '\0';

    char ssid[64] = {0}, password[64] = {0};
    char *p;
    p = strstr(body, "ssid=");
    if (p) {
        p += 5;
        char *end = strchr(p, '&');
        if (end) *end = '\0';
        strncpy(ssid, p, sizeof(ssid) - 1);
        if (end) *end = '&';
    }
    p = strstr(body, "password=");
    if (p) {
        p += 9;
        char *end = strchr(p, '&');
        if (end) *end = '\0';
        strncpy(password, p, sizeof(password) - 1);
    }
    url_decode(ssid);
    url_decode(password);
    ESP_LOGI(TAG, "Ricevuto SSID='%s'", ssid);

    if (!save_wifi_credentials(ssid, password)) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, HTML_ERR, -1);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_OK, -1);
    xTaskCreate(reboot_task, "reboot", 1024, NULL, 5, NULL);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  GET /update  (pagina OTA)                                          */
/* ------------------------------------------------------------------ */

static const char *HTML_OTA =
"<!DOCTYPE html><html lang='it'><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32 OTA Update</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:'Segoe UI',sans-serif;background:#0d1117;color:#e6edf3;"
"display:flex;justify-content:center;align-items:center;min-height:100vh}"
".box{background:#161b22;border:1px solid #30363d;border-radius:14px;"
"padding:2rem;width:100%;max-width:420px}"
"h2{color:#58a6ff;margin-bottom:1.5rem;text-align:center}"
".drop{border:2px dashed #30363d;border-radius:10px;padding:2rem;"
"text-align:center;cursor:pointer;transition:border-color .3s;margin-bottom:1rem}"
".drop:hover,.drop.over{border-color:#58a6ff}"
".drop input{display:none}"
".drop p{color:#8b949e;font-size:0.9rem;margin-top:0.5rem}"
"#filename{color:#58a6ff;font-size:0.85rem;margin-top:0.5rem}"
"button{width:100%;padding:12px;background:#238636;color:#fff;border:none;"
"border-radius:6px;cursor:pointer;font-size:1rem;margin-top:0.5rem}"
"button:disabled{background:#2d333b;color:#484f58;cursor:not-allowed}"
"button:hover:not(:disabled){background:#2ea043}"
"#progress-wrap{background:#21262d;border-radius:6px;height:8px;margin-top:1rem;display:none}"
"#progress-bar{height:8px;background:#58a6ff;border-radius:6px;width:0;transition:width .3s}"
"#status{text-align:center;margin-top:1rem;font-size:0.85rem;color:#8b949e}"
"</style></head><body>"
"<div class='box'>"
"<h2>&#x1F504; OTA Firmware Update</h2>"
"<div class='drop' id='drop' onclick='document.getElementById(\"file\").click()'>"
"  <span style='font-size:2rem'>&#x1F4BE;</span>"
"  <p>Clicca o trascina il file <strong>.bin</strong></p>"
"  <div id='filename'>Nessun file selezionato</div>"
"  <input type='file' id='file' accept='.bin'>"
"</div>"
"<button id='btn' disabled onclick='startUpload()'>Carica Firmware</button>"
"<div id='progress-wrap'><div id='progress-bar'></div></div>"
"<div id='status'></div>"
"</div>"
"<script>"
"const drop=document.getElementById('drop');"
"const fileInput=document.getElementById('file');"
"const btn=document.getElementById('btn');"
"let selectedFile=null;"
"fileInput.addEventListener('change',()=>{"
"  selectedFile=fileInput.files[0];"
"  document.getElementById('filename').textContent=selectedFile?selectedFile.name:'Nessun file';"
"  btn.disabled=!selectedFile;"
"});"
"drop.addEventListener('dragover',e=>{e.preventDefault();drop.classList.add('over');});"
"drop.addEventListener('dragleave',()=>drop.classList.remove('over'));"
"drop.addEventListener('drop',e=>{"
"  e.preventDefault();drop.classList.remove('over');"
"  selectedFile=e.dataTransfer.files[0];"
"  document.getElementById('filename').textContent=selectedFile.name;"
"  btn.disabled=false;"
"});"
"function startUpload(){"
"  if(!selectedFile)return;"
"  btn.disabled=true;"
"  document.getElementById('progress-wrap').style.display='block';"
"  const status=document.getElementById('status');"
"  const bar=document.getElementById('progress-bar');"
"  status.textContent='Upload in corso...';"
"  const xhr=new XMLHttpRequest();"
"  xhr.open('POST','/ota', true);"
"  xhr.setRequestHeader('Content-Type','application/octet-stream');"
"  const auth = btoa('admin:esp32admin');"
"  xhr.setRequestHeader('Authorization', 'Basic ' + auth);"
"  xhr.upload.onprogress=e=>{"
"    if(e.lengthComputable){"
"      const pct=Math.round(e.loaded/e.total*100);"
"      bar.style.width=pct+'%';"
"      status.textContent='Upload: '+pct+'%';"
"    }"
"  };"
"  xhr.onload=()=>{"
"    if(xhr.status===200){"
"      bar.style.width='100%';bar.style.background='#3fb950';"
"      status.textContent='\u2705 Completato! Riavvio in corso...';"
"    }else{"
"      bar.style.background='#f85149';"
"      status.textContent='\u274C Errore: '+xhr.responseText;"
"      btn.disabled=false;"
"    }"
"  };"
"  xhr.onerror=()=>{status.textContent='\u274C Errore di connessione';btn.disabled=false;};"
"  xhr.send(selectedFile);"
"}"
"</script>"
"</body></html>";

static esp_err_t ota_page_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_OTA, -1);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  POST /ota                                                           */
/* ------------------------------------------------------------------ */

static esp_err_t ota_upload_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Partizione OTA non trovata");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA: scrittura su partizione '%s'", update_partition->label);
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin fallito");
        return ESP_FAIL;
    }
    char buf[4096];
    int received = 0, remaining = req->content_len;
    while (remaining > 0) {
        int len = httpd_req_recv(req, buf, MIN(remaining, (int)sizeof(buf)));

        if (len == 0) {
            ESP_LOGE(TAG, "Connessione chiusa");
            break;
        }
        if (len < 0) {
            if (len == HTTPD_SOCK_ERR_TIMEOUT) continue;
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Errore ricezione");
            return ESP_FAIL;
        }
        err = esp_ota_write(ota_handle, buf, len);
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Errore scrittura flash");
            return ESP_FAIL;
        }
        remaining -= len;
        received  += len;
        ESP_LOGI(TAG, "OTA: ricevuto %d / %d bytes", received, req->content_len);
    }
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end fallito");
        return ESP_FAIL;
    }

    esp_app_desc_t new_app_info;

    if (esp_ota_get_partition_description(update_partition, &new_app_info) == ESP_OK) {
        const esp_app_desc_t *running = esp_app_get_description();

        if (strcmp(new_app_info.version, running->version) <= 0) {
            ESP_LOGW(TAG, "Firmware vecchio o uguale → rifiutato");
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Versione non valida");
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Nuovo firmware: %s", new_app_info.version);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Firmware non valido");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition fallito");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA completato! Riavvio...");
    httpd_resp_sendstr(req, "OK - rebooting");
    xTaskCreate(reboot_task, "reboot_ota", 1024, NULL, 5, NULL);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  GET /config                                                         */
/* ------------------------------------------------------------------ */

static const char *HTML_CONFIG =
"<!DOCTYPE html><html lang='it'><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Impostazioni</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:'Segoe UI',sans-serif;background:#0d1117;color:#e6edf3;"
"min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:2rem}"
"h1{font-size:1.4rem;color:#58a6ff;margin-bottom:0.3rem;letter-spacing:2px;text-transform:uppercase}"
"nav{display:flex;gap:0.6rem;margin-bottom:1.5rem;flex-wrap:wrap;justify-content:center}"
"nav a{display:flex;align-items:center;gap:0.4rem;padding:0.4rem 0.9rem;"
"background:#161b22;border:1px solid #30363d;border-radius:8px;"
"color:#8b949e;text-decoration:none;font-size:0.8rem;transition:all .2s}"
"nav a:hover{border-color:#58a6ff;color:#58a6ff}"
"nav a.active{border-color:#58a6ff;color:#58a6ff;background:#1a2233}"
".box{width:100%;max-width:480px;background:#161b22;border:1px solid #30363d;"
"border-radius:14px;padding:1.6rem;margin-bottom:1rem}"
"h2{font-size:1rem;color:#e6edf3;margin-bottom:1.2rem}"
".field{display:flex;flex-direction:column;gap:0.4rem;margin-bottom:1.2rem}"
".field label{font-size:0.75rem;color:#8b949e;text-transform:uppercase;letter-spacing:1px}"
".field input{background:#0d1117;color:#e6edf3;border:1px solid #30363d;border-radius:8px;"
"padding:0.5rem 0.8rem;font-size:1rem;width:100%}"
".field input:focus{outline:none;border-color:#58a6ff}"
".btn-save{width:100%;padding:0.7rem;background:#1a3a5c;color:#58a6ff;"
"border:1px solid #58a6ff;border-radius:8px;cursor:pointer;font-size:0.95rem;font-weight:bold}"
".btn-save:hover{background:#58a6ff;color:#0d1117}"
"#msg{text-align:center;margin-top:0.8rem;font-size:0.85rem;min-height:1.2rem}"
"#msg.ok{color:#3fb950}#msg.err{color:#f85149}"
"</style></head><body>"
"<h1>&#x2699;&#xFE0F; Impostazioni</h1>"
"<nav>"
"  <a href='/monitor'>&#x1F4CA; Monitor</a>"
"  <a href='/config' class='active'>&#x2699;&#xFE0F; Config</a>"
"  <a href='/update'>&#x1F504; OTA</a>"
"</nav>"
"<div class='box'>"
"<h2>Configurazione WiFi</h2>"
"<p style='color:#8b949e;font-size:0.85rem'>Per cambiare rete, usare la pagina di configurazione iniziale (<a href='/' style='color:#58a6ff'>/</a>).</p>"
"</div>"
"</body></html>";

static esp_err_t config_page_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_CONFIG, -1);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  POST /api/config                                                   */
/* ------------------------------------------------------------------ */

static esp_err_t api_config_post_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    char body[64] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[len] = '\0';
    int alt = -1;
    char *p = strstr(body, "\"altitude\"");
    if (p) { p = strchr(p, ':'); if (p) sscanf(p + 1, "%d", &alt); }
    if (alt < 0 || alt > 5000) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Altitudine non valida");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  http_server_start / stop                                            */
/* ------------------------------------------------------------------ */

bool http_server_start(void) {
    if (!s_data_mutex) {
        s_data_mutex = xSemaphoreCreateMutex();
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable  = true;
    config.max_uri_handlers  = 15;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    config.stack_size        = 8192;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Errore avvio server");
        return false;
    }

    httpd_uri_t uris[] = {
        { .uri="/",           .method=HTTP_GET,  .handler=get_handler            },
        { .uri="/save",       .method=HTTP_POST, .handler=post_handler           },
        { .uri="/monitor",    .method=HTTP_GET,  .handler=monitor_handler        },
        { .uri="/data",       .method=HTTP_GET,  .handler=data_handler           },
        { .uri="/favicon.ico",.method=HTTP_GET,  .handler=favicon_handler        },
        { .uri="/update",     .method=HTTP_GET,  .handler=ota_page_handler       },
        { .uri="/ota",        .method=HTTP_POST, .handler=ota_upload_handler     },
        { .uri="/config",     .method=HTTP_GET,  .handler=config_page_handler    },
        { .uri="/api/config", .method=HTTP_POST, .handler=api_config_post_handler},
    };

    for (int i = 0; i < (int)(sizeof(uris)/sizeof(uris[0])); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    ESP_LOGI(TAG, "Server HTTP avviato — /monitor  /data  /config  /update");
    return true;
}

void http_server_stop(void) {
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
}

void get_time_str(char *buf, size_t len) {
    time_t    now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(buf, len, "%d/%m/%Y %H:%M:%S", &timeinfo);
}