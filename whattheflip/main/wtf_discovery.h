/*
 * wtf_discovery.h - Bus auto-discovery and dynamic probe remapping
 *
 * Implements FR-2: Classify UART / I2C within 500 ms, 95% accuracy (±2%).
 * Implements FR-3: Dynamic probe remapping engine (< 10 ms, TC-1.3).
 * Requires 16 consecutive valid edge rhythms before FLG_PROTO_LOCKED (Gap A5).
 */

#ifndef WTF_DISCOVERY_H
#define WTF_DISCOVERY_H

#include "wtf_types.h"

EXPORT void wtf_discovery_task(INT stacd, void *exinf);
EXPORT bool wtf_discovery_is_locked(uint8_t proto);

/* Dynamic probe remapping engine (FR-3, TC-1.3) */
EXPORT ER wtf_discovery_remap_probe(uint8_t probe_pin, uint8_t decoder_id);
EXPORT uint8_t wtf_discovery_get_remapped_decoder(uint8_t probe_pin);

#endif /* WTF_DISCOVERY_H */
