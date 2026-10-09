#include "imu_wifi.h"
#include "sdkconfig.h"

#ifndef CONFIG_TIC3_WIFI_ENABLE
esp_err_t imu_wifi_start(void) { return ESP_OK; }
void imu_wifi_get_status(imu_wifi_status_t *status)
{
    if (status) *status = (imu_wifi_status_t){.profile_index = -1, .ssid = "", .server_host = ""};
}
void imu_wifi_report_server_result(uint32_t generation, bool success)
{ (void)generation; (void)success; }
void imu_wifi_request_rescan(void) {}
#else

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#ifndef CONFIG_TIC3_WIFI_1_ENABLE
#define CONFIG_TIC3_WIFI_1_ENABLE 0
#endif
#ifndef CONFIG_TIC3_WIFI_2_ENABLE
#define CONFIG_TIC3_WIFI_2_ENABLE 0
#endif
#ifndef CONFIG_TIC3_WIFI_3_ENABLE
#define CONFIG_TIC3_WIFI_3_ENABLE 0
#endif
#ifndef CONFIG_TIC3_WIFI_1_OPEN
#define CONFIG_TIC3_WIFI_1_OPEN 0
#endif
#ifndef CONFIG_TIC3_WIFI_2_OPEN
#define CONFIG_TIC3_WIFI_2_OPEN 0
#endif
#ifndef CONFIG_TIC3_WIFI_3_OPEN
#define CONFIG_TIC3_WIFI_3_OPEN 0
#endif

static const imu_wifi_profile_t s_profiles[3] = {
    {CONFIG_TIC3_WIFI_1_ENABLE, CONFIG_TIC3_WIFI_1_OPEN, CONFIG_TIC3_WIFI_1_SSID,
     CONFIG_TIC3_WIFI_1_PASSWORD, CONFIG_TIC3_WIFI_1_SERVER, CONFIG_TIC3_WIFI_1_PORT},
    {CONFIG_TIC3_WIFI_2_ENABLE, CONFIG_TIC3_WIFI_2_OPEN, CONFIG_TIC3_WIFI_2_SSID,
     CONFIG_TIC3_WIFI_2_PASSWORD, CONFIG_TIC3_WIFI_2_SERVER, CONFIG_TIC3_WIFI_2_PORT},
    {CONFIG_TIC3_WIFI_3_ENABLE, CONFIG_TIC3_WIFI_3_OPEN, CONFIG_TIC3_WIFI_3_SSID,
     CONFIG_TIC3_WIFI_3_PASSWORD, CONFIG_TIC3_WIFI_3_SERVER, CONFIG_TIC3_WIFI_3_PORT},
};
static const char *TAG = "imu_wifi";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static imu_wifi_status_t s_status = {.profile_index = -1};
static bool s_eligible[3];
static esp_netif_t *s_netif;
static QueueHandle_t s_events;
static bool s_started;

enum { GOT_IP, LOST_WIFI, SERVER_OK, SERVER_FAILED, RESCAN };
typedef struct { int kind; uint32_t generation; uint32_t ipv4; } wifi_notice_t;

void imu_wifi_get_status(imu_wifi_status_t *status)
{
    if (!status) return;
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
    int i = status->profile_index;
    status->ssid = i >= 0 ? s_profiles[i].ssid : "";
    status->server_host = i >= 0 ? s_profiles[i].server_host : "";
    status->server_port = i >= 0 ? s_profiles[i].server_port : 0;
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    wifi_notice_t notice = {0};
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        notice.kind = GOT_IP;
        notice.ipv4 = ((ip_event_got_ip_t *)data)->ip_info.ip.addr;
    } else if ((base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) ||
               (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP)) {
        portENTER_CRITICAL(&s_lock);
        s_status.ready = false;
        s_status.ipv4 = 0;
        s_status.generation++;
        if (base == WIFI_EVENT)
            s_status.last_disconnect_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        portEXIT_CRITICAL(&s_lock);
        notice.kind = LOST_WIFI;
    } else return;
    /* Queue bounded. Loss also clears ready so the manager's 1-s poll wakes. */
    xQueueSend(s_events, &notice, 0);
}

void imu_wifi_report_server_result(uint32_t generation, bool success)
{
    if (!s_events) return;
    wifi_notice_t notice = {.kind = success ? SERVER_OK : SERVER_FAILED, .generation = generation};
    xQueueSend(s_events, &notice, 0);
}

void imu_wifi_request_rescan(void)
{
    if (!s_events) return;
    wifi_notice_t notice = {.kind = RESCAN};
    xQueueSend(s_events, &notice, 0);
}

static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000u; }

static void disconnect_and_drain(void)
{
    esp_wifi_disconnect();
    portENTER_CRITICAL(&s_lock);
    s_status.ready = false;
    s_status.ipv4 = 0;
    s_status.generation++;
    portEXIT_CRITICAL(&s_lock);
    wifi_notice_t old;
    while (xQueueReceive(s_events, &old, pdMS_TO_TICKS(100)) == pdTRUE) {}
}

static bool visible_profile(int i)
{
    wifi_scan_config_t scan = {
        .ssid = (uint8_t *)s_profiles[i].ssid,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.max = 120,
    };
    esp_err_t err = esp_wifi_scan_start(&scan, true); /* blocks only WiFi manager */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Scan perfil %d: %s", i + 1, esp_err_to_name(err));
        esp_wifi_clear_ap_list();
        return false;
    }
    uint16_t count = 0;
    err = esp_wifi_scan_get_ap_num(&count);
    if (err != ESP_OK || !count) {
        esp_wifi_clear_ap_list();
        return false;
    }
    wifi_ap_record_t ap;
    count = 1;
    err = esp_wifi_scan_get_ap_records(&count, &ap); /* frees scanned list */
    if (err != ESP_OK) {
        esp_wifi_clear_ap_list();
        return false;
    }
    if (ap.authmode == WIFI_AUTH_WPA2_ENTERPRISE) {
        ESP_LOGW(TAG, "Perfil %d requiere 802.1X; configurar soporte enterprise aparte", i + 1);
        return false;
    }
    return count == 1;
}

static bool associate_profile(int i)
{
    disconnect_and_drain();
    const imu_wifi_profile_t *p = &s_profiles[i];
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, p->ssid, strlen(p->ssid));
    memcpy(config.sta.password, p->password, strlen(p->password));
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.threshold.authmode = p->open_network ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.failure_retry_cnt = 0;
    if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK || esp_wifi_connect() != ESP_OK)
        return false;
    uint64_t deadline = now_ms() + CONFIG_TIC3_WIFI_CONNECT_TIMEOUT_S * 1000u;
    while (now_ms() < deadline) {
        wifi_notice_t notice;
        if (xQueueReceive(s_events, &notice, pdMS_TO_TICKS(250)) != pdTRUE) continue;
        if (notice.kind == LOST_WIFI || notice.kind == RESCAN) return false;
        if (notice.kind != GOT_IP) continue;
        wifi_ap_record_t ap;
        esp_netif_ip_info_t info;
        /* Reject stale IP notifications from an earlier connection/profile. */
        if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK ||
            strcmp((const char *)ap.ssid, p->ssid) != 0 ||
            esp_netif_get_ip_info(s_netif, &info) != ESP_OK ||
            !info.ip.addr || info.ip.addr != notice.ipv4) continue;
        portENTER_CRITICAL(&s_lock);
        s_status.profile_index = i;
        s_status.ipv4 = info.ip.addr;
        s_status.generation++;
        s_status.ready = true;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "WiFi perfil %d: %s, IP " IPSTR ", servidor %s:%u (sin handshake aun)",
                 i + 1, p->ssid, IP2STR(&info.ip), p->server_host, p->server_port);
        return true;
    }
    ESP_LOGW(TAG, "Perfil %d: timeout autenticacion/DHCP", i + 1);
    return false;
}

static void wifi_manager(void *arg)
{
    (void)arg;
    uint64_t retry_after[3] = {0};
    unsigned failed_rounds = 0;
    while (true) {
        disconnect_and_drain();
        int connected = -1;
        bool visible[3] = {0};
        for (int i = 0; i < 3; i++) {
            if (s_eligible[i] && now_ms() >= retry_after[i]) visible[i] = visible_profile(i);
        }
        int candidate;
        while ((candidate = imu_wifi_pick(s_eligible, visible, retry_after, now_ms())) >= 0) {
            for (int attempt = 0; attempt < 2; attempt++) {
                ESP_LOGI(TAG, "Intento %d/2 perfil prioridad %d: %s", attempt + 1,
                         candidate + 1, s_profiles[candidate].ssid);
                if (associate_profile(candidate)) { connected = candidate; break; }
            }
            if (connected >= 0) break;
            disconnect_and_drain();
            retry_after[candidate] = now_ms() + IMU_WIFI_FAILURE_COOLDOWN_MS;
            visible[candidate] = false;
            ESP_LOGW(TAG, "Perfil %d agotado; cooldown 60 s, se prueba siguiente", candidate + 1);
        }
        if (connected < 0) {
            portENTER_CRITICAL(&s_lock);
            s_status.failed_rounds++;
            portEXIT_CRITICAL(&s_lock);
            uint32_t delay = imu_wifi_backoff_ms(failed_rounds);
            if (failed_rounds < 5) failed_rounds++;
            delay += esp_random() % 251u;
            if (delay > 30000u) delay = 30000u;
            vTaskDelay(pdMS_TO_TICKS(delay));
            continue;
        }
        failed_rounds = 0;
        unsigned server_failures = 0;
        /* No preemption of healthy fallback WiFi when preferred AP appears. */
        while (true) {
            imu_wifi_status_t status;
            imu_wifi_get_status(&status);
            if (!status.ready) break;
            wifi_notice_t notice;
            if (xQueueReceive(s_events, &notice, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
            if (notice.kind == RESCAN || notice.kind == LOST_WIFI) break;
            if (notice.kind == GOT_IP && notice.ipv4 != status.ipv4) {
                /* New lease invalidates sockets; re-associate from priorities. */
                break;
            }
            if (notice.generation != status.generation) continue;
            if (notice.kind == SERVER_OK) server_failures = 0;
            if (notice.kind == SERVER_FAILED && ++server_failures >= 3) {
                retry_after[connected] = now_ms() + IMU_WIFI_FAILURE_COOLDOWN_MS;
                ESP_LOGW(TAG, "Servidor no accesible tras 3 fallos; se cambia perfil");
                break;
            }
        }
    }
}

esp_err_t imu_wifi_start(void)
{
    if (s_started) return ESP_OK;
    unsigned valid = 0;
    for (int i = 0; i < 3; i++) {
        s_eligible[i] = imu_wifi_profile_valid(&s_profiles[i]);
        if (s_eligible[i]) valid++;
        else if (s_profiles[i].enabled)
            ESP_LOGW(TAG, "Perfil %d incompleto/invalido: ajustar SSID, clave y endpoint", i + 1);
    }
    if (!valid) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) return err; /* never erase NVS to hide an error */
    if ((err = esp_netif_init()) != ESP_OK) return err;
    bool own_loop = false;
    err = esp_event_loop_create_default();
    if (err == ESP_OK) own_loop = true;
    else if (err != ESP_ERR_INVALID_STATE) return err;
    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) { if (own_loop) esp_event_loop_delete_default(); return ESP_ERR_NO_MEM; }
    s_events = xQueueCreate(8, sizeof(wifi_notice_t));
    bool driver = false, wifi_handler = false, ip_handler = false;
    esp_event_handler_instance_t wifi_instance, ip_instance;
    if (!s_events) { err = ESP_ERR_NO_MEM; goto fail; }
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&config)) != ESP_OK) goto fail;
    driver = true;
    if ((err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                   event_handler, NULL, &wifi_instance)) != ESP_OK) goto fail;
    wifi_handler = true;
    if ((err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                   event_handler, NULL, &ip_instance)) != ESP_OK) goto fail;
    ip_handler = true;
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (err = esp_wifi_start()) != ESP_OK) goto fail;
    if (xTaskCreatePinnedToCore(wifi_manager, "imu_wifi", 4096, NULL, 3, NULL, 0) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    s_started = true;
    return ESP_OK;
fail:
    if (wifi_handler) esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_instance);
    if (ip_handler) esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, ip_instance);
    if (driver) { esp_wifi_stop(); esp_wifi_deinit(); }
    if (s_events) { vQueueDelete(s_events); s_events = NULL; }
    esp_netif_destroy_default_wifi(s_netif); s_netif = NULL;
    if (own_loop) esp_event_loop_delete_default();
    return err;
}
#endif
