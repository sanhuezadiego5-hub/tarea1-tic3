#include "wifi_policy.h"
#include <string.h>

bool imu_wifi_profile_valid(const imu_wifi_profile_t *p)
{
    if (!p || !p->enabled || !p->ssid || !p->password || !p->server_host || !p->server_port)
        return false;
    size_t ssid = strlen(p->ssid), password = strlen(p->password), host = strlen(p->server_host);
    if (!ssid || ssid > 32 || !host || host > 63) return false;
    if (p->open_network ? password != 0 : (password < 8 || password > 63)) return false;
    for (size_t i = 0; i < host; i++) {
        char c = p->server_host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    }
    return true;
}

int imu_wifi_pick(const bool eligible[IMU_WIFI_PROFILE_COUNT],
                  const bool visible[IMU_WIFI_PROFILE_COUNT],
                  const uint64_t retry_after_ms[IMU_WIFI_PROFILE_COUNT], uint64_t now_ms)
{
    for (int i = 0; i < IMU_WIFI_PROFILE_COUNT; i++)
        if (eligible[i] && visible[i] && now_ms >= retry_after_ms[i]) return i;
    return IMU_WIFI_NO_PROFILE;
}

uint32_t imu_wifi_backoff_ms(unsigned failures)
{
    return failures < 5 ? 1000u << failures : 30000u;
}
