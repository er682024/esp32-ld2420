#include "http_server.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define TAG         "HTTP"
#define ADMIN_USER  "admin"
#define ADMIN_PASS  "esp32admin"   // ← cambia questa password




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

bool check_auth(httpd_req_t *req) {
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

esp_err_t favicon_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
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

esp_err_t get_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_PAGE, -1);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  POST /save                                                          */
/* ------------------------------------------------------------------ */

esp_err_t post_handler(httpd_req_t *req) {
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

esp_err_t ota_page_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_OTA, -1);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  POST /ota                                                           */
/* ------------------------------------------------------------------ */

esp_err_t ota_upload_handler(httpd_req_t *req) {
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
        ESP_LOGI(TAG, "Nuovo firmware: v%s, build: %s %s",
                new_app_info.version,
                new_app_info.date,
                new_app_info.time);
        // Nessun blocco — accetta qualsiasi firmware valido
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

esp_err_t config_page_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_CONFIG, -1);
    return ESP_OK;
}




void get_time_str(char *buf, size_t len) {
    time_t    now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(buf, len, "%d/%m/%Y %H:%M:%S", &timeinfo);
}