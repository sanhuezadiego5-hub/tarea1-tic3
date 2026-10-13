#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "imu_config.h"
/* Inicializa I2C y BMI270. No iniciar dos veces ni llamar desde red/GUI. */
esp_err_t bmi270_sensor_start(const imu_config_t *config);
/* Cambio de ODR/rangos y lectura de retorno para verificar su aplicación. */
esp_err_t bmi270_sensor_configure(const imu_config_t *config);
bool bmi270_sensor_verify(const imu_config_t *config);
/* Acceso a registros/FIFO; los errores los decide la capa de adquisición. */
esp_err_t bmi270_sensor_read_registers(uint8_t reg,uint8_t *data,size_t length);
esp_err_t bmi270_sensor_write_register(uint8_t reg,uint8_t value);
