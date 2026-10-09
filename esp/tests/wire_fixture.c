#include "imu_wire.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void write_frame(FILE *file, const uint8_t *packet, size_t n)
{
    uint8_t prefix[] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    assert(fwrite(prefix, 1, 4, file) == 4);
    assert(fwrite(packet, 1, n, file) == n);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    FILE *file = fopen(argv[1], "wb");
    FILE *json_file = fopen(argv[2], "wb");
    assert(file && json_file);
    uint8_t packet[IMU_WIRE_PACKET_MAX];
    size_t bytes = imu_wire_encode_control(IMU_WIRE_HELLO, 0x1122334455abULL,
                                            0x1122334455667788ULL, 0, 255,
                                            packet, sizeof(packet));
    assert(bytes == IMU_WIRE_HEADER_BYTES);
    write_frame(file, packet, bytes);
    for (unsigned variant = 0; variant < 2; variant++) {
        for (unsigned stream = 0; stream < 2; stream++) {
            uint16_t hz = stream && variant ? 100 : 400;
            uint32_t interval = variant ? 5000 : 1000;
            uint16_t capacity;
            assert(imu_capacity_for(hz, interval, &capacity));
            void *storage = malloc(imu_block_storage_bytes(capacity));
            imu_block_t block = {0};
            assert(imu_block_bind(&block, storage, imu_block_storage_bytes(capacity), capacity));
            int64_t start = 1000000LL + variant * 5000000LL;
            imu_block_init(&block, (imu_stream_id_t)stream, hz, stream ? 2000 : 8,
                            variant, start, 3, interval);
            unsigned samples = hz * interval / 1000;
            for (unsigned i = 0; i < samples; i++) {
                int16_t xyz[] = {-32768, 32767, (int16_t)i};
                assert(imu_block_append(&block, xyz, start + (uint64_t)i * 1000000 / hz,
                                         1000 + i));
            }
            block.window_end_us = start + (int64_t)interval * 1000;
            for (uint16_t part = 0; part < imu_wire_fragment_count(&block); part++) {
                bytes = imu_wire_encode_fragment(&block, 0x1122334455abULL,
                                                  0x1122334455667788ULL + variant,
                                                  part, packet, sizeof(packet));
                assert(bytes >= 96 && bytes <= 1200);
                assert(!imu_wire_encode_fragment(&block, 0, 0, part, packet, 1));
                write_frame(file, packet, bytes);
            }
            assert(!imu_wire_encode_fragment(&block, 0, 0, imu_wire_fragment_count(&block),
                                              packet, sizeof(packet)));
            char json[IMU_WIRE_PACKET_MAX];
            for (uint16_t part = 0; part < imu_json_fragment_count(&block); part++) {
                bytes = imu_json_encode_fragment(&block, 0x1122334455abULL,
                                                  0x1122334455667788ULL + variant,
                                                  part, json, sizeof(json));
                assert(bytes && bytes <= IMU_WIRE_PACKET_MAX);
                assert(!imu_json_encode_fragment(&block, 0, 0, part, json, 1));
                /* Re-encode after the deliberately undersized output test. */
                bytes = imu_json_encode_fragment(&block, 0x1122334455abULL,
                                                  0x1122334455667788ULL + variant,
                                                  part, json, sizeof(json));
                write_frame(json_file, (const uint8_t *)json, bytes);
            }
            free(storage);
        }
    }
    /* Maximum-width identifiers/counters and negative XYZ: JSON stays bounded. */
    uint32_t worst_storage[14 * 6 / 4];
    imu_block_t worst = {0};
    assert(imu_block_bind(&worst, worst_storage, sizeof(worst_storage), 6));
    imu_block_init(&worst, IMU_ACCEL, 400, 16, UINT32_MAX,
                   INT64_MAX - 60000000LL, UINT32_MAX, 60000);
    worst.window_end_us = INT64_MAX;
    worst.buffer_dropped = worst.fifo_errors = worst.i2c_errors = worst.fifo_skipped_frames = UINT32_MAX;
    for (unsigned i = 0; i < 6; i++) {
        int16_t xyz[] = {-32768, -32768, -32768};
        assert(imu_block_append(&worst, xyz, INT64_MAX, UINT32_MAX));
    }
    char json[IMU_WIRE_PACKET_MAX];
    bytes = imu_json_encode_fragment(&worst, UINT64_MAX, UINT64_MAX, 0, json, sizeof(json));
    assert(bytes && bytes < sizeof(json));
    write_frame(json_file, (const uint8_t *)json, bytes);
    assert(fclose(file) == 0);
    assert(fclose(json_file) == 0);
}
