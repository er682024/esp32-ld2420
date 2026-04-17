#include "ld2420.h"

static const char *TAG = "LD2420";

static ld2420_config_t s_cfg;
static ld2420_state_t  s_state;
static SemaphoreHandle_t s_state_mutex;

static int64_t last_presence_ts = 0;
static int64_t last_absence_ts = 0;
static int64_t last_motion_ts = 0;

static int64_t last_static_ts = 0;     // OT1
static int64_t last_dynamic_ts = 0;    // OT2
static int64_t last_change_ts = 0;     // qualsiasi cambio presenza
static bool last_presence_state = false;

static uint16_t cfg_min_dist = 0;
static uint16_t cfg_max_dist = 600;
static uint8_t  cfg_sensitivity = 5;


static void ld2420_gpio_init(void);
static void ld2420_uart_init(void);
static void ld2420_task(void *arg);

// ─── Frame helper ───────────────────────────────────────────────────
// Header: FD FC FB FA | len_L len_H | cmd_L cmd_H | data | 04 03 02 01
static void send_cmd(uart_port_t port, uint16_t cmd, const uint8_t *data, uint16_t data_len)
{
    uint8_t buf[32];
    uint16_t total = 2 + data_len; // cmd (2 byte) + data
    int i = 0;
    buf[i++] = 0xFD; buf[i++] = 0xFC; buf[i++] = 0xFB; buf[i++] = 0xFA;
    buf[i++] = total & 0xFF;
    buf[i++] = (total >> 8) & 0xFF;
    buf[i++] = cmd & 0xFF;
    buf[i++] = (cmd >> 8) & 0xFF;
    for (int j = 0; j < data_len; j++) buf[i++] = data[j];
    buf[i++] = 0x04; buf[i++] = 0x03; buf[i++] = 0x02; buf[i++] = 0x01;
    uart_write_bytes(port, (const char*)buf, i);
    vTaskDelay(pdMS_TO_TICKS(100)); // attendi ACK
}

void ld2420_exit_engineering_mode(void)
{
    // CMD 0x0063 — nessun parametro
    send_cmd(s_cfg.uart_num, 0x0063, NULL, 0);
    ESP_LOGI(TAG, "Sent: exit engineering mode");
}

void ld2420_enter_engineering_mode(void)
{
    // CMD 0x0062 — nessun parametro
    send_cmd(s_cfg.uart_num, 0x0062, NULL, 0);
    ESP_LOGI(TAG, "Sent: enter engineering mode");
}

esp_err_t ld2420_set_range(uint16_t min_cm, uint16_t max_cm)
{
    // CMD 0x0060 — parametro: min (2B LE) + max (2B LE)
    // Il LD2420 usa gate da 70cm, quindi converte in gate
    // ma accetta anche cm diretti nel campo distanza
    uint8_t data[4];
    data[0] = min_cm & 0xFF;  data[1] = (min_cm >> 8) & 0xFF;
    data[2] = max_cm & 0xFF;  data[3] = (max_cm >> 8) & 0xFF;
    send_cmd(s_cfg.uart_num, 0x0060, data, 4);
    cfg_min_dist = min_cm;
    cfg_max_dist = max_cm;
    ESP_LOGI(TAG, "Set range: %u–%u cm", min_cm, max_cm);
    return ESP_OK;
}

esp_err_t ld2420_set_motion_sensitivity(uint8_t sens)
{
    // CMD 0x0064 — motion threshold (0-9)
    uint8_t data[1] = { sens };
    send_cmd(s_cfg.uart_num, 0x0064, data, 1);
    ESP_LOGI(TAG, "Set motion sensitivity: %u", sens);
    return ESP_OK;
}

esp_err_t ld2420_set_static_sensitivity(uint8_t sens)
{
    // CMD 0x0065 — static threshold (0-9)
    uint8_t data[1] = { sens };
    send_cmd(s_cfg.uart_num, 0x0065, data, 1);
    ESP_LOGI(TAG, "Set static sensitivity: %u", sens);
    return ESP_OK;
}

esp_err_t ld2420_init(const ld2420_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    s_cfg = *cfg;

    s_state_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex) {
        return ESP_FAIL;
    }

    ld2420_gpio_init();
    ld2420_uart_init();
    vTaskDelay(pdMS_TO_TICKS(50));
    ld2420_exit_engineering_mode();

    ESP_LOGI(TAG, "LD2420 initialized (OT1=%d, OT2=%d, UART=%d TX=%d baud=%d)",
             s_cfg.pin_ot1, s_cfg.pin_ot2, s_cfg.uart_num, s_cfg.uart_tx, s_cfg.uart_baud);

    int64_t now = esp_timer_get_time();
    last_presence_ts = now;
    last_absence_ts  = now;
    last_motion_ts   = now;
    last_static_ts   = now;
    last_dynamic_ts  = now;
    last_change_ts   = now;
    last_presence_state = false;

    return ESP_OK;
}

void ld2420_task_start(UBaseType_t priority, uint32_t stack_size)
{
    xTaskCreate(ld2420_task, "ld2420_task", stack_size, NULL, priority, NULL);
}

/* GPIO: OT1 / OT2 come input */
static void ld2420_gpio_init(void)
{
    gpio_config_t io_conf = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    if (s_cfg.pin_ot1 >= 0) {
        io_conf.pin_bit_mask = 1ULL << s_cfg.pin_ot1;
        gpio_config(&io_conf);
    }

    if (s_cfg.pin_ot2 >= 0) {
        io_conf.pin_bit_mask = 1ULL << s_cfg.pin_ot2;
        gpio_config(&io_conf);
    }
}

/* UART: solo TX per comandi verso LD2420 (RX del modulo) */
static void ld2420_uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = s_cfg.uart_baud,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    uart_param_config(s_cfg.uart_num, &uart_config);
    uart_set_pin(s_cfg.uart_num, s_cfg.uart_tx, UART_PIN_NO_CHANGE,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(s_cfg.uart_num, 1024, 0, 0, NULL, 0);
    ESP_LOGI(TAG, "LD2420 UART initialized");
}

/* Comando: uscita da engineering mode */
/*
void ld2420_exit_engineering_mode(void)
{
    uint8_t cmd[] = {0xFD,0xFC,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
    uart_write_bytes(s_cfg.uart_num, (const char*)cmd, sizeof(cmd));
    ESP_LOGI(TAG, "Sent: exit engineering mode");
}

void ld2420_enter_engineering_mode(void)
{
    uint8_t cmd[] = {0xFD,0xFC,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
    uart_write_bytes(s_cfg.uart_num, (const char*)cmd, sizeof(cmd));
    ESP_LOGI(TAG, "Sent: enter engineering mode");
}
*/

/* Task: legge OT1/OT2 e aggiorna stato */
static void ld2420_task(void *arg)
{
    bool last_ot1 = false;
    bool last_ot2 = false;

    while (1) {
        bool ot1 = false;
        bool ot2 = false;

        if (s_cfg.pin_ot1 >= 0) {
            ot1 = gpio_get_level(s_cfg.pin_ot1);
        }
        if (s_cfg.pin_ot2 >= 0) {
            ot2 = gpio_get_level(s_cfg.pin_ot2);
        }

        bool motion   = ot1;
        bool static_p = ot2;
        bool presence = ot1 || ot2;

        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            s_state.presence        = presence;
            s_state.motion          = motion;
            s_state.static_presence = static_p;
            xSemaphoreGive(s_state_mutex);
        }

        /* log solo su cambiamento per non spammare */
        if (ot2 && !last_ot2) {
            ESP_LOGI(TAG, "OT2 presence detected");
        }
        if (!ot2 && last_ot2) {
            ESP_LOGI(TAG, "OT2 cleared");
        }
        if (s_cfg.pin_ot1 >= 0) {
            if (ot1 && !last_ot1) {
                ESP_LOGI(TAG, "OT1 static presence detected");
            }
            if (!ot1 && last_ot1) {
                ESP_LOGI(TAG, "OT1 static cleared");
            }
        }

        last_ot1 = ot1;
        last_ot2 = ot2;

        int64_t now = esp_timer_get_time(); // microsecondi

        if (presence) {
            last_presence_ts = now;
        } else {
            last_absence_ts = now;
        }

        if (presence != last_presence_state) {
            last_change_ts = now;
        }
        last_presence_state = presence;

        if (motion) {
            last_motion_ts = now;
        }

        if (ot1) {
            last_static_ts = now;
        }

        if (ot2) {
            last_dynamic_ts = now;
        }

        vTaskDelay(pdMS_TO_TICKS(50)); // 20 Hz
    }
}

ld2420_state_t ld2420_get_state(void)
{
    ld2420_state_t copy;
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        copy = s_state;
        xSemaphoreGive(s_state_mutex);
    } else {
        copy = s_state;
    }
    return copy;
}

uint32_t ld2420_ms_since_presence() {
    int64_t now = esp_timer_get_time();
    return (uint32_t)((now - last_presence_ts) / 1000);
}

uint32_t ld2420_ms_since_absence() {
    int64_t now = esp_timer_get_time();
    return (uint32_t)((now - last_absence_ts) / 1000);
}

uint32_t ld2420_ms_since_motion() {
    int64_t now = esp_timer_get_time();
    return (uint32_t)((now - last_motion_ts) / 1000);
}

uint16_t ld2420_get_min_distance() {
    return cfg_min_dist;
}

uint16_t ld2420_get_max_distance() {
    return cfg_max_dist;
}

uint8_t ld2420_get_sensitivity() {
    return cfg_sensitivity;
}

uint32_t ld2420_get_uptime_ms() {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

uint32_t ld2420_ms_since_static_presence() {
    return (uint32_t)((esp_timer_get_time() - last_static_ts) / 1000);
}

uint32_t ld2420_ms_since_dynamic_presence() {
    return (uint32_t)((esp_timer_get_time() - last_dynamic_ts) / 1000);
}

uint32_t ld2420_ms_since_state_change() {
    return (uint32_t)((esp_timer_get_time() - last_change_ts) / 1000);
}

/*
esp_err_t ld2420_set_range(uint16_t min_cm, uint16_t max_cm)
{
    uint8_t cmd[12] = {0xFD,0xFC,0x05,0x00};
    cmd[4] = min_cm & 0xFF;
    cmd[5] = (min_cm >> 8) & 0xFF;
    cmd[6] = max_cm & 0xFF;
    cmd[7] = (max_cm >> 8) & 0xFF;

    uart_write_bytes(s_cfg.uart_num, (const char*)cmd, sizeof(cmd));
    ESP_LOGI(TAG, "Set range: %u–%u cm", min_cm, max_cm);

    cfg_min_dist = min_cm;
    cfg_max_dist = max_cm;
    return ESP_OK;
}

esp_err_t ld2420_set_motion_sensitivity(uint8_t sens)
{
    uint8_t cmd[12] = {0xFD,0xFC,0x07,0x00, sens, 0,0,0,0,0,0,0};
    uart_write_bytes(s_cfg.uart_num, (const char*)cmd, sizeof(cmd));
    ESP_LOGI(TAG, "Set motion sensitivity: %u", sens);
    return ESP_OK;
}
*/

/*
esp_err_t ld2420_set_static_sensitivity(uint8_t sens)
{
    uint8_t cmd[12] = {0xFD,0xFC,0x08,0x00, sens, 0,0,0,0,0,0,0};
    uart_write_bytes(s_cfg.uart_num, (const char*)cmd, sizeof(cmd));
    ESP_LOGI(TAG, "Set static sensitivity: %u", sens);
    return ESP_OK;
}
*/

esp_err_t ld2420_apply_default_config(void)
{
    ESP_LOGI(TAG, "Applying LD2420 default config...");

    ld2420_enter_engineering_mode();
    vTaskDelay(pdMS_TO_TICKS(50));

    // Range consigliato per evitare falsi positivi
    uint16_t min_dist = 100;
    uint16_t max_dist = 300;

    if (ld2420_set_range(min_dist, max_dist) != ESP_OK) {
        ESP_LOGE(TAG, "Error setting range");
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    // Sensibilità consigliate
    if (ld2420_set_motion_sensitivity(6) != ESP_OK) {
        ESP_LOGE(TAG, "Error setting motion sensitivity");
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    if (ld2420_set_static_sensitivity(5) != ESP_OK) {
        ESP_LOGE(TAG, "Error setting static sensitivity");
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    ld2420_exit_engineering_mode();
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "LD2420 default config applied.");
    return ESP_OK;
}


/**
 * @brief Stampa tutti i parametri configurati del LD2420
 */
void ld2420_print_params(const ld2420_params_t *params)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "PARAMETRI LD2420");
    ESP_LOGI(TAG, "========================================");
    
    ESP_LOGI(TAG, "Max Gate:           %u (portata ~%um)", 
             params->max_gate, params->max_gate * 7 / 10);
    
    ESP_LOGI(TAG, "Timeout:            %u secondi", params->timeout);
    ESP_LOGI(TAG, "Output Mode:        %u", params->output_mode);
    ESP_LOGI(TAG, "Sensitivity Global: %u", params->sensitivity);
    ESP_LOGI(TAG, "Auto Threshold:     %s", 
             params->auto_threshold ? "ON" : "OFF");
    
    ESP_LOGI(TAG, "----------------------------------------");
    ESP_LOGI(TAG, "Soglie per GATE (0-15):");
    ESP_LOGI(TAG, "----------------------------------------");
    
    // Stampa intestazione tabella
    printf("Gate | Distanza  | Static Thr | Motion Thr | Stato\n");
    printf("-----|-----------|------------|------------|-------\n");
    
    for (int i = 0; i < 16; i++) {
        float distance_m = i * 0.7f;  // ~0.7m per gate (dipende da config)
        bool enabled = (params->static_threshold[i] > 0 || params->motion_threshold[i] > 0);
        
        printf(" %2d  | %4.1f-%4.1fm |    %4u    |    %4u    |  %s\n",
               i,
               distance_m,
               distance_m + 0.7f,
               params->static_threshold[i],
               params->motion_threshold[i],
               enabled ? "ON" : "OFF");
    }
    
    ESP_LOGI(TAG, "========================================");
}

/**
 * @brief Stampa mappa gate semplificata (solo gate attivi fino a max_gate)
 */
void ld2420_print_gate_map(uint16_t max_gate, 
                           const uint16_t *static_thr, 
                           const uint16_t *motion_thr)
{
    ESP_LOGI(TAG, "--- Mappa Gate Attivi (0-%u) ---", max_gate);
    
    for (uint16_t i = 0; i <= max_gate && i < 16; i++) {
        char bar_static[21] = {0};
        char bar_motion[21] = {0};
        
        // Crea barre grafiche (0-100 → 0-20 caratteri)
        int s_len = static_thr[i] / 5;
        int m_len = motion_thr[i] / 5;
        
        for (int j = 0; j < 20; j++) {
            bar_static[j] = (j < s_len) ? '#' : '.';
            bar_motion[j] = (j < m_len) ? '#' : '.';
        }
        
        ESP_LOGI(TAG, "Gate[%2u] S:[%s] %3u  M:[%s] %3u", 
                 i, bar_static, static_thr[i], bar_motion, motion_thr[i]);
    }
}

/**
 * @brief Stampa dati in tempo reale (da chiamare nel loop)
 */
void ld2420_print_realtime(bool motion, bool static_presence, 
                           uint16_t distance, uint16_t energy)
{
    static uint32_t last_print = 0;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Stampa ogni 500ms solo se cambiato
    if (now - last_print < 500) return;
    last_print = now;
    
    const char *status = motion ? "MOVIMENTO" : 
                         (static_presence ? "STATICO  " : "VUOTO    ");
    
    ESP_LOGI(TAG, "[%s] Dist:%3ucm Energia:%4u | M:%d S:%d",
             status, distance, energy, motion, static_presence);
}

