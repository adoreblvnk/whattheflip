/*
 * wtf_fuzzer.c - Bounded hardware fault injector
 *
 * Implements FR-5: 10 µs UART bit flip, 50 µs I2C ACK kill, 5% duty budget.
 */

#include "wtf_fuzzer.h"
#include "wtf_safety.h"
#include "soc/gpio_struct.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"

static void fuzzer_hw_init(void)
{
    /* Keep Probes 0 and 2 in high-Z (input) mode during idle */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << WTF_PIN_PROBE0_GLITCH) | (1ULL << WTF_PIN_PROBE2_SDA),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

static void inject_uart_bit_flip_10us(void)
{
    if (wtf_safety_is_aborted()) return;

    /* Drive Probe 0 LOW for 10 µs into Row 12 (series 220 Ω limits current to 15 mA) */
    GPIO.out_w1tc.val = (1UL << WTF_PIN_PROBE0_GLITCH);
    GPIO.enable_w1ts.val = (1UL << WTF_PIN_PROBE0_GLITCH); /* Output enabled */

    esp_rom_delay_us(10); /* Exactly 10 µs (1 bit duration at 115200 baud) */

    /* Float Probe 0 immediately */
    GPIO.enable_w1tc.val = (1UL << WTF_PIN_PROBE0_GLITCH);
}

static void inject_i2c_ack_kill_50us(void)
{
    if (wtf_safety_is_aborted()) return;

    /* Drive Probe 2 HIGH for 50 µs on Row 43 (SDA) to overwrite slave ACK */
    GPIO.out_w1ts.val = (1UL << WTF_PIN_PROBE2_SDA);
    GPIO.enable_w1ts.val = (1UL << WTF_PIN_PROBE2_SDA);

    esp_rom_delay_us(50); /* 50 µs bounded glitch */

    /* Float Probe 2 immediately */
    GPIO.enable_w1tc.val = (1UL << WTF_PIN_PROBE2_SDA);
}

EXPORT void wtf_fuzzer_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

    fuzzer_hw_init();
    tm_printf((UB *)"[FUZZER] TSK_FUZZER online: Bounded fault injector (10 µs UART, 50 µs I2C, FR-5)\n");

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

        if ((fuzz_cycle % 2) == 1) {
            /* Cycle A: UART Glitch */
            inject_uart_bit_flip_10us();
            tm_printf((UB *)"[FUZZER] INJECT: 10 µs UART bit flip on Row 12 (Probe 0, duty=0.0005%%)\n");
        } else {
            /* Cycle B: I2C ACK-Kill Glitch */
            inject_i2c_ack_kill_50us();
            tm_printf((UB *)"[FUZZER] INJECT: 50 µs I2C ACK-kill on Row 43 (Probe 2, duty=0.0025%%)\n");
        }

        if (g_mtx_probe_access > 0) {
            (void)tk_unl_mtx(g_mtx_probe_access);
        }

        /* 3000 ms period between faults ensures duty cycle << 5% */
        tk_dly_tsk(3000);
    }
}
