/*
 * wtf_sniffer.h - Passive packet sniffer for UART and I2C buses
 *
 * Implements FR-1: Passive sampling with 1 µs edge resolution, zero bus loading.
 */

#ifndef WTF_SNIFFER_H
#define WTF_SNIFFER_H

#include "wtf_types.h"

EXPORT void wtf_sniffer_task(INT stacd, void *exinf);

#endif /* WTF_SNIFFER_H */
