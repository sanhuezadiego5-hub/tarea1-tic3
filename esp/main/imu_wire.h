#pragma once
#include "imu_stream.h"

/* Wire v1: explicit big-endian integers, never a dump of a C struct. */
#define IMU_WIRE_HEADER_BYTES 96u
#define IMU_WIRE_RECORD_BYTES 14u
#define IMU_WIRE_PACKET_MAX 1200u
#define IMU_WIRE_SAMPLES_PER_FRAGMENT 78u
#define IMU_JSON_SAMPLES_PER_FRAGMENT 6u
#define IMU_WIRE_HELLO 1u
#define IMU_WIRE_DATA 2u
#define IMU_WIRE_ACK 3u
#define IMU_WIRE_HELLO_ACK 4u
#define IMU_WIRE_START 5u
#define IMU_WIRE_STOP 6u
#define IMU_WIRE_STATE 7u
#define IMU_WIRE_CONTROL_ACK 8u
#define IMU_WIRE_CONFIG 9u
#define IMU_WIRE_CONFIG_OK 10u
#define IMU_WIRE_CONFIG_ERROR 11u
#define IMU_WIRE_FLAG_HOST_TIME 1u
#define IMU_WIRE_FLAG_UNKNOWN_LOSS 2u
#define IMU_WIRE_FLAG_PARTIAL_WINDOW 4u
#define IMU_WIRE_FLAG_SEQUENTIAL 8u

uint16_t imu_wire_fragment_count(const imu_block_t *block);
/* Returns encoded byte count or zero for invalid input/output capacity. */
size_t imu_wire_encode_fragment(const imu_block_t *block, uint64_t device_id,
                                uint64_t boot_id, uint16_t fragment_index,
                                uint8_t *out, size_t capacity);
/* HELLO, HELLO_ACK and ACK share the header, with no sample payload. */
size_t imu_wire_encode_control(uint8_t type, uint64_t device_id, uint64_t boot_id,
                               uint32_t window_seq, uint8_t stream,
                               uint8_t *out, size_t capacity);
uint16_t imu_json_fragment_count(const imu_block_t *block);
size_t imu_json_encode_fragment(const imu_block_t *block, uint64_t device_id,
                                uint64_t boot_id, uint16_t fragment_index,
                                char *out, size_t capacity);
bool imu_wire_control_matches(const uint8_t *data, size_t size, uint8_t type,
                              uint64_t device_id, uint64_t boot_id,
                              uint32_t window_seq, uint8_t stream);

size_t imu_wire_encode_extended(uint8_t type, uint64_t device, uint64_t boot,
    uint32_t seq, uint8_t state, const uint8_t *payload, size_t bytes, uint8_t *out, size_t capacity);
bool imu_wire_extended_matches(const uint8_t *data, size_t size, uint8_t type,
    uint64_t device, uint64_t boot, uint32_t seq, uint8_t state);
