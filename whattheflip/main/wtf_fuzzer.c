/*
 * wtf_fuzzer.c - Bounded hardware fault injector
 *
 * Implements FR-5: 10 µs UART bit flip, 50 µs I2C ACK-kill, 50 µs I2C clock-stretch, 5% duty budget.
 */

#include "wtf_fuzzer.h"
#include "wtf_safety.h"
#include "soc/gpio_struct.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

static void fuzzer_hw_init(void)
{
    /* Keep Probes 0, 2, and 3 in high-Z (input) mode during idle */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << WTF_PIN_PROBE0_GLITCH) |
                        (1ULL << WTF_PIN_PROBE2_SDA)    |
                        (1ULL << WTF_PIN_PROBE3_SCL),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

EXPORT void inject_uart_bit_flip_10us(void)
{
    if (wtf_safety_is_aborted()) {
        return;
    }

    /* Drive Probe 0 LOW for 10 µs into Row 12 (series 220 Ω limits current to 15 mA) */
    GPIO.out_w1tc.val = (1UL << WTF_PIN_PROBE0_GLITCH);
    GPIO.enable_w1ts.val = (1UL << WTF_PIN_PROBE0_GLITCH); /* Output enabled (drives LOW) */

    esp_rom_delay_us(10); /* Exactly 10 µs (1 bit duration at 115200 baud) */

    /* Float Probe 0 immediately */
    GPIO.enable_w1tc.val = (1UL << WTF_PIN_PROBE0_GLITCH);
}

EXPORT void inject_i2c_ack_kill_50us(void)
{
    if (wtf_safety_is_aborted()) {
        return;
    }

    /* Drive Probe 2 HIGH for 50 µs on Row 43 (SDA) to overwrite slave ACK */
    GPIO.out_w1ts.val = (1UL << WTF_PIN_PROBE2_SDA);
    GPIO.enable_w1ts.val = (1UL << WTF_PIN_PROBE2_SDA);

    esp_rom_delay_us(50); /* 50 µs bounded glitch */

    /* Float Probe 2 immediately */
    GPIO.enable_w1tc.val = (1UL << WTF_PIN_PROBE2_SDA);
}

EXPORT void inject_i2c_clock_stretch_50us(void)
{
    if (wtf_safety_is_aborted()) {
        return;
    }

    /* Drive Probe 3 (GPIO7 SCL) LOW for exactly 50 µs via direct register write */
    GPIO.out_w1tc.val = (1UL << WTF_PIN_PROBE3_SCL);
    GPIO.enable_w1ts.val = (1UL << WTF_PIN_PROBE3_SCL); /* Output enabled (drives LOW) */

    esp_rom_delay_us(50); /* Exactly 50 µs clock-stretch */

    /* Float Probe 3 immediately */
    GPIO.enable_w1tc.val = (1UL << WTF_PIN_PROBE3_SCL);
}

EXPORT void wtf_fuzzer_inject_uart(void)
{
    if (wtf_safety_is_aborted()) {
        return;
    }

    if (g_mtx_probe_access > 0) {
        (void)tk_loc_mtx(g_mtx_probe_access, TMO_FEVR);
    }

    wtf_safety_set_state(WTF_STATE_INJECTING);
    inject_uart_bit_flip_10us();
    wtf_safety_chirp_buzzer(20);
    wtf_safety_set_state(WTF_STATE_ARMED);

    tm_printf((UB *)"[FUZZER] MANUAL INJECT: 10 µs UART bit flip on Row 12 (Probe 0)\n");

    if (g_mtx_probe_access > 0) {
        (void)tk_unl_mtx(g_mtx_probe_access);
    }
}

EXPORT void wtf_fuzzer_inject_i2c_ack(void)
{
    if (wtf_safety_is_aborted()) {
        return;
    }

    if (g_mtx_probe_access > 0) {
        (void)tk_loc_mtx(g_mtx_probe_access, TMO_FEVR);
    }

    wtf_safety_set_state(WTF_STATE_INJECTING);
    inject_i2c_ack_kill_50us();
    wtf_safety_chirp_buzzer(20);
    wtf_safety_set_state(WTF_STATE_ARMED);

    tm_printf((UB *)"[FUZZER] MANUAL INJECT: 50 µs I2C ACK-kill on Row 43 (Probe 2)\n");

    if (g_mtx_probe_access > 0) {
        (void)tk_unl_mtx(g_mtx_probe_access);
    }
}

EXPORT void wtf_fuzzer_inject_i2c_stretch(void)
{
    if (wtf_safety_is_aborted()) {
        return;
    }

    if (g_mtx_probe_access > 0) {
        (void)tk_loc_mtx(g_mtx_probe_access, TMO_FEVR);
    }

    wtf_safety_set_state(WTF_STATE_INJECTING);
    inject_i2c_clock_stretch_50us();
    wtf_safety_chirp_buzzer(20);
    wtf_safety_set_state(WTF_STATE_ARMED);

    tm_printf((UB *)"[FUZZER] MANUAL INJECT: 50 µs I2C clock-stretch on Row 42 (Probe 3)\n");

    if (g_mtx_probe_access > 0) {
        (void)tk_unl_mtx(g_mtx_probe_access);
    }
}

EXPORT void wtf_fuzzer_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

    fuzzer_hw_init();
    tm_printf((UB *)"[FUZZER] TSK_FUZZER online: Bounded fault injector (10 µs UART, 50 µs ACK, 50 µs Stretch, FR-5)\n");

    /* Initial 3-second delay to allow victims to establish clean baseline */
    tk_dly_tsk(3000);

    uint32_t fuzz_cycle = 0;

    for (;;) {
        if (wtf_safety_is_aborted()) {
            tk_slp_tsk(TMO_FEVR);
        }

        fuzz_cycle++;

        /* Acquire probe mutex */
        if (g_mtx_probe_access > 0) {
            (void)tk_loc_mtx(g_mtx_probe_access, TMO_FEVR);
        }

        uint32_t mode = (fuzz_cycle % 3);

        if (mode == 1) {
            /* Cycle 1: UART 10 µs bit flip on Probe 0 */
            wtf_safety_set_state(WTF_STATE_INJECTING);
            inject_uart_bit_flip_10us();
            wtf_safety_chirp_buzzer(20);
            wtf_safety_set_state(WTF_STATE_ARMED);
            tm_printf((UB *)"[FUZZER] INJECT: 10 µs UART bit flip on Row 12 (Probe 0, duty=0.0003%%)\n");
        } else if (mode == 2) {
            /* Cycle 2: I2C 50 µs ACK-kill on Probe 2 */
            wtf_safety_set_state(WTF_STATE_INJECTING);
            inject_i2c_ack_kill_50us();
            wtf_safety_chirp_buzzer(20);
            wtf_safety_set_state(WTF_STATE_ARMED);
            tm_printf((UB *)"[FUZZER] INJECT: 50 µs I2C ACK-kill on Row 43 (Probe 2, duty=0.0017%%)\n");
        } else {
            /* Cycle 0: I2C 50 µs clock-stretch on Probe 3 */
            wtf_safety_set_state(WTF_STATE_INJECTING);
            inject_i2c_clock_stretch_50us();
            wtf_safety_chirp_buzzer(20);
            wtf_safety_set_state(WTF_STATE_ARMED);
            tm_printf((UB *)"[FUZZER] INJECT: 50 µs I2C clock-stretch on Row 42 (Probe 3, duty=0.0017%%)\n");
        }

        /* Notify telemetry queue of fault event */
        if (g_mbf_telemetry > 0) {
            wtf_record_t alert_rec;
            alert_rec.timestamp_us = (uint32_t)esp_timer_get_time();
            alert_rec.protocol = PROTO_FAULT_ALERT;
            alert_rec.probe_id = (uint8_t)((mode == 1) ? 0 : ((mode == 2) ? 2 : 3));
            alert_rec.length = 0;
            (void)tk_snd_mbf(g_mbf_telemetry, &alert_rec, sizeof(alert_rec), TMO_POL);
        }

        if (g_mtx_probe_access > 0) {
            (void)tk_unl_mtx(g_mtx_probe_access);
        }

        /* 3000 ms period between faults ensures duty cycle << 0.005% (< 5% limit) */
        tk_dly_tsk(3000);
    }
}
