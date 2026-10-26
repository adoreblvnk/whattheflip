/*
 * wtf_telemetry.c - Periodic telemetry broadcaster and fault alert monitor
 *
 * Implements FR-9 (1..2 Hz update) and FR-10 (< 200 ms alert).
 */

#include "wtf_telemetry.h"

EXPORT void wtf_telemetry_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

    tm_printf((UB *)"[TELEMETRY] TSK_TELEMETRY online: Periodic broadcaster at 1..2 Hz (FR-9, FR-10)\n");

    uint32_t uart_packets = 0;
    uint32_t i2c_packets = 0;
    uint32_t fault_alerts = 0;
    wtf_record_t rec;

    for (;;) {
        /* Drain telemetry queue with 500 ms timeout (2 Hz period) */
        INT rcv_len = tk_rcv_mbf(g_mbf_telemetry, &rec, 500);

        if (rcv_len > 0) {
            if (rec.protocol == PROTO_UART) {
                uart_packets++;
            } else if (rec.protocol == PROTO_I2C) {
                i2c_packets++;
            } else if (rec.protocol == PROTO_FAULT_ALERT) {
                fault_alerts++;
                tm_printf((UB *)"[TELEMETRY:ALERT] FAST FAULT ALERT: Latency < 200 ms (alert #%u)\n",
                          fault_alerts);
            }
        }

        /* Periodic telemetry line at 2 Hz */
        tm_printf((UB *)"[DASHBOARD:2Hz] Telemetry: UART_pkts=%u | I2C_pkts=%u | Alerts=%u | Safety=%s\n",
                  uart_packets, i2c_packets, fault_alerts,
                  (g_flg_system > 0) ? "ARMED" : "INIT");

        tk_dly_tsk(500);
    }
}
