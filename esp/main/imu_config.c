#include "imu_config.h"
#include "imu_stream.h"
#include "sdkconfig.h"
#include <string.h>
static void p16(uint8_t *p, uint16_t v) { p[0]=v>>8; p[1]=v; }
static void p32(uint8_t *p, uint32_t v) { for (int i=0;i<4;i++) p[i]=v>>(24-8*i); }
static uint16_t g16(const uint8_t *p) { return ((uint16_t)p[0]<<8)|p[1]; }
static uint32_t g32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
void imu_config_defaults(imu_config_t *c) {
    *c=(imu_config_t){.accel_hz=CONFIG_TIC3_ACCEL_HZ,.gyro_hz=CONFIG_TIC3_GYRO_HZ,
        .accel_range=8,.gyro_range=2000,.interval_ms=CONFIG_TIC3_INTERVAL_MS,
        .parallel=1,.blocks=CONFIG_TIC3_BLOCKS_PER_STREAM};
#ifdef CONFIG_TIC3_FORMAT_JSON
    c->json=1;
#endif
    /* TCP is always the boot default; Raspberry can select UDP remotely. */
}
bool imu_config_valid(const imu_config_t *c) {
    uint16_t a,g;
    if (!c || !imu_capacity_for(c->accel_hz,c->interval_ms,&a) ||
        !imu_capacity_for(c->gyro_hz,c->interval_ms,&g)) return false;
    if (!(c->accel_range==2||c->accel_range==4||c->accel_range==8||c->accel_range==16)||
        !(c->gyro_range==125||c->gyro_range==250||c->gyro_range==500||c->gyro_range==1000||c->gyro_range==2000)||
        c->parallel>1||c->udp>1||c->json>1||c->blocks<2||c->blocks>4||
        c->accel_samples>10000000||c->gyro_samples>10000000) return false;
    return c->blocks*(imu_block_storage_bytes(a)+imu_block_storage_bytes(g))<=IMU_POOL_BUDGET_BYTES;
}
void imu_config_pack(const imu_config_t *c,uint8_t out[IMU_CONFIG_BYTES]) {
    memset(out,0,IMU_CONFIG_BYTES);p16(out,c->accel_hz);p16(out+2,c->gyro_hz);
    p16(out+4,c->accel_range);p16(out+6,c->gyro_range);p32(out+8,c->interval_ms);
    p32(out+12,c->accel_samples);p32(out+16,c->gyro_samples);
    out[20]=c->parallel;out[21]=c->udp;out[22]=c->json;out[23]=c->blocks;
}
bool imu_config_unpack(const uint8_t *p,uint32_t length,imu_config_t *c) {
    if (!p||!c||length!=IMU_CONFIG_BYTES) return false;
    for (int i=24;i<32;i++) if(p[i]) return false;
    *c=(imu_config_t){.accel_hz=g16(p),.gyro_hz=g16(p+2),.accel_range=g16(p+4),
        .gyro_range=g16(p+6),.interval_ms=g32(p+8),.accel_samples=g32(p+12),.gyro_samples=g32(p+16),
        .parallel=p[20],.udp=p[21],.json=p[22],.blocks=p[23]};
    return imu_config_valid(c);
}
