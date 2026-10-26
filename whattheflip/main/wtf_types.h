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
