#pragma once
#include "esp_err.h"
#include "imu_stream.h"

/* Called only by the output task. The block must not be retained after return. */
esp_err_t imu_send_block(const imu_block_t *block);
/* Helpers for future transport: selected format and immutable block identity. */
uint16_t imu_output_fragment_count(const imu_block_t *block);
size_t imu_output_encode_fragment(const imu_block_t *block, uint16_t index,
                                  uint8_t *out, size_t capacity);
void imu_output_identity(uint64_t *device_id, uint64_t *boot_id);
esp_err_t imu_console_send_block(const imu_block_t *block);
