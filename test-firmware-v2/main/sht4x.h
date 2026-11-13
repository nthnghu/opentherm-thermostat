#pragma once
// Sensirion SHT40/SHT45 on the shared I2C bus (0x44).
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

esp_err_t sht4x_init(i2c_master_bus_handle_t bus);
esp_err_t sht4x_read_serial(uint32_t *serial);
// High-precision measurement (~9 ms). Values in °C and %RH.
esp_err_t sht4x_measure(float *temp_c, float *rh);
