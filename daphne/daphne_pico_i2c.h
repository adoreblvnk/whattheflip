#ifndef DAPHNE_PICO_I2C_H
#define DAPHNE_PICO_I2C_H

#include <stdint.h>
#include <stdbool.h>

uint32_t daphne_hw_scan_all_pins_raw(void);
void daphne_hw_i2c_check_pins(int *out_sda_raw, int *out_scl_raw, int *out_sda_pull, int *out_scl_pull);
void daphne_hw_i2c_init(uint8_t bus_num, uint32_t baudrate);
int daphne_hw_i2c_write(uint8_t addr, const uint8_t *src, int len, bool nostop);
int daphne_hw_i2c_read(uint8_t addr, uint8_t *dst, int len, bool nostop);
uint32_t daphne_hw_i2c_get_abrt_source(void);
void daphne_hw_i2c_bus_recovery(uint8_t bus_num);
void daphne_hw_gpio_init_out(uint32_t pin);
void daphne_hw_gpio_set(uint32_t pin, bool level);
void daphne_hw_buzzer_beep(uint32_t duration_ms);
#endif /* DAPHNE_PICO_I2C_H */
