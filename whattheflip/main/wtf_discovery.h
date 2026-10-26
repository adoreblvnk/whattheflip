/*
 * wtf_discovery.h - Bus auto-discovery and protocol classification
 *
 * Implements FR-2: Classify UART / I2C within 500 ms, 95% accuracy (±2%).
 * Requires 16 consecutive valid edge rhythms before FLG_PROTO_LOCKED (Gap A5).
 */

#ifndef WTF_DISCOVERY_H
#define WTF_DISCOVERY_H

#include "wtf_types.h"

EXPORT void wtf_discovery_task(INT stacd, void *exinf);
EXPORT bool wtf_discovery_is_locked(uint8_t proto);

#endif /* WTF_DISCOVERY_H */
