#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
prueba_dir=$(mktemp -d)
trap 'rm -rf "$prueba_dir"' EXIT
cc -std=c11 -Wall -Wextra -Werror -I esp/main esp/main/imu_stream.c esp/tests/test_imu_stream.c -lm -o "$prueba_dir/stream"
"$prueba_dir/stream"
cc -std=c11 -Wall -Wextra -Werror -I esp/main esp/main/wifi_policy.c esp/tests/test_wifi_policy.c -o "$prueba_dir/wifi"
"$prueba_dir/wifi"
cc -std=c11 -Wall -Wextra -Werror -I esp/main esp/main/imu_stream.c esp/main/imu_wire.c esp/tests/wire_fixture.c -o "$prueba_dir/wire"
"$prueba_dir/wire" "$prueba_dir/wire.bin" "$prueba_dir/json.bin"
cat > "$prueba_dir/sdkconfig.h" <<'CONFIG'
#define CONFIG_TIC3_ACCEL_HZ 400
#define CONFIG_TIC3_GYRO_HZ 400
#define CONFIG_TIC3_INTERVAL_MS 1000
#define CONFIG_TIC3_BLOCKS_PER_STREAM 2
CONFIG
cc -std=c11 -Wall -Wextra -Werror -I "$prueba_dir" -I esp/main esp/main/imu_stream.c esp/main/imu_wire.c esp/main/imu_config.c esp/tests/test_imu_config.c -o "$prueba_dir/config"
"$prueba_dir/config" "$prueba_dir/config.bin"
PYTHONPATH="$PWD/ejemplos_tcp${PYTHONPATH:+:$PYTHONPATH}" TIC3_WIRE_FIXTURE="$prueba_dir/wire.bin" TIC3_JSON_FIXTURE="$prueba_dir/json.bin" TIC3_CONFIG_FIXTURE="$prueba_dir/config.bin" python3 -m unittest discover -s raspi -p 'test_*.py' -v
