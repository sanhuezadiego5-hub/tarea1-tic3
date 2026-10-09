idf_component_register(SRCS "main.c" "imu_stream.c" "imu_wire.c" "imu_wifi.c" "wifi_policy.c" "imu_transport.c" "imu_config.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_driver_i2c esp_timer esp_wifi esp_netif esp_event nvs_flash lwip
                    )
