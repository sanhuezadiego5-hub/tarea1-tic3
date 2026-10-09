#pragma once
#include "esp_err.h"
esp_err_t imu_transport_init(void);
esp_err_t imu_transport_wait_ready(void);

#include <stdbool.h>
#include <stdint.h>
bool imu_transport_capture_requested(void);
void imu_transport_capture_state(uint8_t state); /* 0 paused, 1 acquiring, 3 fault */
void imu_transport_pending_add(int delta);

#include "imu_config.h"
void imu_transport_get_config(imu_config_t *config);
bool imu_transport_take_config(imu_config_t *config, uint32_t *revision);
void imu_transport_config_complete(const imu_config_t *config, uint32_t revision, uint8_t error);
void imu_transport_finish_capture(void);
void imu_transport_preview(unsigned stream, const int16_t xyz[3], uint32_t count, uint32_t read_ms);

void imu_transport_window(uint32_t window);

uint32_t imu_transport_capture_id(void);
