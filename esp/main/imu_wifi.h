#pragma once
#include "esp_err.h"
#include "wifi_policy.h"

typedef struct {
    bool ready; /* authenticated and DHCP IP obtained; NOT server confirmation */
    int profile_index;
    uint32_t ipv4; /* network byte order */
    uint32_t generation; /* changed on loss/reconnect: invalidate old sockets */
    uint16_t last_disconnect_reason;
    uint32_t failed_rounds;
    const char *ssid;
    const char *server_host;
    uint16_t server_port;
} imu_wifi_status_t;

esp_err_t imu_wifi_start(void);
void imu_wifi_get_status(imu_wifi_status_t *status);
/* Future socket layer: report confirmed connect/HELLO failure or success.
 * Stale reports for a previous generation are ignored. After 3 failures the
 * selected profile is cooled down and fallback selection is restarted. */
void imu_wifi_report_server_result(uint32_t generation, bool success);
/* Explicit request between sessions; never roam automatically from healthy WiFi. */
void imu_wifi_request_rescan(void);
