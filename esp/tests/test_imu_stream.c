#include "imu_stream.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static imu_block_t blocks[2];
#define TEST_CAPACITY 512
static uint32_t storage[2][14 * TEST_CAPACITY / sizeof(uint32_t)];
static uint32_t seq[2];

static void collect(void *context, imu_stream_id_t stream, const int16_t xyz[3])
{
    (void)context;
    assert(imu_block_append(&blocks[stream], xyz, 123456, seq[stream]++));
}

static void reset(void)
{
    memset(seq, 0, sizeof(seq));
    for (unsigned i = 0; i < 2; i++)
        assert(imu_block_bind(&blocks[i], storage[i], sizeof(storage[i]), TEST_CAPACITY));
    imu_block_init(&blocks[IMU_ACCEL], IMU_ACCEL, 400, 8, 0, 0, 0, 1000);
    imu_block_init(&blocks[IMU_GYRO], IMU_GYRO, 100, 2000, 0, 0, 0, 1000);
}

static void test_separate_rates(void)
{
    reset();
    /* One nominal second: acceleration 400 XYZ, gyro 100 XYZ.
     * Combined frames put gyro first, acceleration second. */
    const uint8_t acc[] = {0x84, 0xff, 0xff, 0x00, 0x80, 0xff, 0x7f};
    const uint8_t both[] = {0x8c, 1, 0, 2, 0, 3, 0,
                          0xff, 0xff, 0x00, 0x80, 0xff, 0x7f};
    for (unsigned i = 0; i < 400; i++) {
        bool combined = i % 4 == 0;
        imu_fifo_result_t r = imu_fifo_parse(combined ? both : acc,
                                             combined ? sizeof(both) : sizeof(acc),
                                             collect, NULL);
        assert(!r.invalid_header && !r.partial_tail);
    }
    assert(blocks[IMU_ACCEL].count == 400);
    assert(3 * blocks[IMU_ACCEL].count == 1200);
    assert(blocks[IMU_GYRO].count == 100);
    assert(3 * blocks[IMU_GYRO].count == 300);
    assert(blocks[IMU_ACCEL].xyz[399][0] == -1);
    assert(blocks[IMU_ACCEL].xyz[399][1] == -32768);
    assert(blocks[IMU_ACCEL].xyz[399][2] == 32767);
    assert(blocks[IMU_GYRO].xyz[99][0] == 1);
    assert(blocks[IMU_GYRO].sample_seq[99] == 99);
}

static void test_control_and_partial_frames(void)
{
    reset();
    /* Input-config is FOUR payload bytes, timestamp three, skip one. */
    const uint8_t data[] = {0x48, 0, 1, 2, 3, 0x44, 0, 0, 0,
                           0x40, 7, 0x87, 1, 0, 2, 0, 3, 0,
                           0x88, 1, 0};
    imu_fifo_result_t r = imu_fifo_parse(data, sizeof(data), collect, NULL);
    assert(r.skipped_frames == 7 && r.partial_tail && !r.invalid_header);
    assert(blocks[IMU_ACCEL].count == 1 && blocks[IMU_GYRO].count == 0);
    /* The sensor repeats the complete partial frame, not just its tail. */
    const uint8_t repeated[] = {0x88, 1, 0, 2, 0, 3, 0, 0x80, 0};
    r = imu_fifo_parse(repeated, sizeof(repeated), collect, NULL);
    assert(!r.invalid_header && !r.partial_tail);
    assert(blocks[IMU_GYRO].count == 1);
    const uint8_t bad[] = {0x00};
    r = imu_fifo_parse(bad, sizeof(bad), collect, NULL);
    assert(r.invalid_header);
    r = imu_fifo_parse(NULL, 0, collect, NULL);
    assert(!r.invalid_header && !r.partial_tail);
}

static void test_default_400_and_every_truncation(void)
{
    const uint8_t both[] = {0x8c, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0};
    for (size_t cut = 0; cut < sizeof(both); cut++) {
        reset();
        imu_fifo_result_t r = imu_fifo_parse(both, cut, collect, NULL);
        assert(!r.invalid_header && r.partial_tail == (cut != 0));
        assert(blocks[0].count == 0 && blocks[1].count == 0);
    }
    reset();
    blocks[IMU_GYRO].sample_hz = 400;
    for (unsigned i = 0; i < 400; i++) {
        imu_fifo_result_t r = imu_fifo_parse(both, sizeof(both), collect, NULL);
        assert(!r.partial_tail && !r.invalid_header);
    }
    for (unsigned stream = 0; stream < 2; stream++) {
        assert(blocks[stream].count == 400);
        assert(3 * blocks[stream].count == 1200);
    }
    assert(blocks[IMU_ACCEL].xyz[399][0] == 4);
    assert(blocks[IMU_GYRO].xyz[399][0] == 1);
}

static void test_capacity_and_conversion(void)
{
    reset();
    const int16_t xyz[] = {123, -456, 789};
    for (unsigned i = 0; i < TEST_CAPACITY; i++) {
        assert(imu_block_append(&blocks[0], xyz, 999, i));
    }
    assert(!imu_block_append(&blocks[0], xyz, 999, 512));
    assert(blocks[0].count == 512 && blocks[0].buffer_dropped == 1);
    assert(blocks[0].xyz[511][2] == 789);
    assert(fabsf(imu_raw_to_si(IMU_ACCEL, 8, 4096) - 9.80665f) < 0.00001f);
    assert(fabsf(imu_raw_to_si(IMU_ACCEL, 2, 16384) - 9.80665f) < 0.00001f);
    assert(fabsf(imu_raw_to_si(IMU_GYRO, 2000, 16384) - 17.453293f) < 0.00001f);
    imu_block_init(&blocks[0], IMU_ACCEL, 400, 8, 5, 5000000, 2, 1000);
    assert(blocks[0].count == 0 && blocks[0].buffer_dropped == 0);
    assert(blocks[0].window_seq == 5 && blocks[0].output_dropped_blocks == 2);
}

static void test_interval_and_memory_limits(void)
{
    uint16_t a, g;
    assert(imu_capacity_for(400, 1000, &a) && a == 428);
    assert(imu_capacity_for(100, 5000, &g) && g == 533);
    assert(imu_capacity_for(400, 5000, &a) && a == 2108);
    assert(4 * imu_block_storage_bytes(a) == 118048);
    assert(4 * imu_block_storage_bytes(a) <= IMU_POOL_BUDGET_BYTES);
    assert(imu_capacity_for(400, 10000, &a));
    assert(4 * imu_block_storage_bytes(a) > IMU_POOL_BUDGET_BYTES);
    assert(imu_capacity_for(25, 60000, &a));
    assert(4 * imu_block_storage_bytes(a) <= IMU_POOL_BUDGET_BYTES);
    assert(!imu_capacity_for(400, 0, &a));
    assert(!imu_capacity_for(400, 99, &a));
    assert(!imu_capacity_for(400, 60001, &a));
    assert(!imu_capacity_for(123, 1000, &a));
    assert(!imu_capacity_for(400, 1000, NULL));
    reset();
    assert(!imu_block_bind(&blocks[0], storage[0], 1, 512));
    assert(!imu_block_bind(&blocks[0], (uint8_t *)storage[0] + 1, sizeof(storage[0]) - 1, 512));
    const int16_t xyz[] = {0, 0, 0};
    imu_block_init(&blocks[0], IMU_ACCEL, 400, 8, 0, 1000000, 0, 5000);
    assert(imu_block_append(&blocks[0], xyz, 6000000, 123));
    assert(imu_block_read_us(&blocks[0], 0) == 6000000);
    assert(!imu_block_append(&blocks[0], xyz, 999999, 124));
    assert(!imu_block_append(&blocks[0], xyz, 1000000ULL + UINT32_MAX + 1, 125));
}

int main(void)
{
    test_separate_rates();
    test_control_and_partial_frames();
    test_default_400_and_every_truncation();
    test_capacity_and_conversion();
    test_interval_and_memory_limits();
    puts("PASS: independent rates/XYZ, FIFO ordering/partial/control frames, capacity and scaling");
}
