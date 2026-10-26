/*
 * wtf_types.h - Common definitions, pinouts, and kernel objects for whattheflip
 *
 * ESP32-C6 µT-Kernel 3.0 Diagnostic and Fault Injection Tool
 */

#ifndef WTF_TYPES_H
#define WTF_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "riscv/encoding.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>

/* Physical Pin Mapping on Waveshare ESP32-C6-Pico */
#define WTF_PIN_PROBE0_GLITCH   4   /* Probe 0: UART Glitch Inject (Row 12, 220 Ω) */
#define WTF_PIN_PROBE1_SNIFF    5   /* Probe 1: Clean UART Sniff (Row 10, Hestia TX) */
#define WTF_PIN_PROBE2_SDA      6   /* Probe 2: I2C SDA Sniff / ACK-kill Inject (Row 43) */
#define WTF_PIN_PROBE3_SCL      7   /* Probe 3: I2C SCL Sniff / Stretch Inject (Row 42) */

#define WTF_PIN_ESTOP           9   /* Boot button: Hardware E-Stop (Active LOW) */
#define WTF_PIN_NEOPIXEL        8   /* Onboard WS2812 RGB LED */

/* TCA9554PWR I2C Expander Pins (for Buzzer EXIO3 on Maker Pi Base) */
#define WTF_PIN_TCA9554_SDA     22
#define WTF_PIN_TCA9554_SCL     23
#define WTF_EXIO_BUZZER         3
#define WTF_EXIO_SD_CS          7

/* Safety Status States */
typedef enum {
    WTF_STATE_ARMED       = 0,  /* Green: Armed / Sniffing */
    WTF_STATE_INJECTING   = 1,  /* Yellow: Fault Injecting */
    WTF_STATE_ABORTED     = 2   /* Red: E-Stop Aborted */
} wtf_safety_state_t;

/* Decoder Remap Identifiers (FR-3, TC-1.3) */
#define WTF_DECODER_NONE            0
#define WTF_DECODER_UART1_RX        1   /* Remap to UART1 RX input signal */
#define WTF_DECODER_TIMER_CAPTURE   2   /* Remap to Timer / MCPWM capture signal */
#define WTF_DECODER_RMT_RX0         3   /* Remap to RMT RX channel 0 */
#define WTF_DECODER_RMT_RX1         4   /* Remap to RMT RX channel 1 */

/* Fault Mode Identifiers (FR-5, TC-3.1, TC-3.2) */
#define WTF_FAULT_NONE              0
#define WTF_FAULT_UART_BIT_FLIP     1
#define WTF_FAULT_I2C_ACK_KILL      2
#define WTF_FAULT_I2C_CLOCK_STRETCH 3
/* MicroSD Bitbang SPI Pins (Maker Pi Base GP15 jumpered to GP13 -> GPIO21) */
#define WTF_PIN_SD_SCK          18
#define WTF_PIN_SD_MOSI         19
#define WTF_PIN_SD_MISO         20
#define WTF_PIN_SD_CS           21

/* Protocol Identifiers */
#define PROTO_NONE              0
#define PROTO_UART              1
#define PROTO_I2C               2
#define PROTO_FAULT_ALERT       3

/* Event Flag Bits */
#define FLG_BIT_SAFETY_ABORT    (1 << 0)
#define FLG_BIT_UART_LOCKED     (1 << 1)
#define FLG_BIT_I2C_LOCKED      (1 << 2)

/* Packet Record Structure */
#define WTF_PKT_DATA_MAX        64

typedef struct {
    uint32_t timestamp_us;
    uint8_t protocol;
    uint8_t probe_id;
    uint16_t length;
    uint8_t data[WTF_PKT_DATA_MAX];
} wtf_record_t;

/* Global Kernel Object Handles */
extern ID g_mpf_packets;
extern ID g_mbf_storage;
extern ID g_mbf_telemetry;
extern ID g_mtx_probe_access;
extern ID g_flg_system;

#endif /* WTF_TYPES_H */
