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

#include <string.h>
#include "esp_ieee802154.h"

/* Fixed Iris Target Address (FR-6 Invariant: Never Broadcast) */
static const uint8_t s_iris_target_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x03};
static const uint16_t s_iris_target_short = 0x0003;

static volatile bool s_fuzzer_auto_mode = false; /* Demand-driven / scenario-triggered by default */

EXPORT void wtf_fuzzer_set_auto(bool enable)
{
    s_fuzzer_auto_mode = enable;
}

EXPORT bool wtf_fuzzer_get_auto(void)
{
    return s_fuzzer_auto_mode;
}

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

EXPORT void inject_wireless_target_jam_50us(void)
{
    if (wtf_safety_is_aborted()) {
        return;
    }

    /* FR-6 Invariant: Verify destination strictly matches Iris allow-list (Never broadcast) */
    if (s_iris_target_short != 0x0003 || s_iris_target_mac[0] != 0x02) {
        return;
    }

    uint8_t jam_frame[32];
    uint8_t psdu_len = 22;
    jam_frame[0] = psdu_len;
    jam_frame[1] = 0x41; /* FCF LSB: Data frame, PAN ID comp */
    jam_frame[2] = 0x88; /* FCF MSB: Short dest, short src */
    jam_frame[3] = 0xFA;
    jam_frame[4] = 0x34; /* PAN ID 0x1234 */
    jam_frame[5] = 0x12;
    jam_frame[6] = (uint8_t)(s_iris_target_short & 0xFF);
    jam_frame[7] = (uint8_t)((s_iris_target_short >> 8) & 0xFF);
    jam_frame[8] = 0x01;
    jam_frame[9] = 0x00;
    memcpy(&jam_frame[10], "WTF:JAM:TARGET:IRIS", 19);

    /* Transmit targeted burst on Channel 15 */
    (void)esp_ieee802154_transmit(jam_frame, false);
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
    /* Synchronize to Start Bit on Probe 1 (GPIO5) */
    uint32_t t_wait = 0;
    while (((GPIO.in.val >> WTF_PIN_PROBE1_SNIFF) & 1) != 0 && t_wait < 550000) {
        esp_rom_delay_us(10);
        t_wait += 10;
    }
    esp_rom_delay_us(25); /* Advance into payload byte */
    inject_uart_bit_flip_10us();
    wtf_safety_chirp_buzzer(20);
    tk_dly_tsk(150); /* Visible LED hold */
    wtf_safety_set_state(WTF_STATE_ARMED);
    tm_printf((UB *)"[FUZZER] MANUAL INJECT: 10 µs UART bit flip on Row 12 (Probe 0)\n");

    if (g_mbf_telemetry > 0) {
        wtf_record_t alert_rec;
        alert_rec.timestamp_us = (uint32_t)esp_timer_get_time();
        alert_rec.protocol = PROTO_FAULT_ALERT;
        alert_rec.probe_id = 0;
        alert_rec.length = 0;
        (void)tk_snd_mbf(g_mbf_telemetry, &alert_rec, sizeof(alert_rec), TMO_POL);
    }
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
    /* Synchronize to active I2C SCL clock on Probe 3 (GPIO7) */
    uint32_t t_wait = 0;
    while (((GPIO.in.val >> WTF_PIN_PROBE3_SCL) & 1) != 0 && t_wait < 250000) {
        esp_rom_delay_us(10);
        t_wait += 10;
    }
    inject_i2c_ack_kill_50us();
    wtf_safety_chirp_buzzer(20);
    tk_dly_tsk(150); /* Visible LED hold */
    wtf_safety_set_state(WTF_STATE_ARMED);
    tm_printf((UB *)"[FUZZER] MANUAL INJECT: 50 µs I2C ACK-kill on Row 43 (Probe 2)\n");

    if (g_mbf_telemetry > 0) {
        wtf_record_t alert_rec;
        alert_rec.timestamp_us = (uint32_t)esp_timer_get_time();
        alert_rec.protocol = PROTO_FAULT_ALERT;
        alert_rec.probe_id = 2;
        alert_rec.length = 0;
        (void)tk_snd_mbf(g_mbf_telemetry, &alert_rec, sizeof(alert_rec), TMO_POL);
    }
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
    uint32_t t_wait = 0;
    while (((GPIO.in.val >> WTF_PIN_PROBE3_SCL) & 1) != 0 && t_wait < 250000) {
        esp_rom_delay_us(10);
        t_wait += 10;
    }
    inject_i2c_clock_stretch_50us();
    wtf_safety_chirp_buzzer(20);
    tk_dly_tsk(150); /* Visible LED hold */
    wtf_safety_set_state(WTF_STATE_ARMED);
    tm_printf((UB *)"[FUZZER] MANUAL INJECT: 50 µs I2C clock-stretch on Row 42 (Probe 3)\n");

    if (g_mbf_telemetry > 0) {
        wtf_record_t alert_rec;
        alert_rec.timestamp_us = (uint32_t)esp_timer_get_time();
        alert_rec.protocol = PROTO_FAULT_ALERT;
        alert_rec.probe_id = 3;
        alert_rec.length = 0;
        (void)tk_snd_mbf(g_mbf_telemetry, &alert_rec, sizeof(alert_rec), TMO_POL);
    }
    if (g_mtx_probe_access > 0) {
        (void)tk_unl_mtx(g_mtx_probe_access);
    }
}

EXPORT void wtf_fuzzer_inject_wireless_jam(void)
{
    if (wtf_safety_is_aborted()) {
        return;
    }

    if (g_mtx_probe_access > 0) {
        (void)tk_loc_mtx(g_mtx_probe_access, TMO_FEVR);
    }

    wtf_safety_set_state(WTF_STATE_INJECTING);
    inject_wireless_target_jam_50us();
    wtf_safety_chirp_buzzer(20);
    tk_dly_tsk(150); /* Visible LED hold */
    wtf_safety_set_state(WTF_STATE_ARMED);
    tm_printf((UB *)"[FUZZER:WIRELESS] MANUAL INJECT: Targeted 802.15.4 burst to Iris BSSID 02:00:00:00:00:03 on Ch 15 (FR-6, TC-3.3)\n");

    if (g_mbf_telemetry > 0) {
        wtf_record_t alert_rec;
        alert_rec.timestamp_us = (uint32_t)esp_timer_get_time();
        alert_rec.protocol = PROTO_FAULT_ALERT;
        alert_rec.probe_id = 15;
        alert_rec.length = 0;
        (void)tk_snd_mbf(g_mbf_telemetry, &alert_rec, sizeof(alert_rec), TMO_POL);
    }
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
        if (wtf_safety_is_aborted() || !s_fuzzer_auto_mode) {
            tk_dly_tsk(100);
            continue;
        }
        fuzz_cycle++;

        /* Acquire probe mutex */
        if (g_mtx_probe_access > 0) {
            (void)tk_loc_mtx(g_mtx_probe_access, TMO_FEVR);
        }

        uint32_t mode = (fuzz_cycle % 4);

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
        } else if (mode == 3) {
            /* Cycle 3: I2C 50 µs clock-stretch on Probe 3 */
            wtf_safety_set_state(WTF_STATE_INJECTING);
            inject_i2c_clock_stretch_50us();
            wtf_safety_chirp_buzzer(20);
            wtf_safety_set_state(WTF_STATE_ARMED);
            tm_printf((UB *)"[FUZZER] INJECT: 50 µs I2C clock-stretch on Row 42 (Probe 3, duty=0.0017%%)\n");
        } else {
            /* Cycle 0: Targeted 802.15.4 stress burst directed strictly to Iris BSSID (FR-6, TC-3.3) */
            wtf_safety_set_state(WTF_STATE_INJECTING);
            inject_wireless_target_jam_50us();
            wtf_safety_chirp_buzzer(20);
            wtf_safety_set_state(WTF_STATE_ARMED);
            tm_printf((UB *)"[FUZZER:WIRELESS] INJECT: Targeted 802.15.4 burst to Iris BSSID 02:00:00:00:00:03 on Ch 15 (FR-6, TC-3.3)\n");
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
