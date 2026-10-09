#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IMU_MAX_RATE_HZ 400
#define IMU_MIN_INTERVAL_MS 100
#define IMU_MAX_INTERVAL_MS 60000
#define IMU_POOL_BUDGET_BYTES (128u * 1024u)

typedef enum { IMU_ACCEL = 0, IMU_GYRO = 1 } imu_stream_id_t;

typedef struct {
    imu_stream_id_t stream;
    uint16_t sample_hz;
    uint16_t range;
    uint16_t capacity;
    uint32_t interval_ms;
    uint8_t flags;
    uint32_t window_seq;
    int64_t window_start_us;
    int64_t window_end_us;
    uint16_t count;
    uint32_t buffer_dropped;
    uint32_t fifo_skipped_frames;
    uint32_t fifo_errors;
    uint32_t i2c_errors;
    uint32_t output_dropped_blocks;
    /* Bound once to owned storage. Reinitializing metadata preserves pointers. */
    int16_t (*xyz)[3];
    uint32_t *read_offset_us;
    uint32_t *sample_seq;
} imu_block_t;

bool imu_capacity_for(uint16_t hz, uint32_t interval_ms, uint16_t *capacity);
size_t imu_block_storage_bytes(uint16_t capacity);
bool imu_block_bind(imu_block_t *block, void *storage, size_t bytes, uint16_t capacity);
void imu_block_init(imu_block_t *block, imu_stream_id_t stream,
                    uint16_t hz, uint16_t range, uint32_t window_seq,
                    int64_t start_us, uint32_t dropped_blocks, uint32_t interval_ms);
bool imu_block_append(imu_block_t *block, const int16_t xyz[3],
                      uint64_t read_us, uint32_t sample_seq);
float imu_raw_to_si(imu_stream_id_t stream, uint16_t range, int16_t raw);
uint64_t imu_block_read_us(const imu_block_t *block, size_t index);

/* Only complete FIFO frames are delivered. BMI270 repeats an incomplete
 * frame on the next FIFO_DATA transaction; do not concatenate the tail. */
typedef void (*imu_fifo_sample_cb)(void *context, imu_stream_id_t stream,
                                   const int16_t xyz[3]);
typedef struct {
    uint32_t skipped_frames; /* 255 is a lower bound, per hardware semantics */
    bool invalid_header;
    bool partial_tail;
} imu_fifo_result_t;

imu_fifo_result_t imu_fifo_parse(const uint8_t *data, size_t length,
                                imu_fifo_sample_cb callback, void *context);
