/*
 * daphne_pico_i2c.c - Direct hardware I2C driver for RP2040 (I2C0 on GP4/5 and I2C1 on GP6/7)
 *
 * Implements DW_apb_i2c master operations.
 * Auto-detects whether lines are connected to GP4/GP5 (I2C0) or GP6/GP7 (I2C1).
 */

#include <stdint.h>
#include <stdbool.h>

#include "hardware/structs/i2c.h"
#include "hardware/regs/i2c.h"
#include "hardware/structs/io_bank0.h"
#include "hardware/regs/io_bank0.h"
#include "hardware/structs/pads_bank0.h"
#include "hardware/regs/pads_bank0.h"
#include "hardware/structs/resets.h"
#include "hardware/regs/resets.h"
#include "hardware/structs/sio.h"

#include "daphne_pico_i2c.h"

static bool s_restart_on_next = false;
static i2c_hw_t *s_active_i2c = i2c0_hw;

uint32_t daphne_hw_scan_all_pins_raw(void)
{
    /* Configure GP2..GP28 as SIO inputs with NO pullup and NO pulldown */
    for (int pin = 2; pin <= 28; pin++) {
        io_bank0_hw->io[pin].ctrl = 5; /* GPIO_FUNC_SIO */
        pads_bank0_hw->io[pin] = PADS_BANK0_GPIO0_IE_BITS; /* No PUE, no PDE */
    }

    for (volatile int i = 0; i < 10000; i++) {}

    return (uint32_t)(sio_hw->gpio_in & 0x1FFFFFFF);
}

void daphne_hw_i2c_check_pins(int *out_sda_raw, int *out_scl_raw, int *out_sda_pull, int *out_scl_pull)
{
    /* Check GP4 and GP5 */
    io_bank0_hw->io[4].ctrl = 5;
    io_bank0_hw->io[5].ctrl = 5;

    pads_bank0_hw->io[4] = PADS_BANK0_GPIO0_IE_BITS;
    pads_bank0_hw->io[5] = PADS_BANK0_GPIO0_IE_BITS;

    for (volatile int i = 0; i < 5000; i++) {}

    *out_sda_raw = (sio_hw->gpio_in >> 4) & 1;
    *out_scl_raw = (sio_hw->gpio_in >> 5) & 1;

    pads_bank0_hw->io[4] = PADS_BANK0_GPIO0_IE_BITS | PADS_BANK0_GPIO0_PUE_BITS;
    pads_bank0_hw->io[5] = PADS_BANK0_GPIO0_IE_BITS | PADS_BANK0_GPIO0_PUE_BITS;

    for (volatile int i = 0; i < 5000; i++) {}

    *out_sda_pull = (sio_hw->gpio_in >> 4) & 1;
    *out_scl_pull = (sio_hw->gpio_in >> 5) & 1;
}

void daphne_hw_i2c_init(uint8_t bus_num, uint32_t baudrate)
{
    if (bus_num == 1) {
        /* I2C1 on GP6 (SDA) and GP7 (SCL) */
        s_active_i2c = i2c1_hw;

        resets_hw->reset |= RESETS_RESET_I2C1_BITS;
        resets_hw->reset &= ~RESETS_RESET_I2C1_BITS;
        while (!(resets_hw->reset_done & RESETS_RESET_I2C1_BITS)) {}

        pads_bank0_hw->io[6] = PADS_BANK0_GPIO0_IE_BITS | PADS_BANK0_GPIO0_PUE_BITS;
        pads_bank0_hw->io[7] = PADS_BANK0_GPIO0_IE_BITS | PADS_BANK0_GPIO0_PUE_BITS;
        io_bank0_hw->io[6].ctrl = 3; /* GPIO_FUNC_I2C */
        io_bank0_hw->io[7].ctrl = 3;
    } else {
        /* I2C0 on GP4 (SDA) and GP5 (SCL) */
        s_active_i2c = i2c0_hw;

        resets_hw->reset |= RESETS_RESET_I2C0_BITS;
        resets_hw->reset &= ~RESETS_RESET_I2C0_BITS;
        while (!(resets_hw->reset_done & RESETS_RESET_I2C0_BITS)) {}

        pads_bank0_hw->io[4] = PADS_BANK0_GPIO0_IE_BITS | PADS_BANK0_GPIO0_PUE_BITS;
        pads_bank0_hw->io[5] = PADS_BANK0_GPIO0_IE_BITS | PADS_BANK0_GPIO0_PUE_BITS;
        io_bank0_hw->io[4].ctrl = 3; /* GPIO_FUNC_I2C */
        io_bank0_hw->io[5].ctrl = 3;
    }

    s_active_i2c->enable = 0;

    s_active_i2c->con = (I2C_IC_CON_SPEED_VALUE_FAST << I2C_IC_CON_SPEED_LSB) |
                        I2C_IC_CON_MASTER_MODE_BITS |
                        I2C_IC_CON_IC_SLAVE_DISABLE_BITS |
                        I2C_IC_CON_IC_RESTART_EN_BITS |
                        I2C_IC_CON_TX_EMPTY_CTRL_BITS;

    s_active_i2c->tx_tl = 0;
    s_active_i2c->rx_tl = 0;

    uint32_t freq_in = 125000000UL;
    uint32_t period = (freq_in + baudrate / 2) / baudrate;
    uint32_t lcnt = period * 3 / 5;
    uint32_t hcnt = period - lcnt;

    s_active_i2c->fs_scl_hcnt = hcnt;
    s_active_i2c->fs_scl_lcnt = lcnt;
    s_active_i2c->fs_spklen = lcnt < 16 ? 1 : lcnt / 16;
    s_active_i2c->sda_hold = ((freq_in * 3) / 10000000) + 1;

    s_active_i2c->enable = 1;
    s_restart_on_next = false;
}

int daphne_hw_i2c_write(uint8_t addr, const uint8_t *src, int len, bool nostop)
{
    s_active_i2c->enable = 0;
    s_active_i2c->tar = addr & 0x7F;
    s_active_i2c->enable = 1;

    bool abort = false;
    for (int byte_ctr = 0; byte_ctr < len; ++byte_ctr) {
        bool first = (byte_ctr == 0);
        bool last = (byte_ctr == len - 1);

        uint32_t val = (uint32_t)src[byte_ctr];
        if (first && s_restart_on_next) {
            val |= I2C_IC_DATA_CMD_RESTART_BITS;
        }
        if (last && !nostop) {
            val |= I2C_IC_DATA_CMD_STOP_BITS;
        }
        s_active_i2c->data_cmd = val;

        int timeout = 50000;
        while (!(s_active_i2c->raw_intr_stat & I2C_IC_RAW_INTR_STAT_TX_EMPTY_BITS) && --timeout > 0) {}

        if (s_active_i2c->tx_abrt_source) {
            (void)s_active_i2c->clr_tx_abrt;
            abort = true;
        }

        if (abort || (last && !nostop)) {
            timeout = 50000;
            while (!(s_active_i2c->raw_intr_stat & I2C_IC_RAW_INTR_STAT_STOP_DET_BITS) && --timeout > 0) {}
            (void)s_active_i2c->clr_stop_det;
        }

        if (abort) break;
    }

    s_restart_on_next = nostop;
    return abort ? -1 : len;
}

int daphne_hw_i2c_read(uint8_t addr, uint8_t *dst, int len, bool nostop)
{
    s_active_i2c->enable = 0;
    s_active_i2c->tar = addr & 0x7F;
    s_active_i2c->enable = 1;

    bool abort = false;
    for (int byte_ctr = 0; byte_ctr < len; ++byte_ctr) {
        bool first = (byte_ctr == 0);
        bool last = (byte_ctr == len - 1);

        uint32_t cmd = I2C_IC_DATA_CMD_CMD_BITS;
        if (first && s_restart_on_next) {
            cmd |= I2C_IC_DATA_CMD_RESTART_BITS;
        }
        if (last && !nostop) {
            cmd |= I2C_IC_DATA_CMD_STOP_BITS;
        }
        s_active_i2c->data_cmd = cmd;

        int timeout = 50000;
        do {
            if (s_active_i2c->raw_intr_stat & I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS) {
                abort = true;
                (void)s_active_i2c->clr_tx_abrt;
            }
        } while (!abort && (s_active_i2c->rxflr == 0) && --timeout > 0);

        if (abort || timeout == 0) {
            abort = true;
            break;
        }

        *dst++ = (uint8_t)s_active_i2c->data_cmd;
    }

    if (!nostop) {
        int timeout = 50000;
        while (!abort && !(s_active_i2c->raw_intr_stat & I2C_IC_RAW_INTR_STAT_STOP_DET_BITS) && --timeout > 0) {}
        (void)s_active_i2c->clr_stop_det;
    }

    s_restart_on_next = nostop;
    return abort ? -1 : len;
}

uint32_t daphne_hw_i2c_get_abrt_source(void)
{
    return s_active_i2c->tx_abrt_source;
}
