#pragma once
#include <stdbool.h>
#include <stdint.h>
#define IMU_CONFIG_BYTES 32u
typedef struct {
    uint16_t accel_hz, gyro_hz, accel_range, gyro_range;
    uint32_t interval_ms, accel_samples, gyro_samples;
    uint8_t parallel, udp, json, blocks;
} imu_config_t;
void imu_config_defaults(imu_config_t *config);
bool imu_config_valid(const imu_config_t *config);
void imu_config_pack(const imu_config_t *config, uint8_t out[IMU_CONFIG_BYTES]);
bool imu_config_unpack(const uint8_t *bytes, uint32_t length, imu_config_t *config);
