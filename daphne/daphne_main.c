/*
 * daphne_main.c - Main entry point for Daphne (RP2040 dual-core SMP)
 *
 * Invariants:
 * - µT-Kernel 3.0 SMP (SMP=1, IEEE 2050-2018).
 * - Core 0 (Processor 1): USB-CDC console + UART0 RX task on GP1.
 * - Core 1 (Processor 2): I2C0 Master & GY-511 sensor polling task on GP4/GP5.
 * - Strict Barr C compliance: no dynamic memory allocation, static stacks.
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <tk/smp.h>

#include "daphne_uart.h"
#include "daphne_i2c.h"

#define DAPHNE_STACK_SZ     4096

/* Task Creation Control Blocks with Static Affinity */
LOCAL T_CTSK ctsk_uart = {
    .exinf      = NULL,
    .tskatr     = TA_HLNG | TA_RNG3 | TA_ASSPRC,
    .task       = daphne_uart_rx_task,
    .itskpri    = 9,
    .stksz      = DAPHNE_STACK_SZ,
    .bufptr     = NULL,
    .assprc     = TP_PRC1, /* Bound to Processor 1 (physical Core 0) */
};

LOCAL T_CTSK ctsk_i2c = {
    .exinf      = NULL,
    .tskatr     = TA_HLNG | TA_RNG3 | TA_ASSPRC,
    .task       = daphne_i2c_sensor_task,
    .itskpri    = 10,
    .stksz      = DAPHNE_STACK_SZ,
    .bufptr     = NULL,
    .assprc     = TP_PRC2, /* Bound to Processor 2 (physical Core 1) */
};

LOCAL BOOL make_task(const char *name, ID *out_id, T_CTSK *pk_ctsk)
{
    ID id = tk_cre_tsk(pk_ctsk);
    if (id <= E_OK) {
        tm_printf((UB *)"[DAPHNE:INIT] FATAL: tk_cre_tsk(%s) failed, err=%d\n", name, id);
        return FALSE;
    }
    ER err = tk_sta_tsk(id, 0);
    if (err != E_OK) {
        tm_printf((UB *)"[DAPHNE:INIT] FATAL: tk_sta_tsk(%s) failed, err=%d\n", name, err);
        return FALSE;
    }
    *out_id = id;
    return TRUE;
}

EXPORT INT usermain(void)
{
    ID uart_tid = 0;
    ID i2c_tid = 0;

    tm_printf((UB *)"\n==================================================\n");
    tm_printf((UB *)"  DAPHNE - Victim B Firmware (µT-Kernel 3.0 SMP) \n");
    tm_printf((UB *)"  Core 0: UART0 RX (GP1 @ 115200)               \n");
    tm_printf((UB *)"  Core 1: GY-511 I2C0 Master (GP4/GP5 @ 400kHz)  \n");
    tm_printf((UB *)"==================================================\n");

    if (!make_task("UART_RX", &uart_tid, &ctsk_uart)) {
        return 1;
    }

    if (!make_task("I2C_SENSOR", &i2c_tid, &ctsk_i2c)) {
        return 1;
    }

    /* Initial task sleeps forever; scheduler runs the application tasks */
    tk_slp_tsk(TMO_FEVR);
    return 0;
}
