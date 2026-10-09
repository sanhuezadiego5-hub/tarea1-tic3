#include "imu_config.h"
#include "imu_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv) {
    imu_config_t c,d;uint8_t payload[32],packet[128];imu_config_defaults(&c);
    c.accel_hz=200;c.gyro_hz=100;c.accel_range=4;c.gyro_range=500;
    c.accel_samples=500;c.gyro_samples=200;c.parallel=0;c.udp=1;c.json=1;
    assert(imu_config_valid(&c));imu_config_pack(&c,payload);
    assert(imu_config_unpack(payload,sizeof(payload),&d));assert(d.gyro_hz==100&&d.udp==1&&d.parallel==0);
    size_t n=imu_wire_encode_extended(9,10,20,3,255,payload,32,packet,sizeof(packet));assert(n==128);
    assert(imu_wire_extended_matches(packet,n,9,10,20,3,255));
    if(argc==2){FILE *f=fopen(argv[1],"wb");assert(f);assert(fwrite(packet,1,n,f)==n);fclose(f);}
    packet[120]^=1;assert(!imu_wire_extended_matches(packet,n,9,10,20,3,255));
    c.interval_ms=60000;assert(!imu_config_valid(&c));c.interval_ms=1000;
    c.accel_hz=30;assert(!imu_config_valid(&c));c.accel_hz=200;
    c.accel_samples=10000001;assert(!imu_config_valid(&c));
    return 0;
}
