/*
 * hestia_main.c - Main entry point for Hestia (Victim A, RP2040)
 *
 * Invariants:
 * - µT-Kernel 3.0 SMP (SMP=1, IEEE 2050-2018).
 * - Core 0 (Processor 1): USB-CDC console + UART0 TX task on GP0.
 * - Strict Barr C compliance: no dynamic memory allocation, static stacks.
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <tk/smp.h>

#include "hestia_uart.h"

#define HESTIA_STACK_SZ     4096

LOCAL T_CTSK ctsk_uart = {
    .exinf      = NULL,
    .tskatr     = TA_HLNG | TA_RNG3 | TA_ASSPRC,
    .task       = hestia_uart_tx_task,
    .itskpri    = 9,
    .stksz      = HESTIA_STACK_SZ,
    .bufptr     = NULL,
    .assprc     = TP_PRC1, /* Bound to Processor 1 (physical Core 0) */
};

LOCAL BOOL make_task(const char *name, ID *out_id, T_CTSK *pk_ctsk)
{
    ID id = tk_cre_tsk(pk_ctsk);
    if (id <= E_OK) {
        tm_printf((UB *)"[HESTIA:INIT] FATAL: tk_cre_tsk(%s) failed, err=%d\n", name, id);
        return FALSE;
    }
    ER err = tk_sta_tsk(id, 0);
    if (err != E_OK) {
        tm_printf((UB *)"[HESTIA:INIT] FATAL: tk_sta_tsk(%s) failed, err=%d\n", name, err);
        return FALSE;
    }
    *out_id = id;
    return TRUE;
}

EXPORT INT usermain(void)
{
    ID uart_tid = 0;

    tm_printf((UB *)"\n==================================================\n");
    tm_printf((UB *)"  HESTIA - Victim A Firmware (µT-Kernel 3.0 SMP) \n");
    tm_printf((UB *)"  Core 0: UART0 TX (GP0 @ 115200, 2 Hz)         \n");
    tm_printf((UB *)"==================================================\n");

    if (!make_task("UART_TX", &uart_tid, &ctsk_uart)) {
        return 1;
    }

    /* Initial task sleeps forever; scheduler runs the transmitter task */
    tk_slp_tsk(TMO_FEVR);
    return 0;
}
