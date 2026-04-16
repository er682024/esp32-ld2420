// #include "esp_app_format.h"
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
/*  Dati condivisi — protetti da mutex contro race condition           */
/*  (sensor_task scrive, HTTP handler legge da task diversi)          */
/* ------------------------------------------------------------------ */

static SemaphoreHandle_t s_data_mutex = NULL;

static bool     s_data_valid = false;   /* true dopo la prima lettura sensore */
static char     s_time[32] = "--:--:--";

static ld2420_state_t s_sensor_state;
static uint32_t s_min_dist = 0;
static uint32_t s_max_dist = 0;

void http_server_update_data(const char *time_str, ld2420_state_t state, uint32_t min, uint32_t max) {
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
/*  Gestisce sia %XX sia + → spazio (application/x-www-form-urlencoded)*/
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
    /* Buffer abbastanza grande da contenere la decodifica + '\0'
       senza rischio di overflow di 1 byte. */
    unsigned char decoded[65] = {0};
    size_t decoded_len = 0;

    mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                          (unsigned char *)encoded, strlen(encoded));
    decoded[decoded_len] = '\0';

    char expected[64];
    snprintf(expected, sizeof(expected), "%s:%s", ADMIN_USER, ADMIN_PASS);

    if (strcmp((char *)decoded, expected) != 0) {
        /* Non loggare le credenziali ricevute — potrebbero contenere
           la password vera dell'utente o dati sensibili. */
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
/*  GET /data  (JSON)                                                   */
/* ------------------------------------------------------------------ */

static esp_err_t data_handler(httpd_req_t *req) {
    char json[1950];
    char     ts[32];
    ld2420_state_t st;
    uint32_t d_min, d_max;

    if (s_data_mutex && xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(ts, s_time, sizeof(ts));
        st = s_sensor_state;
        d_min = s_min_dist;
        d_max = s_max_dist;
        xSemaphoreGive(s_data_mutex);
    } else {
        strncpy(ts, "--:--:--", sizeof(ts));
        memset(&st, 0, sizeof(st));
        d_min = d_max = 0;
    }

    /* Termostato */
    time_t now_t = time(NULL);
    struct tm ti;
    localtime_r(&now_t, &ti);
    // uint8_t cur_h = (uint8_t)ti.tm_hour;
    // uint8_t cur_m = (uint8_t)ti.tm_min;

    /* WiFi RSSI e canale */
    wifi_ap_record_t ap_info = {0};
    int8_t  rssi    = 0;
    uint8_t channel = 0;
    char    bssid_str[18] = "--";
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi    = ap_info.rssi;
        channel = ap_info.primary;
        snprintf(bssid_str, sizeof(bssid_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 ap_info.bssid[0], ap_info.bssid[1], ap_info.bssid[2],
                 ap_info.bssid[3], ap_info.bssid[4], ap_info.bssid[5]);
    }

    /* Qualità segnale da RSSI */
    const char *rssi_label;
    const char *rssi_icon;
    if      (rssi >= -50) { rssi_label = "Ottimo";   rssi_icon = "\U0001F4F6"; }
    else if (rssi >= -65) { rssi_label = "Buono";    rssi_icon = "\U0001F4F6"; }
    else if (rssi >= -75) { rssi_label = "Discreto"; rssi_icon = "\U0001F4F5"; }
    else if (rssi >= -85) { rssi_label = "Scarso";   rssi_icon = "\U0001F4F5"; }
    else                  { rssi_label = "Pessimo";  rssi_icon = "⚠️"; }

    snprintf(json, sizeof(json),
        "{"
        "\"time\":\"%s\","
        "\"presence\":%d,"
        "\"min_dist\":%lu,"
        "\"max_dist\":%lu,"
        "\"uptime_s\":%lld,"
        "\"wifi\":{\"rssi\":%d, \"label\":\"%s\", \"icon\":\"%s\",\"channel\":\"%d\"}"
        "}",
        ts, st.presence, d_min, d_max,
        (long long)(esp_timer_get_time() / 1000000LL),
        rssi, rssi_label, rssi_icon, channel);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json, -1);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  GET /monitor  (HTML dashboard)                                      */
/* ------------------------------------------------------------------ */

static const char *HTML_MONITOR =
"<!DOCTYPE html><html lang='it'><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32 LD2420 Monitor</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:'Segoe UI',sans-serif;background:#0d1117;color:#e6edf3;min-height:100vh;"
"display:flex;flex-direction:column;align-items:center;padding:2rem}"
"h1{font-size:1.4rem;color:#58a6ff;margin-bottom:0.3rem;letter-spacing:2px;text-transform:uppercase}"
"#clock{font-size:2.2rem;font-weight:bold;color:#f0f6fc;margin-bottom:0.4rem;letter-spacing:3px}"
"#uptime{font-size:0.75rem;color:#484f58;margin-bottom:1.6rem;letter-spacing:1px}"
".grid{display:grid;grid-template-columns:repeat(2,1fr);gap:1.2rem;width:100%;max-width:480px}"
".card{background:#161b22;border:1px solid #30363d;border-radius:14px;padding:1.4rem;"
"display:flex;flex-direction:column;align-items:center;gap:0.4rem;transition:border-color .3s}"
".card:hover{border-color:#58a6ff}"
".icon{font-size:2.2rem}"
".label{font-size:0.75rem;color:#8b949e;text-transform:uppercase;letter-spacing:1px}"
".value{font-size:2rem;font-weight:bold;color:#f0f6fc}"
".unit{font-size:0.85rem;color:#8b949e}"
".sublabel{font-size:0.7rem;color:#8b949e;text-align:center}"
"#temp .value{color:#ff7b72}"
"#hum  .value{color:#79c0ff}"
"#pres .value{color:#a5d6ff}"
"#gas  .value{color:#7ee787}"
".status{margin-top:1.5rem;font-size:0.75rem;color:#484f58}"
"nav{display:flex;gap:0.6rem;margin-bottom:1.5rem;flex-wrap:wrap;justify-content:center}"
"nav a{display:flex;align-items:center;gap:0.4rem;padding:0.4rem 0.9rem;"
"background:#161b22;border:1px solid #30363d;border-radius:8px;"
"color:#8b949e;text-decoration:none;font-size:0.8rem;transition:all .2s}"
"nav a:hover{border-color:#58a6ff;color:#58a6ff}"
"nav a.active{border-color:#58a6ff;color:#58a6ff;background:#1a2233}"
"background:#161b22;border:1px solid #30363d;border-radius:14px;"
"padding:1.2rem 1.4rem;display:flex;align-items:center;justify-content:space-between;gap:1rem}"
"font-weight:bold;text-transform:uppercase}"
".btn-mode{font-size:0.72rem;padding:0.25rem 0.7rem;border-radius:6px;cursor:pointer;"
"font-weight:bold;text-transform:uppercase;border:1px solid #58a6ff;background:#1a2233;color:#58a6ff}"
".btn-mode:hover{background:#58a6ff;color:#0d1117}"
".btn-mode.manual{border-color:#d29922;background:#2a2010;color:#d29922}"
".btn-mode.manual:hover{background:#d29922;color:#0d1117}"
".btn-relay-on{font-size:0.72rem;padding:0.25rem 0.7rem;border-radius:6px;cursor:pointer;"
"font-weight:bold;border:1px solid #3fb950;background:#1a3a1a;color:#3fb950}"
".btn-relay-on:hover{background:#3fb950;color:#0d1117}"
".btn-relay-off{font-size:0.72rem;padding:0.25rem 0.7rem;border-radius:6px;cursor:pointer;"
"font-weight:bold;border:1px solid #f85149;background:#3a1a1a;color:#f85149}"
".btn-relay-off:hover{background:#f85149;color:#fff}"
"#iaq-panel{width:100%;max-width:480px;margin-top:1.2rem;"
"background:#161b22;border:1px solid #30363d;border-radius:14px;padding:1.2rem 1.4rem}"
"#iaq-panel .iaq-header{display:flex;align-items:center;justify-content:space-between;margin-bottom:0.6rem}"
"#iaq-panel .iaq-title{font-size:0.75rem;color:#8b949e;text-transform:uppercase;letter-spacing:1px}"
"#iaq-panel .iaq-badge{font-size:0.75rem;font-weight:bold;padding:0.2rem 0.7rem;border-radius:20px;border:1px solid currentColor}"
"#iaq-panel .iaq-value{font-size:3rem;font-weight:bold;text-align:center;line-height:1.1}"
"#iaq-panel .iaq-level{text-align:center;font-size:1rem;margin-bottom:0.6rem}"
"#iaq-panel .iaq-bar-wrap{background:#21262d;border-radius:6px;height:8px;margin:0.4rem 0}"
"#iaq-panel .iaq-bar{height:8px;border-radius:6px;transition:width .8s,background .8s}"
"#iaq-panel .iaq-labels{display:flex;justify-content:space-between;font-size:0.62rem;color:#484f58}"
"#iaq-panel .iaq-detail{display:flex;justify-content:space-between;flex-wrap:wrap;gap:0.4rem;"
"margin-top:0.8rem;padding-top:0.8rem;border-top:1px solid #21262d;font-size:0.78rem;color:#8b949e}"
"#iaq-panel .iaq-detail .d-item{display:flex;flex-direction:column;align-items:center;gap:0.1rem}"
"#iaq-panel .iaq-detail .d-val{font-size:0.95rem;font-weight:bold;color:#e6edf3}"
"#iaq-panel .iaq-detail .d-lbl{font-size:0.65rem;text-transform:uppercase;letter-spacing:0.5px}"
"#iaq-warmup{text-align:center;font-size:0.78rem;color:#8b949e;margin-top:0.5rem}"
"#wifi-panel{width:100%;max-width:480px;margin-top:1.2rem;"
"background:#161b22;border:1px solid #30363d;border-radius:14px;padding:1.2rem 1.4rem}"
"#wifi-panel .wifi-header{display:flex;align-items:center;justify-content:space-between;margin-bottom:0.8rem}"
"#wifi-panel .wifi-title{font-size:0.75rem;color:#8b949e;text-transform:uppercase;letter-spacing:1px}"
"#wifi-panel .wifi-badge{font-size:0.75rem;font-weight:bold;padding:0.2rem 0.7rem;border-radius:20px;border:1px solid currentColor}"
"#wifi-panel .wifi-rssi{font-size:2.8rem;font-weight:bold;text-align:center;color:#58a6ff}"
"#wifi-panel .wifi-unit{text-align:center;font-size:0.8rem;color:#8b949e;margin-bottom:0.4rem}"
"#wifi-panel .wifi-bar-wrap{background:#21262d;border-radius:6px;height:8px;margin:0.4rem 0}"
"#wifi-panel .wifi-bar{height:8px;border-radius:6px;transition:width .8s,background .8s}"
"#wifi-panel .wifi-detail{display:flex;justify-content:space-between;flex-wrap:wrap;gap:0.4rem;"
"margin-top:0.8rem;padding-top:0.8rem;border-top:1px solid #21262d}"
"#wifi-panel .wifi-detail .d-item{display:flex;flex-direction:column;align-items:center;gap:0.1rem}"
"#wifi-panel .wifi-detail .d-val{font-size:0.9rem;font-weight:bold;color:#e6edf3}"
"#wifi-panel .wifi-detail .d-lbl{font-size:0.65rem;color:#8b949e;text-transform:uppercase;letter-spacing:0.5px}"
"#comfort-panel{width:100%;max-width:480px;margin-top:1.2rem;"
"background:#161b22;border:1px solid #30363d;border-radius:14px;padding:1.2rem 1.4rem}"
"#comfort-panel .cp-title{font-size:0.75rem;color:#8b949e;text-transform:uppercase;"
"letter-spacing:1px;margin-bottom:0.8rem}"
"#comfort-panel .cp-grid{display:grid;grid-template-columns:1fr 1fr;gap:0.8rem}"
"#comfort-panel .cp-item{background:#0d1117;border-radius:10px;padding:0.7rem 0.9rem;"
"display:flex;flex-direction:column;gap:0.2rem}"
"#comfort-panel .cp-lbl{font-size:0.65rem;color:#8b949e;text-transform:uppercase;letter-spacing:0.5px}"
"#comfort-panel .cp-val{font-size:1.1rem;font-weight:bold;color:#e6edf3}"
"#comfort-panel .cp-sub{font-size:0.75rem;color:#8b949e}"
"#zambretti-panel{width:100%;max-width:480px;margin-top:1.2rem;"
"background:#161b22;border:1px solid #30363d;border-radius:14px;padding:1.2rem 1.4rem}"
"#zambretti-panel .zp-header{display:flex;justify-content:space-between;align-items:center;"
"margin-bottom:0.6rem}"
"#zambretti-panel .zp-title{font-size:0.75rem;color:#8b949e;text-transform:uppercase;letter-spacing:1px}"
"#zambretti-panel .zp-letter{font-size:2rem;font-weight:bold;color:#79c0ff;"
"width:2.4rem;height:2.4rem;display:flex;align-items:center;justify-content:center;"
"background:#1a2233;border-radius:8px;border:1px solid #58a6ff}"
"#zambretti-panel .zp-forecast{font-size:1.1rem;font-weight:600;color:#e6edf3;margin-bottom:0.6rem}"
"#zambretti-panel .zp-footer{display:flex;justify-content:space-between;align-items:center}"
"#zambretti-panel .zp-rel{font-size:0.75rem;color:#8b949e}"
"#zambretti-panel .zp-note{font-size:0.68rem;color:#484f58;font-style:italic}"
"</style></head><body>"
"<h1>&#x1F4E1; LD2420 Monitor</h1>"
"<nav>"
"  <a href='/monitor' class='active'>&#x1F4CA; Monitor</a>"
"  <a href='/schedule'>&#x1F552; Fasce</a>"
"  <a href='/config'>&#x1F3D4;&#xFE0F; Config</a>"
"  <a href='/update'>&#x1F504; OTA</a>"
"</nav>"
"<div id='clock'>--:--:--</div>"
"<div id='uptime'>uptime: --</div>"
"<div class='grid'>"
"  <div class='card' id='presence-card'>"
"    <span class='icon' id='v-icon'>👤</span>"
"    <span class='label'>Stato Presenza</span>"
"    <span class='value' id='v-presence'>--</span>"
"    <span class='unit' id='v-presence-text'>Inizializzazione</span>"
"  </div>"
"  <div class='card'>"
"    <span class='icon'>📏</span>"
"    <span class='label'>Range Rilevamento</span>"
"     <span class='value' id='v-range'>--</span>"
"    <span class='unit'>cm (Min-Max)</span>"
"</div>"
"  <div class='card' id='temp'>"
"    <span class='icon'>&#x1F321;</span>"
"    <span class='label'>Temperatura</span>"
"    <span class='value' id='v-temp'>--.-</span>"
"    <span class='unit'>&deg;C</span>"
"    <span class='sublabel' id='v-heat-index' style='color:#ff7b72'></span>"
"    <span class='sublabel' id='v-comfort-temp'></span>"
"  </div>"
"  <div class='card' id='hum'>"
"    <span class='icon'>&#x1F4A7;</span>"
"    <span class='label'>Umidità</span>"
"    <span class='value' id='v-hum'>--.-</span>"
"    <span class='unit'>%</span>"
"    <span class='sublabel' id='v-dew-point'></span>"
"    <span class='sublabel' id='v-abs-hum'></span>"
"  </div>"
"  <div class='card' id='pres'>"
"    <span class='icon' id='p-icon'>&#x1F30D;</span>"
"    <span class='label'>Pressione</span>"
"    <span class='value' id='v-pres'>----</span>"
"    <span class='unit'>hPa &nbsp;<span id='p-trend-icon'></span></span>"
"    <span class='sublabel' id='p-state'>--</span>"
"    <span class='sublabel' id='p-trend'>--</span>"
"    <span class='sublabel' id='p-qnh' style='color:#484f58'></span>"
"    <span class='sublabel' id='p-zambretti' style='color:#79c0ff'></span>"
"  </div>"
"  <div class='card' id='gas'>"
"    <span class='icon'>&#x1F32C;</span>"
"    <span class='label'>Gas</span>"
"    <span class='value' id='v-gas'>----</span>"
"    <span class='unit'>&Omega;</span>"
"  </div>"
"</div>"
"<div id='iaq-panel'>"
"  <div class='iaq-header'>"
"    <span class='iaq-title'>&#x1F32C;&#xFE0F; Qualit&agrave; Aria (IAQ)</span>"
"    <span class='iaq-badge' id='iaq-badge' style='color:#484f58'>--</span>"
"  </div>"
"  <div class='iaq-value' id='iaq-value' style='color:#484f58'>---</div>"
"  <div class='iaq-level' id='iaq-level'>Calibrazione in corso...</div>"
"  <div class='iaq-bar-wrap'><div class='iaq-bar' id='iaq-bar' style='width:0%;background:#484f58'></div></div>"
"  <div class='iaq-labels'><span>Ottima</span><span>Buona</span><span>Discreta</span><span>Scarsa</span><span>Pessima</span></div>"
"  <div class='iaq-detail'>"
"    <div class='d-item'><span class='d-val' id='iaq-gas-raw'>---</span><span class='d-lbl'>Gas (&Omega;)</span></div>"
"    <div class='d-item'><span class='d-val' id='iaq-baseline'>---</span><span class='d-lbl'>Baseline (&Omega;)</span></div>"
"    <div class='d-item'><span class='d-val' id='iaq-calibrated'>--</span><span class='d-lbl'>Stato</span></div>"
"  </div>"
"  <div id='iaq-warmup'></div>"
"</div>"
"<div id='wifi-panel'>"
"  <div class='wifi-header'>"
"    <span class='wifi-title'>&#x1F4F6; Segnale WiFi</span>"
"    <span class='wifi-badge' id='wifi-badge' style='color:#58a6ff'>--</span>"
"  </div>"
"  <div class='wifi-rssi' id='wifi-rssi'>---</div>"
"  <div class='wifi-unit'>dBm</div>"
"  <div class='wifi-bar-wrap'><div class='wifi-bar' id='wifi-bar' style='width:0%;background:#484f58'></div></div>"
"  <div class='wifi-detail'>"
"    <div class='d-item'><span class='d-val' id='wifi-ch'>--</span><span class='d-lbl'>Canale</span></div>"
"    <div class='d-item'><span class='d-val' id='wifi-bssid'>--</span><span class='d-lbl'>BSSID</span></div>"
"  </div>"
"</div>"
"<div id='comfort-panel'>"
"  <div class='cp-title'>&#x1F9EC; Comfort Termico</div>"
"  <div class='cp-grid'>"
"    <div class='cp-item'>"
"      <span class='cp-lbl'>Temp. percepita</span>"
"      <span class='cp-val' id='cp-hi'>--.-&deg;C</span>"
"      <span class='cp-sub'>Heat Index</span>"
"    </div>"
"    <div class='cp-item'>"
"      <span class='cp-lbl'>Punto di rugiada</span>"
"      <span class='cp-val' id='cp-dp'>--.-&deg;C</span>"
"      <span class='cp-sub' id='cp-condensa'>--</span>"
"    </div>"
"    <div class='cp-item'>"
"      <span class='cp-lbl'>Indice Thom</span>"
"      <span class='cp-val' id='cp-thom'>--.-</span>"
"      <span class='cp-sub' id='cp-thom-str'>--</span>"
"    </div>"
"    <div class='cp-item'>"
"      <span class='cp-lbl'>Umidit&agrave; assoluta</span>"
"      <span class='cp-val' id='cp-ah'>-- g/m&sup3;</span>"
"      <span class='cp-sub' id='cp-comfort'>--</span>"
"    </div>"
"  </div>"
"</div>"
"<div id='zambretti-panel'>"
"  <div class='zp-header'>"
"    <span class='zp-title'>&#x1F52E; Previsione Zambretti</span>"
"    <span class='zp-letter' id='zp-letter'>?</span>"
"  </div>"
"  <div class='zp-forecast' id='zp-forecast'>In attesa dati...</div>"
"  <div class='zp-footer'>"
"    <span class='zp-rel' id='zp-rel'></span>"
"    <span class='zp-note'>previsione ~12h</span>"
"  </div>"
"</div>"
"<div class='status' id='status'>Connessione...</div>"
"<script>"
"async function fetchData(){"
"  try{"
"    const r=await fetch('/data');"
"    if(!r.ok){document.getElementById('status').textContent='Errore auth';return;}"
"    const d=await r.json();"
"    document.getElementById('clock').textContent  = d.time;"
"    if(d.uptime_s!==undefined){"
"      const u=d.uptime_s;"
"      const dd=Math.floor(u/86400);"
"      const hh=Math.floor((u%86400)/3600);"
"      const mm=Math.floor((u%3600)/60);"
"      const ss=u%60;"
"      const parts=[];"
"      if(dd>0)parts.push(dd+'g');"
"      parts.push(String(hh).padStart(2,'0')+'h');"
"      parts.push(String(mm).padStart(2,'0')+'m');"
"      parts.push(String(ss).padStart(2,'0')+'s');"
"      document.getElementById('uptime').textContent='uptime: '+parts.join(' ');"
"    }"
"    document.getElementById('v-temp').textContent =d.temperature.toFixed(1);"
"    document.getElementById('v-hum').textContent  =d.humidity.toFixed(1);"
"    if(d.comfort){"
"      const co=d.comfort;"
"      document.getElementById('v-heat-index').textContent='Percepita: '+co.heat_index.toFixed(1)+'\u00B0C';"
"      document.getElementById('v-comfort-temp').textContent=co.level_icon+' '+co.level;"
"      document.getElementById('v-dew-point').textContent='Rugiada: '+co.dew_point.toFixed(1)+'\u00B0C';"
"      document.getElementById('v-abs-hum').textContent=co.abs_humidity.toFixed(1)+' g/m\u00B3';"
"      document.getElementById('cp-hi').innerHTML=co.heat_index.toFixed(1)+'&deg;C';"
"      document.getElementById('cp-dp').innerHTML=co.dew_point.toFixed(1)+'&deg;C';"
"      document.getElementById('cp-condensa').textContent=co.condensa_icon+' '+co.condensa_str;"
"      document.getElementById('cp-thom').textContent=co.thom.toFixed(1);"
"      document.getElementById('cp-thom-str').textContent=co.thom_icon+' '+co.thom_str+(co.thom_note?' \u2014 '+co.thom_note:'');"
"      document.getElementById('cp-ah').textContent=co.abs_humidity.toFixed(1)+' g/m\u00B3';"
"      document.getElementById('cp-comfort').textContent=co.level_icon+' '+co.level;"
"      document.getElementById('cp-hi').style.color=co.color;"
"      document.getElementById('cp-thom').style.color=co.color;"
"    }"
"    document.getElementById('status').textContent ='Aggiornato: '+new Date().toLocaleTimeString();"
/* WiFi */
"    if(d.wifi){"
"      const wf=d.wifi;"
"      const rssiPct=Math.min(100,Math.max(0,(wf.rssi+100)*2));"
"      const col=wf.rssi>=-65?'#3fb950':wf.rssi>=-75?'#d29922':wf.rssi>=-85?'#e3700a':'#f85149';"
"      document.getElementById('wifi-rssi').textContent=wf.rssi;"
"      document.getElementById('wifi-rssi').style.color=col;"
"      document.getElementById('wifi-badge').textContent=wf.icon+' '+wf.label;"
"      document.getElementById('wifi-badge').style.color=col;"
"      document.getElementById('wifi-bar').style.width=rssiPct+'%';"
"      document.getElementById('wifi-bar').style.background=col;"
"      document.getElementById('wifi-ch').textContent=wf.channel;"
"      document.getElementById('wifi-bssid').textContent=wf.bssid;"
"    }"
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
/*  NVS — salvataggio credenziali con gestione errori                  */
/* ------------------------------------------------------------------ */

static bool save_wifi_credentials(const char *ssid, const char *password) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open fallito: %s", esp_err_to_name(err));
        return false;
    }

    bool ok = true;
    if (nvs_set_str(nvs, "ssid", ssid) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(ssid) fallito");
        ok = false;
    }
    if (ok && nvs_set_str(nvs, "password", password) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(password) fallito");
        ok = false;
    }
    if (ok && nvs_commit(nvs) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit fallito");
        ok = false;
    }

    nvs_close(nvs);
    if (ok) ESP_LOGI(TAG, "Credenziali salvate: SSID='%s'", ssid);
    return ok;
}

/* ------------------------------------------------------------------ */
/*  Task di reboot — delegato fuori dall'HTTP handler                  */
/* ------------------------------------------------------------------ */

static void reboot_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

/* ------------------------------------------------------------------ */
/*  Pagine HTML configurazione WiFi                                     */
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
    if (len <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
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
        /* end non viene ripristinato — password è l'ultimo campo */
    }

    /* Decodifica completa %XX e + → spazio */
    url_decode(ssid);
    url_decode(password);

    ESP_LOGI(TAG, "Ricevuto SSID='%s'", ssid);

    if (!save_wifi_credentials(ssid, password)) {
        /* Salvataggio fallito: informa l'utente senza riavviare */
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, HTML_ERR, -1);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_OK, -1);

    /* Reboot delegato a un task separato — l'handler ritorna subito
       e il server può completare l'invio della risposta al browser. */
    xTaskCreate(reboot_task, "reboot", 1024, NULL, 5, NULL);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  GET /update  (pagina upload OTA)                                   */
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
"const drop = document.getElementById('drop');"
"const fileInput = document.getElementById('file');"
"const btn = document.getElementById('btn');"
"let selectedFile = null;"
"fileInput.addEventListener('change', () => {"
"  selectedFile = fileInput.files[0];"
"  document.getElementById('filename').textContent = selectedFile ? selectedFile.name : 'Nessun file';"
"  btn.disabled = !selectedFile;"
"});"
"drop.addEventListener('dragover', e => { e.preventDefault(); drop.classList.add('over'); });"
"drop.addEventListener('dragleave', () => drop.classList.remove('over'));"
"drop.addEventListener('drop', e => {"
"  e.preventDefault(); drop.classList.remove('over');"
"  selectedFile = e.dataTransfer.files[0];"
"  document.getElementById('filename').textContent = selectedFile.name;"
"  btn.disabled = false;"
"});"
"function startUpload() {"
"  if (!selectedFile) return;"
"  btn.disabled = true;"
"  document.getElementById('progress-wrap').style.display = 'block';"
"  const status = document.getElementById('status');"
"  const bar    = document.getElementById('progress-bar');"
"  status.textContent = 'Upload in corso...';"
"  const xhr = new XMLHttpRequest();"
"  xhr.open('POST', '/ota');"
"  xhr.setRequestHeader('Content-Type', 'application/octet-stream');"
"  xhr.upload.onprogress = e => {"
"    if (e.lengthComputable) {"
"      const pct = Math.round(e.loaded / e.total * 100);"
"      bar.style.width = pct + '%';"
"      status.textContent = 'Upload: ' + pct + '%';"
"    }"
"  };"
"  xhr.onload = () => {"
"    if (xhr.status === 200) {"
"      bar.style.width = '100%';"
"      bar.style.background = '#3fb950';"
"      status.textContent = '\u2705 Completato! Riavvio in corso...';"
"    } else {"
"      bar.style.background = '#f85149';"
"      status.textContent = '\u274C Errore: ' + xhr.responseText;"
"      btn.disabled = false;"
"    }"
"  };"
"  xhr.onerror = () => {"
"    status.textContent = '\u274C Errore di connessione';"
"    btn.disabled = false;"
"  };"
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
/*  POST /ota  (riceve il .bin e flasha)                               */
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

    char buf[1024];
    int received  = 0;
    int remaining = req->content_len;

    while (remaining > 0) {
        int len = httpd_req_recv(req, buf, MIN(remaining, (int)sizeof(buf)));
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

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition fallito");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA completato! Riavvio...");
    httpd_resp_sendstr(req, "OK");

    /* Anche qui deleghiamo il reboot a un task separato. */
    xTaskCreate(reboot_task, "reboot_ota", 1024, NULL, 5, NULL);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  GET /settings  (pagina impostazioni generali: altitudine, ecc.)   */
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
"h2{font-size:1rem;color:#e6edf3;margin-bottom:1.2rem;font-weight:600}"
".field{display:flex;flex-direction:column;gap:0.4rem;margin-bottom:1.2rem}"
".field label{font-size:0.75rem;color:#8b949e;text-transform:uppercase;letter-spacing:1px}"
".field input{background:#0d1117;color:#e6edf3;border:1px solid #30363d;border-radius:8px;"
"padding:0.5rem 0.8rem;font-size:1rem;width:100%}"
".field input:focus{outline:none;border-color:#58a6ff}"
".field .hint{font-size:0.72rem;color:#484f58}"
".btn-save{width:100%;padding:0.7rem;background:#1a3a5c;color:#58a6ff;"
"border:1px solid #58a6ff;border-radius:8px;cursor:pointer;font-size:0.95rem;font-weight:bold}"
".btn-save:hover{background:#58a6ff;color:#0d1117}"
"#msg{text-align:center;margin-top:0.8rem;font-size:0.85rem;min-height:1.2rem}"
"#msg.ok{color:#3fb950}#msg.err{color:#f85149}"
".pres-preview{margin-top:1rem;padding:0.8rem;background:#0d1117;border-radius:8px;"
"font-size:0.85rem;color:#8b949e;line-height:1.6}"
"</style></head><body>"
"<h1>&#x2699;&#xFE0F; Impostazioni</h1>"
"<nav>"
"  <a href='/monitor'>&#x1F4CA; Monitor</a>"
"  <a href='/config' class='active'>&#x2699;&#xFE0F; Impostazioni</a>"
"  <a href='/update'>&#x1F504; OTA</a>"
"</nav>"
"<script>"
"async function load(){"
"  try{"
"    const r=await fetch('/data');"
"    const d=await r.json();"
"      +'<b>Stato:</b> '+pe.state_emoji+' '+pe.state+'<br>'"
"      +'<b>Trend 3h:</b> '+pe.trend_emoji+' '+pe.trend+(pe.delta3h!==0?' ('+( pe.delta3h>0?'+':'')+pe.delta3h.toFixed(1)+' hPa/3h)':'')+'<br>'"
"  }catch(e){document.getElementById('preview').textContent='Errore caricamento dati';}"
"}"
"async function save(){"
"  const alt=parseInt(document.getElementById('alt').value);"
"  if(isNaN(alt)||alt<0||alt>5000){showMsg('Altitudine non valida (0-5000m)','err');return;}"
"  try{"
"    const r=await fetch('/api/config',{method:'POST',"
"      headers:{'Content-Type':'application/json'},"
"      body:JSON.stringify({altitude:alt})});"
"    if(r.ok){showMsg('Salvato! Il trend si azzera con la nuova altitudine.','ok');load();}"
"    else showMsg('Errore salvataggio','err');"
"  }catch(e){showMsg('Errore connessione alt','err');}"
"}"
"function showMsg(t,c){const m=document.getElementById('msg');m.textContent=t;m.className=c;"
"  setTimeout(()=>{m.textContent='';m.className=''},4000);}"
"load();"
"</script></body></html>";

static esp_err_t config_page_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_CONFIG, -1);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  POST /api/settings  (salva altitudine)                            */
/* ------------------------------------------------------------------ */

static esp_err_t api_config_post_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    char body[64] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[len] = '\0';

    /* Parsing minimale: cerca "altitude": N */
    int alt = -1;
    char *p = strstr(body, "\"altitude\"");
    if (p) {
        p = strchr(p, ':');
        if (p) sscanf(p + 1, "%d", &alt);
    }

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

void http_server_start(void) {
    /* Crea il mutex la prima volta — idempotente se chiamato due volte. */
    if (!s_data_mutex) {
        s_data_mutex = xSemaphoreCreateMutex();
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable  = true;
    config.max_uri_handlers  = 15;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    config.stack_size        = 8192;   /* default 4096 — aumentato per HTML grande */

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Errore avvio server");
        return;
    }

    httpd_uri_t uris[] = {
        { .uri="/",              .method=HTTP_GET,  .handler=get_handler              },
        { .uri="/save",          .method=HTTP_POST, .handler=post_handler             },
        { .uri="/monitor",       .method=HTTP_GET,  .handler=monitor_handler          },
        { .uri="/data",          .method=HTTP_GET,  .handler=data_handler             },
        { .uri="/favicon.ico",   .method=HTTP_GET,  .handler=favicon_handler          },
        { .uri="/update",        .method=HTTP_GET,  .handler=ota_page_handler         },
        { .uri="/ota",           .method=HTTP_POST, .handler=ota_upload_handler       },
        { .uri="/config",        .method=HTTP_GET,  .handler=config_page_handler      },
        { .uri="/api/config",        .method=HTTP_POST, .handler=api_config_post_handler  },
    };

    #define URI_COUNT (sizeof(uris) / sizeof(uris[0]))

    for (int i = 0; i < URI_COUNT; i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    ESP_LOGI(TAG, "Server HTTP avviato — /monitor  /data  /config  /update");
}

void http_server_stop(void) {
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
}