#include <inttypes.h>
#include <string.h>

#include "main.h"

extern bool wifi_connected;

static ld2420_state_t s_sensor_state;
static uint32_t      s_min_dist   = 0;
static uint32_t      s_max_dist   = 0;
static uint64_t total_presence_ms = 0;
static uint64_t total_absence_ms  = 0;
static uint32_t last_update_ms    = 0;
static uint64_t total_moto_ms = 0;
static uint64_t total_static_ms = 0;

static const char *TAG = "MAIN";

void http_server_update_data(const char *time_str, ld2420_state_t state, uint32_t min, uint32_t max);

void app_main(void)
{
    ld2420_config_t cfg = {
        .pin_ot1   = GPIO_NUM_16,
        .pin_ot2   = GPIO_NUM_4,
        .uart_num  = UART_NUM_2,
        .uart_tx   = GPIO_NUM_17,
        .uart_baud = 256000,
    };

    char ssid[64]     = {0};
    char password[64] = {0};

    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Firmware: %s", app_desc->project_name);
    ESP_LOGI(TAG, "Versione: %s", app_desc->version);
    ESP_LOGI(TAG, "Compilato: %s %s", app_desc->date, app_desc->time);
    ESP_LOGI(TAG, "IDF: %s", app_desc->idf_ver);
    ESP_LOGI(TAG, "Build CMake: %s", BIN_BUILD_DATE);
    ESP_LOGI(TAG, "========================================");

    ld2420_init(&cfg);
    vTaskDelay(pdMS_TO_TICKS(500));
    ld2420_apply_default_config();
    vTaskDelay(pdMS_TO_TICKS(500));
    // ld2420_exit_engineering_mode();
    // vTaskDelay(pdMS_TO_TICKS(200));
    ld2420_task_start(5, 4096);
    vTaskDelay(pdMS_TO_TICKS(200));

    wifi_init();

    if (wifi_load_credentials(ssid, sizeof(ssid), password, sizeof(password))) {
        ESP_LOGI(TAG, "Credenziali trovate: SSID='%s'", ssid);
        wifi_connect(ssid, password);
    } else {
        ESP_LOGW(TAG, "Nessuna credenziale salvata");
    }

    if (wifi_connected) {
        time_sync_init();
        if (http_server_start())
        {
            vTaskDelay(pdMS_TO_TICKS(5000));
            ESP_LOGI(TAG, "Sistema stabile → confermo OTA");
            // esp_ota_mark_app_valid_cancel_rollback();
        }
    } else {
        wifi_start_ap();
        http_server_start();
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    while (1) {
        // putchar('.');
        ld2420_print_status_line();
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void ld2420_print_status_line(void)
{
    return; // --- IGNORE ---
    ld2420_state_t s = ld2420_get_state();

    /* --- Lettura info WiFi --- */
    char    wifi_ssid[33]  = "--";
    char    wifi_bssid[18] = "--";
    int8_t  wifi_rssi      = 0;
    uint8_t wifi_channel   = 0;
    int     wifi_quality   = 0;   /* 0-100 % */
    const char *wifi_label = "N/A";

    wifi_ap_record_t ap_info = {0};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        strncpy(wifi_ssid, (char *)ap_info.ssid, sizeof(wifi_ssid) - 1);
        wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';

        snprintf(wifi_bssid, sizeof(wifi_bssid),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 ap_info.bssid[0], ap_info.bssid[1], ap_info.bssid[2],
                 ap_info.bssid[3], ap_info.bssid[4], ap_info.bssid[5]);

        wifi_rssi    = ap_info.rssi;
        wifi_channel = ap_info.primary;

        /* RSSI → percentuale: -100 dBm = 0%, -40 dBm = 100% */
        int q = (wifi_rssi + 100) * 100 / 60;
        if (q < 0)   q = 0;
        if (q > 100) q = 100;
        wifi_quality = q;

        if      (wifi_rssi >= -50) wifi_label = "Ottimo";
        else if (wifi_rssi >= -65) wifi_label = "Buono";
        else if (wifi_rssi >= -75) wifi_label = "Discreto";
        else if (wifi_rssi >= -85) wifi_label = "Scarso";
        else                       wifi_label = "Pessimo";
    }

    printf(
        "uptime=%" PRIu32 "ms "
        "presence=%d "
        "presence_ms=%" PRIu32 " "
        "absence_ms=%" PRIu32 " "
        "motion_ms=%" PRIu32 " "
        "static_ms=%" PRIu32 " "
        "dynamic_ms=%" PRIu32 " "
        "change_ms=%" PRIu32 " "
        "min=%u "
        "max=%u "
        "sens=%u "
        "wifi_ssid=%s "
        "wifi_rssi=%d "
        "wifi_quality=%d%% "
        "wifi_label=%s "
        "wifi_ch=%u "
        "wifi_bssid=%s\n",
        ld2420_get_uptime_ms(),
        s.presence,
        ld2420_ms_since_presence(),
        ld2420_ms_since_absence(),
        ld2420_ms_since_motion(),
        ld2420_ms_since_static_presence(),
        ld2420_ms_since_dynamic_presence(),
        ld2420_ms_since_state_change(),
        ld2420_get_min_distance(),
        ld2420_get_max_distance(),
        ld2420_get_sensitivity(),
        wifi_ssid,
        (int)wifi_rssi,
        wifi_quality,
        wifi_label,
        wifi_channel,
        wifi_bssid
    );
}


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
/*  GET /data  (JSON — include SSID e qualità WiFi)                    */
/* ------------------------------------------------------------------ */

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
        
        if (st.motion)
            total_moto_ms += delta;
        
        if (st.static_presence)
            total_static_ms += delta;
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

        "\"total_moto_ms\":%" PRIu64 ","
        "\"total_static_ms\":%" PRIu64 ","

        "\"presence_total_ms\":%" PRIu64 ","
        "\"absence_total_ms\":%" PRIu64 ","
        "\"presence_pct\":%.2f,"

        "\"min_dist\":%lu,"
        "\"max_dist\":%lu,"

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

        total_moto_ms,
        total_static_ms,

        total_presence_ms,
        total_absence_ms,
        presence_pct,

        d_min,
        d_max,

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
"    <div class='d-item'><span class='d-val' id='v-moto-tot'>--</span><span class='d-lbl'>Motion Totale</span></div>"
"    <div class='d-item'><span class='d-val' id='v-static-tot'>--</span><span class='d-lbl'>Static Totale</span></div>"
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
"   if (d.total_moto_ms !== undefined) {"
"       document.getElementById('v-moto-tot').textContent = fmtMs(d.total_moto_ms);"
"   }"
"   if (d.total_static_ms !== undefined) {"
"       document.getElementById('v-static-tot').textContent = fmtMs(d.total_static_ms);"
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

esp_err_t monitor_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_MONITOR, -1);
    return ESP_OK;
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



    for (int i = 0; i < (int)(sizeof(uris)/sizeof(uris[0])); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    ESP_LOGI(TAG, "Server HTTP avviato — /monitor  /data  /config  /update");
    return true;
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

void http_server_stop(void) {
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
}