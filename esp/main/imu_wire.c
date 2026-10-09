#include "imu_wire.h"
#include <string.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8); p[1] = (uint8_t)value;
}
static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (24 - 8 * i));
}
static void put64(uint8_t *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(value >> (56 - 8 * i));
}

static uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    return crc;
}

static void finish_crc(uint8_t *out, size_t payload_bytes)
{
    uint32_t crc = crc_update(UINT32_MAX, out, 92);
    crc = crc_update(crc, out + IMU_WIRE_HEADER_BYTES, payload_bytes);
    put32(out + 92, crc ^ UINT32_MAX);
}

bool imu_wire_control_matches(const uint8_t *data, size_t size, uint8_t type,
                              uint64_t device_id, uint64_t boot_id,
                              uint32_t seq, uint8_t stream)
{
    uint8_t expected[IMU_WIRE_HEADER_BYTES];
    if (!data || size != sizeof(expected) ||
        !imu_wire_encode_control(type, device_id, boot_id, seq, stream, expected, sizeof(expected)))
        return false;
    return memcmp(data, expected, sizeof(expected)) == 0;
}

static uint16_t fragment_count(const imu_block_t *b, unsigned per_fragment)
{
    return b->count ? (uint16_t)((b->count + per_fragment - 1u) / per_fragment) : 1u;
}

uint16_t imu_wire_fragment_count(const imu_block_t *b)
{
    return fragment_count(b, IMU_WIRE_SAMPLES_PER_FRAGMENT);
}

uint16_t imu_json_fragment_count(const imu_block_t *b)
{
    return fragment_count(b, IMU_JSON_SAMPLES_PER_FRAGMENT);
}

static size_t encode_fragment(const imu_block_t *b, uint64_t device_id,
                              uint64_t boot_id, uint16_t fragment,
                              uint8_t *out, size_t capacity, unsigned per_fragment)
{
    if (!b || !out || b->count > b->capacity || b->stream > IMU_GYRO) return 0;
    uint16_t fragments = fragment_count(b, per_fragment);
    if (fragment >= fragments) return 0;
    uint32_t offset = (uint32_t)fragment * per_fragment;
    uint32_t n = b->count - offset;
    if (n > per_fragment) n = per_fragment;
    size_t payload = (size_t)n * IMU_WIRE_RECORD_BYTES;
    size_t total = IMU_WIRE_HEADER_BYTES + payload;
    if (capacity < total || (n && (!b->xyz || !b->sample_seq || !b->read_offset_us))) return 0;
    memset(out, 0, IMU_WIRE_HEADER_BYTES);
    memcpy(out, "TIC3", 4);
    out[4] = 1;
    out[5] = IMU_WIRE_DATA;
    out[6] = (uint8_t)b->stream;
    out[7] = IMU_WIRE_FLAG_HOST_TIME | b->flags;
    if (b->fifo_errors || b->fifo_skipped_frames >= 255)
        out[7] |= IMU_WIRE_FLAG_UNKNOWN_LOSS;
    put64(out + 8, device_id);
    put64(out + 16, boot_id);
    put32(out + 24, b->window_seq);
    put16(out + 28, b->sample_hz);
    put16(out + 30, b->range);
    put32(out + 32, b->interval_ms);
    put64(out + 36, (uint64_t)b->window_start_us);
    put64(out + 44, (uint64_t)b->window_end_us);
    put32(out + 52, b->count);
    put16(out + 56, fragment);
    put16(out + 58, fragments);
    put32(out + 60, offset);
    put32(out + 64, n);
    put32(out + 68, b->buffer_dropped);
    put32(out + 72, b->fifo_skipped_frames);
    put32(out + 76, b->fifo_errors);
    put32(out + 80, b->i2c_errors);
    put32(out + 84, b->output_dropped_blocks);
    put32(out + 88, (uint32_t)payload);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *p = out + IMU_WIRE_HEADER_BYTES + i * IMU_WIRE_RECORD_BYTES;
        put32(p, b->sample_seq[offset + i]);
        put32(p + 4, b->read_offset_us[offset + i]);
        for (unsigned axis = 0; axis < 3; axis++)
            put16(p + 8 + 2 * axis, (uint16_t)b->xyz[offset + i][axis]);
    }
    finish_crc(out, payload);
    return total;
}

size_t imu_wire_encode_fragment(const imu_block_t *b, uint64_t device_id,
                                uint64_t boot_id, uint16_t fragment,
                                uint8_t *out, size_t capacity)
{
    return encode_fragment(b, device_id, boot_id, fragment, out, capacity,
                            IMU_WIRE_SAMPLES_PER_FRAGMENT);
}

typedef struct { char *out; size_t size; size_t used; bool failed; } json_writer_t;

static void json_append(json_writer_t *w, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void json_append(json_writer_t *w, const char *format, ...)
{
    if (w->failed) return;
    va_list args;
    va_start(args, format);
    int n = vsnprintf(w->out + w->used, w->size - w->used, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= w->size - w->used) w->failed = true;
    else w->used += (size_t)n;
}

size_t imu_json_encode_fragment(const imu_block_t *b, uint64_t device_id,
                                uint64_t boot_id, uint16_t fragment,
                                char *out, size_t capacity)
{
    if (!out || !capacity) return 0;
    uint8_t binary[IMU_WIRE_HEADER_BYTES + IMU_JSON_SAMPLES_PER_FRAGMENT * IMU_WIRE_RECORD_BYTES];
    size_t bytes = encode_fragment(b, device_id, boot_id, fragment, binary, sizeof(binary),
                                   IMU_JSON_SAMPLES_PER_FRAGMENT);
    if (!bytes) return 0;
    uint32_t crc = 0;
    for (unsigned i = 0; i < 4; i++) crc = (crc << 8) | binary[92 + i];
    unsigned offset = fragment * IMU_JSON_SAMPLES_PER_FRAGMENT;
    unsigned n = (unsigned)(bytes - IMU_WIRE_HEADER_BYTES) / IMU_WIRE_RECORD_BYTES;
    json_writer_t w = {.out = out, .size = capacity};
    json_append(&w, "{\"magic\":\"TIC3\",\"version\":1,\"type\":2,\"stream\":%u,\"flags\":%u"
                ",\"device_id\":\"%016" PRIx64 "\",\"boot_id\":\"%016" PRIx64 "\""
                ",\"window_seq\":%" PRIu32 ",\"sample_hz\":%u,\"range\":%u"
                ",\"interval_ms\":%" PRIu32 ",\"window_start_us\":%" PRId64
                ",\"window_end_us\":%" PRId64 ",\"sample_count\":%u"
                ",\"fragment_index\":%u,\"fragment_count\":%u,\"sample_offset\":%u"
                ",\"fragment_samples\":%u,\"buffer_dropped\":%" PRIu32
                ",\"fifo_skipped_frames\":%" PRIu32 ",\"fifo_errors\":%" PRIu32
                ",\"i2c_errors\":%" PRIu32 ",\"output_dropped_blocks\":%" PRIu32
                ",\"payload_bytes\":%u,\"crc32\":%" PRIu32 ",\"values\":[",
                (unsigned)b->stream, binary[7], device_id, boot_id, b->window_seq,
                b->sample_hz, b->range, b->interval_ms, b->window_start_us,
                b->window_end_us, b->count, fragment, imu_json_fragment_count(b),
                offset, n, b->buffer_dropped, b->fifo_skipped_frames, b->fifo_errors,
                b->i2c_errors, b->output_dropped_blocks, n * IMU_WIRE_RECORD_BYTES, crc);
    for (unsigned i = 0; i < n; i++)
        json_append(&w, "%s%d,%d,%d", i ? "," : "", b->xyz[offset + i][0],
                      b->xyz[offset + i][1], b->xyz[offset + i][2]);
    json_append(&w, "],\"sample_seq\":[");
    for (unsigned i = 0; i < n; i++)
        json_append(&w, "%s%" PRIu32, i ? "," : "", b->sample_seq[offset + i]);
    json_append(&w, "],\"read_offset_us\":[");
    for (unsigned i = 0; i < n; i++)
        json_append(&w, "%s%" PRIu32, i ? "," : "", b->read_offset_us[offset + i]);
    json_append(&w, "]}");
    return w.failed || w.used > IMU_WIRE_PACKET_MAX ? 0 : w.used;
}

size_t imu_wire_encode_control(uint8_t type, uint64_t device_id, uint64_t boot_id,
                               uint32_t window_seq, uint8_t stream,
                               uint8_t *out, size_t capacity)
{
    if (!out || capacity < IMU_WIRE_HEADER_BYTES ||
        ((type < IMU_WIRE_ACK || type > IMU_WIRE_CONFIG_ERROR) && type != IMU_WIRE_HELLO)) return 0;
    memset(out, 0, IMU_WIRE_HEADER_BYTES);
    memcpy(out, "TIC3", 4);
    out[4] = 1; out[5] = type; out[6] = stream;
    put64(out + 8, device_id); put64(out + 16, boot_id);
    put32(out + 24, window_seq);
    finish_crc(out, 0);
    return IMU_WIRE_HEADER_BYTES;
}

size_t imu_wire_encode_extended(uint8_t type, uint64_t device, uint64_t boot,
    uint32_t seq, uint8_t state, const uint8_t *payload, size_t bytes, uint8_t *out, size_t capacity)
{
    if (bytes > 32 || (bytes && !payload) || capacity < IMU_WIRE_HEADER_BYTES + bytes ||
        !imu_wire_encode_control(type, device, boot, seq, state, out, capacity)) return 0;
    if (bytes) memcpy(out + IMU_WIRE_HEADER_BYTES, payload, bytes);
    put32(out + 88, bytes); finish_crc(out, bytes);
    return IMU_WIRE_HEADER_BYTES + bytes;
}
bool imu_wire_extended_matches(const uint8_t *data, size_t size, uint8_t type,
    uint64_t device, uint64_t boot, uint32_t seq, uint8_t state)
{
    uint8_t expected[IMU_WIRE_HEADER_BYTES + 32];
    if (!data || size < IMU_WIRE_HEADER_BYTES || size > sizeof(expected)) return false;
    size_t n = imu_wire_encode_extended(type, device, boot, seq, state,
        data + IMU_WIRE_HEADER_BYTES, size - IMU_WIRE_HEADER_BYTES, expected, sizeof(expected));
    return n == size && memcmp(data, expected, size) == 0;
}
