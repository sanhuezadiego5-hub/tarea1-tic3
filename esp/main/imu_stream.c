#include "imu_stream.h"
#include <string.h>

bool imu_capacity_for(uint16_t hz, uint32_t interval_ms, uint16_t *capacity)
{
    if (!capacity || (hz != 25 && hz != 50 && hz != 100 && hz != 200 && hz != 400) ||
        interval_ms < IMU_MIN_INTERVAL_MS || interval_ms > IMU_MAX_INTERVAL_MS) return false;
    uint64_t expected = ((uint64_t)hz * interval_ms + 999u) / 1000u;
    uint64_t n = expected + (expected + 19u) / 20u + 8u; /* 5% + 8 XYZ margin */
    if (n > UINT16_MAX) return false;
    *capacity = (uint16_t)n;
    return true;
}

size_t imu_block_storage_bytes(uint16_t capacity)
{
    return (size_t)capacity * 14u; /* raw XYZ=6, read offset=4, sequence=4 */
}

bool imu_block_bind(imu_block_t *b, void *storage, size_t bytes, uint16_t capacity)
{
    if (!b || !storage || !capacity || bytes < imu_block_storage_bytes(capacity) ||
        (uintptr_t)storage % _Alignof(uint32_t) != 0) return false;
    b->capacity = capacity;
    b->sample_seq = storage;
    b->read_offset_us = b->sample_seq + capacity;
    b->xyz = (int16_t (*)[3])(b->read_offset_us + capacity);
    return true;
}

void imu_block_init(imu_block_t *b, imu_stream_id_t stream,
                    uint16_t hz, uint16_t range, uint32_t seq,
                    int64_t start_us, uint32_t dropped_blocks, uint32_t interval_ms)
{
    /* Arrays outside count are never serialized; avoid zeroing 9 KiB. */
    b->stream = stream;
    b->sample_hz = hz;
    b->range = range;
    b->interval_ms = interval_ms;
    b->flags = 0;
    b->window_seq = seq;
    b->window_start_us = start_us;
    b->window_end_us = start_us;
    b->count = 0;
    b->buffer_dropped = 0;
    b->fifo_skipped_frames = 0;
    b->fifo_errors = 0;
    b->i2c_errors = 0;
    b->output_dropped_blocks = dropped_blocks;
}

bool imu_block_append(imu_block_t *b, const int16_t xyz[3],
                      uint64_t read_us, uint32_t seq)
{
    if (b->count >= b->capacity || read_us < (uint64_t)b->window_start_us ||
        read_us - (uint64_t)b->window_start_us > UINT32_MAX) {
        b->buffer_dropped++;
        return false;
    }
    size_t n = b->count++;
    memcpy(b->xyz[n], xyz, sizeof(b->xyz[n]));
    b->read_offset_us[n] = (uint32_t)(read_us - (uint64_t)b->window_start_us);
    b->sample_seq[n] = seq;
    return true;
}

uint64_t imu_block_read_us(const imu_block_t *b, size_t index)
{
    return (uint64_t)b->window_start_us + b->read_offset_us[index];
}

float imu_raw_to_si(imu_stream_id_t stream, uint16_t range, int16_t raw)
{
    const float factor = stream == IMU_ACCEL ? 9.80665f : 0.017453292519943295f;
    return (float)raw * (float)range * factor / 32768.0f;
}

static void unpack_xyz(const uint8_t *p, int16_t xyz[3])
{
    for (size_t axis = 0; axis < 3; axis++) {
        uint16_t u = (uint16_t)p[2 * axis] | ((uint16_t)p[2 * axis + 1] << 8);
        int32_t value = u >= 32768u ? (int32_t)u - 65536 : (int32_t)u;
        xyz[axis] = (int16_t)value;
    }
}

imu_fifo_result_t imu_fifo_parse(const uint8_t *data, size_t length,
                                imu_fifo_sample_cb callback, void *context)
{
    imu_fifo_result_t result = {0};
    size_t pos = 0;
    while (pos < length) {
        uint8_t header = data[pos++] & 0xfc; /* discard interrupt tags */
        size_t payload;
        switch (header) {
            case 0x84: case 0x88: payload = 6; break;
            case 0x8c: payload = 12; break;
            case 0x40: payload = 1; break;
            case 0x44: payload = 3; break;
            case 0x48: payload = 4; break;
            case 0x80: return result; /* overread: no valid data after it */
            default: result.invalid_header = true; return result;
        }
        if (length - pos < payload) {
            result.partial_tail = true;
            return result;
        }
        int16_t xyz[3];
        if (header == 0x88 || header == 0x8c) {
            unpack_xyz(data + pos, xyz);
            callback(context, IMU_GYRO, xyz);
        }
        if (header == 0x84 || header == 0x8c) {
            /* FIFO combined frames contain gyro FIRST, unlike data registers. */
            unpack_xyz(data + pos + (header == 0x8c ? 6 : 0), xyz);
            callback(context, IMU_ACCEL, xyz);
        }
        if (header == 0x40) result.skipped_frames += data[pos];
        pos += payload;
    }
    return result;
}
