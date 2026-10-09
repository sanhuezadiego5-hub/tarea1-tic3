#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IMU_WIFI_PROFILE_COUNT 3
#define IMU_WIFI_NO_PROFILE (-1)
#define IMU_WIFI_FAILURE_COOLDOWN_MS 60000u

typedef struct {
    bool enabled;
    bool open_network;
    const char *ssid;
    const char *password;
    const char *server_host;
    uint16_t server_port;
} imu_wifi_profile_t;

bool imu_wifi_profile_valid(const imu_wifi_profile_t *profile);
/* Lowest array index has highest priority; cooldown prevents bad credentials
 * on a preferred AP from starving all fallback profiles. */
int imu_wifi_pick(const bool eligible[IMU_WIFI_PROFILE_COUNT],
                  const bool visible[IMU_WIFI_PROFILE_COUNT],
                  const uint64_t retry_after_ms[IMU_WIFI_PROFILE_COUNT], uint64_t now_ms);
uint32_t imu_wifi_backoff_ms(unsigned failures);
