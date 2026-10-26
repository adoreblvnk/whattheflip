/*
 * daphne_i2c.c - I2C Master & GY-511 sensor implementation for Daphne
 *
 * Invariants:
 * - Dedicated to Processor 2 (Core 1) under TA_ASSPRC.
 * - Scans both I2C0 (GP4/GP5) and I2C1 (GP6/GP7) to handle socket/board wiring variations.
 * - LSM303DLHC: Accel at 0x19, Mag at 0x1E.
 * - Multi-byte reads on Accel MUST set bit 7 (0x80) on register address (OUT_X_L_A | 0x80 = 0xA8).
 * - Implements 8-sample moving average filter.
 * - Zero dynamic memory allocation.
 */

#include "daphne_i2c.h"
#include "daphne_pico_i2c.h"
#include <tm/tmonitor.h>
#include <stdint.h>
#include <stdbool.h>

/* Sensor Addresses */
#define LSM303_ACCEL_ADDR       0x19
#define LSM303_MAG_ADDR         0x1E

/* Sensor Registers */
#define LSM303_CTRL_REG1_A      0x20
#define LSM303_CTRL_REG4_A      0x23
#define LSM303_OUT_X_L_A        0x28
#define LSM303_AUTO_INC         0x80

#define LSM303_CRA_REG_M        0x00
#define LSM303_CRB_REG_M        0x01
#define LSM303_MR_REG_M         0x02
#define LSM303_OUT_X_H_M        0x03

/* Filter Window */
#define FILTER_SIZE             8

typedef struct {
    int32_t x[FILTER_SIZE];
    int32_t y[FILTER_SIZE];
    int32_t z[FILTER_SIZE];
    uint32_t idx;
    uint32_t count;
} filter_t;

LOCAL filter_t s_accel_filt;

LOCAL void filter_init(filter_t *f)
{
    f->idx = 0;
    f->count = 0;
    for (int i = 0; i < FILTER_SIZE; i++) {
        f->x[i] = 0;
        f->y[i] = 0;
        f->z[i] = 0;
    }
}

LOCAL void filter_update(filter_t *f, int16_t x, int16_t y, int16_t z,
                         int16_t *out_x, int16_t *out_y, int16_t *out_z)
{
    f->x[f->idx] = (int32_t)x;
    f->y[f->idx] = (int32_t)y;
    f->z[f->idx] = (int32_t)z;
    f->idx = (f->idx + 1) % FILTER_SIZE;
    if (f->count < FILTER_SIZE) {
        f->count++;
    }

    int32_t sx = 0, sy = 0, sz = 0;
    for (uint32_t i = 0; i < f->count; i++) {
        sx += f->x[i];
        sy += f->y[i];
        sz += f->z[i];
    }
    *out_x = (int16_t)(sx / (int32_t)f->count);
    *out_y = (int16_t)(sy / (int32_t)f->count);
    *out_z = (int16_t)(sz / (int32_t)f->count);
}

LOCAL int daphne_i2c_bus_scan(uint8_t bus)
{
    tm_printf((UB *)"[DAPHNE:I2C] Scanning I2C%d (%s)...\n",
              bus, (bus == 1) ? "GP6/GP7" : "GP4/GP5");
    int found = 0;

    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        uint8_t dummy = 0;
        int ret = daphne_hw_i2c_read(addr, &dummy, 1, false);
        if (ret >= 0) {
            found++;
            if (addr == LSM303_ACCEL_ADDR) {
                tm_printf((UB *)"[DAPHNE:I2C] -> Found 0x%02X: LSM303DLHC Accelerometer on I2C%d!\n", addr, bus);
            } else if (addr == LSM303_MAG_ADDR) {
                tm_printf((UB *)"[DAPHNE:I2C] -> Found 0x%02X: LSM303DLHC Magnetometer on I2C%d!\n", addr, bus);
            } else {
                tm_printf((UB *)"[DAPHNE:I2C] -> Found 0x%02X: Active Device on I2C%d\n", addr, bus);
            }
        }
    }
    if (found == 0) {
        tm_printf((UB *)"[DAPHNE:I2C] I2C%d: 0 devices responded.\n", bus);
    }
    return found;
}

EXPORT void daphne_i2c_sensor_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

    filter_init(&s_accel_filt);

    /* 1. Universal GPIO scan (GP2..GP28) with NO pullup/pulldown to find any pulled-up line */
    uint32_t raw_pins = daphne_hw_scan_all_pins_raw();
    tm_printf((UB *)"[DAPHNE:I2C] Raw pin mask (no pull): 0x%08X (", raw_pins);
    int high_count = 0;
    for (int p = 2; p <= 28; p++) {
        if ((raw_pins >> p) & 1) {
            tm_printf((UB *)"GP%d ", p);
            high_count++;
        }
    }
    if (high_count == 0) {
        tm_printf((UB *)"ALL 0V)\n");
    } else {
        tm_printf((UB *)"are HIGH)\n");
    }

    /* Wait 50 ms for power rail */
    tk_dly_tsk(50);

    /* 2. Probe I2C0 (GP4/GP5) */
    daphne_hw_i2c_init(0, 100000);
    int found0 = daphne_i2c_bus_scan(0);

    /* 3. Probe I2C1 (GP6/GP7) */
    daphne_hw_i2c_init(1, 100000);
    int found1 = daphne_i2c_bus_scan(1);

    uint8_t active_bus = 0;
    if (found1 > 0) {
        active_bus = 1;
    } else if (found0 > 0) {
        active_bus = 0;
    } else {
        /* Default to GP6/GP7 if GP6/GP7 are HIGH in raw pin scan, else GP4/GP5 */
        active_bus = ((raw_pins & (1 << 6)) && (raw_pins & (1 << 7))) ? 1 : 0;
    }

    tm_printf((UB *)"[DAPHNE:I2C] Selected active bus: I2C%d (%s @ 400 kHz)\n",
              active_bus, (active_bus == 1) ? "GP6/GP7" : "GP4/GP5");

    daphne_hw_i2c_init(active_bus, 400000);

    /* Configure LSM303DLHC Accelerometer */
    uint8_t cfg_a1[2] = {LSM303_CTRL_REG1_A, 0x77}; /* 400 Hz ODR, all axes */
    uint8_t cfg_a4[2] = {LSM303_CTRL_REG4_A, 0x08}; /* High-Res ±2g */
    int ret = daphne_hw_i2c_write(LSM303_ACCEL_ADDR, cfg_a1, 2, false);
    if (ret < 0) {
        tm_printf((UB *)"[DAPHNE:I2C] ERROR: Accel CTRL_REG1_A init failed (err=%d, abrt=0x%04X)\n",
                  ret, daphne_hw_i2c_get_abrt_source());
    }
    ret = daphne_hw_i2c_write(LSM303_ACCEL_ADDR, cfg_a4, 2, false);
    if (ret < 0) {
        tm_printf((UB *)"[DAPHNE:I2C] ERROR: Accel CTRL_REG4_A init failed (err=%d, abrt=0x%04X)\n",
                  ret, daphne_hw_i2c_get_abrt_source());
    }

    /* Configure LSM303DLHC Magnetometer */
    uint8_t cfg_m_cra[2] = {LSM303_CRA_REG_M, 0x14}; /* 30 Hz ODR */
    uint8_t cfg_m_crb[2] = {LSM303_CRB_REG_M, 0x20}; /* ±1.3 Gauss */
    uint8_t cfg_m_mr[2]  = {LSM303_MR_REG_M,  0x00}; /* Continuous conversion */
    ret = daphne_hw_i2c_write(LSM303_MAG_ADDR, cfg_m_cra, 2, false);
    if (ret < 0) {
        tm_printf((UB *)"[DAPHNE:I2C] ERROR: Mag CRA_REG_M init failed (err=%d, abrt=0x%04X)\n",
                  ret, daphne_hw_i2c_get_abrt_source());
    }
    ret = daphne_hw_i2c_write(LSM303_MAG_ADDR, cfg_m_crb, 2, false);
    if (ret < 0) {
        tm_printf((UB *)"[DAPHNE:I2C] ERROR: Mag CRB_REG_M init failed (err=%d, abrt=0x%04X)\n",
                  ret, daphne_hw_i2c_get_abrt_source());
    }
    ret = daphne_hw_i2c_write(LSM303_MAG_ADDR, cfg_m_mr, 2, false);
    if (ret < 0) {
        tm_printf((UB *)"[DAPHNE:I2C] ERROR: Mag MR_REG_M init failed (err=%d, abrt=0x%04X)\n",
                  ret, daphne_hw_i2c_get_abrt_source());
    }

    uint8_t raw_buf[6];
    uint32_t poll_count = 0;
    uint32_t i2c_fault_count = 0;

    for (;;) {
        poll_count++;

        /* 1. Read Accelerometer (6 bytes starting at OUT_X_L_A with 0x80 auto-increment) */
        uint8_t reg_accel = LSM303_OUT_X_L_A | LSM303_AUTO_INC;
        ret = daphne_hw_i2c_write(LSM303_ACCEL_ADDR, &reg_accel, 1, true);
        if (ret >= 0) {
            ret = daphne_hw_i2c_read(LSM303_ACCEL_ADDR, raw_buf, 6, false);
        }

        if (ret >= 0) {
            int16_t raw_x = (int16_t)(((uint16_t)raw_buf[1] << 8) | raw_buf[0]) >> 4;
            int16_t raw_y = (int16_t)(((uint16_t)raw_buf[3] << 8) | raw_buf[2]) >> 4;
            int16_t raw_z = (int16_t)(((uint16_t)raw_buf[5] << 8) | raw_buf[4]) >> 4;

            int16_t avg_x = 0, avg_y = 0, avg_z = 0;
            filter_update(&s_accel_filt, raw_x, raw_y, raw_z, &avg_x, &avg_y, &avg_z);

            /* 2. Read Magnetometer (6 bytes starting at OUT_X_H_M) */
            uint8_t reg_mag = LSM303_OUT_X_H_M;
            int mret = daphne_hw_i2c_write(LSM303_MAG_ADDR, &reg_mag, 1, true);
            if (mret >= 0) {
                mret = daphne_hw_i2c_read(LSM303_MAG_ADDR, raw_buf, 6, false);
            }

            if (mret >= 0) {
                int16_t mag_x = (int16_t)(((uint16_t)raw_buf[0] << 8) | raw_buf[1]);
                int16_t mag_z = (int16_t)(((uint16_t)raw_buf[2] << 8) | raw_buf[3]);
                int16_t mag_y = (int16_t)(((uint16_t)raw_buf[4] << 8) | raw_buf[5]);

                if ((poll_count % 5) == 0 || poll_count <= 3) {
                    tm_printf((UB *)"[DAPHNE:I2C] ACCEL mg: X=%d Y=%d Z=%d (avg: X=%d Y=%d Z=%d) | MAG: X=%d Y=%d Z=%d\n",
                              raw_x, raw_y, raw_z, avg_x, avg_y, avg_z, mag_x, mag_y, mag_z);
                }
            } else {
                i2c_fault_count++;
                tm_printf((UB *)"[DAPHNE:I2C] FAULT: Mag read failed (err=%d, abrt=0x%04X, faults=%u)\n",
                          mret, daphne_hw_i2c_get_abrt_source(), i2c_fault_count);
            }
        } else {
            i2c_fault_count++;
            tm_printf((UB *)"[DAPHNE:I2C] FAULT: Accel read failed (err=%d, abrt=0x%04X, faults=%u)\n",
                      ret, daphne_hw_i2c_get_abrt_source(), i2c_fault_count);
        }

        /* 200 ms sample period (5 Hz logging rate) */
        tk_dly_tsk(200);
    }
}
