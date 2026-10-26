/*
 * daphne_i2c.h - I2C0 Master & GY-511 sensor task for Daphne (RP2040 Core 1)
 *
 * Polls LSM303DLHC accelerometer (0x19) and magnetometer (0x1E)
 * on GP4 (SDA) / GP5 (SCL) at 400 kHz Fast-Mode.
 */

#ifndef DAPHNE_I2C_H
#define DAPHNE_I2C_H

#include <tk/tkernel.h>
#include <stdint.h>
/* Start I2C0/I2C1 Master & GY-511 polling task */
EXPORT void daphne_i2c_sensor_task(INT stacd, void *exinf);

/* 9-clock I2C bus recovery routine */
EXPORT void daphne_i2c_bus_recovery(uint8_t bus);
#endif /* DAPHNE_I2C_H */
