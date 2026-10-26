/*
 * wtf_dashboard.h - Wi-Fi SoftAP and WebSocket web dashboard task
 *
 * Implements FR-9: Real-time telemetry broadcast at 1..2 Hz (TC-5.1).
 * Implements FR-10: Immediate fault alert delivery in < 200 ms (TC-5.2).
 * Implements FR-8: Live PCAP capture download link over HTTP GET /capture.pcap (TC-4.2).
 */

#ifndef WTF_DASHBOARD_H
#define WTF_DASHBOARD_H

#include "wtf_types.h"

#ifdef __cplusplus
extern "C" {
#endif

EXPORT void wtf_dashboard_task(INT stacd, void *exinf);
EXPORT void wtf_dashboard_broadcast_alert(const char *msg);

#ifdef __cplusplus
}
#endif

#endif /* WTF_DASHBOARD_H */
