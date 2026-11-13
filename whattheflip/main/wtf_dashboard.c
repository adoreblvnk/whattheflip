/*
 * wtf_dashboard.c - SoftAP, HTTP server, and WebSocket dashboard for whattheflip
 *
 * Implements FR-8: Live PCAP capture download link over HTTP GET /capture.pcap (TC-4.2).
 * Implements FR-9: 1..2 Hz real-time telemetry updates (TC-5.1).
 * Implements FR-10: Immediate fault alert delivery in < 200 ms (TC-5.2).
 * Hosts SoftAP on Channel 6, IP 192.168.4.1.
 */

#include "wtf_dashboard.h"
#include "wtf_safety.h"
#include "wtf_discovery.h"
#include "wtf_storage.h"
#include "wtf_sniffer.h"
#include "wtf_fuzzer.h"
#include "dashboard_html.h"
#include "../../shared/packet_types.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

#define MAX_WS_CLIENTS          4
#define DASHBOARD_WIFI_CHANNEL  6
#define DASHBOARD_SSID          "whattheflip"


static httpd_handle_t s_http_server = NULL;
static int s_ws_fds[MAX_WS_CLIENTS];
static uint32_t s_alert_counter = 0U;

/* Stream static asset directly from MicroSD /WWW/ directory in 512-byte sectors */
static esp_err_t serve_sd_www_asset(httpd_req_t *req, const uint8_t name83[11],
                                    const char *content_type, bool is_gzipped)
{
    wtf_sniffer_notify_http_activity();
    size_t sd_sz = wtf_storage_get_www_file_size(name83);
    if (sd_sz == 0U) {
        const uint8_t *fb_data = NULL;
        size_t fb_len = 0U;
        if (memcmp(name83, "INDEX   HTM", 11) == 0) {
            fb_data = g_www_index_htm;
            fb_len  = g_www_index_htm_len;
        } else if (memcmp(name83, "APP     JS ", 11) == 0) {
            fb_data = g_www_app_js_gz;
            fb_len  = g_www_app_js_gz_len;
        } else if (memcmp(name83, "STYLE   CSS", 11) == 0) {
            fb_data = g_www_style_css_gz;
            fb_len  = g_www_style_css_gz_len;
        }
        if (fb_data != NULL && fb_len > 0U) {
            httpd_resp_set_type(req, content_type);
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
            if (is_gzipped) {
                httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
            }
            return httpd_resp_send(req, (const char *)fb_data, fb_len);
        }
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (is_gzipped) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }

    static uint8_t s_www_sector[512];
    size_t offset = 0U;
    while (offset < sd_sz) {
        size_t to_read = sd_sz - offset;
        if (to_read > sizeof(s_www_sector)) {
            to_read = sizeof(s_www_sector);
        }
        size_t got = wtf_storage_read_www_file(name83, offset, s_www_sector, to_read);
        if (got == 0U) {
            break;
        }
        esp_err_t err = httpd_resp_send_chunk(req, (const char *)s_www_sector, got);
        if (err != ESP_OK) {
            return err;
        }
        offset += got;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const uint8_t name_htm[11] = {'I','N','D','E','X',' ',' ',' ','H','T','M'};
    return serve_sd_www_asset(req, name_htm, "text/html", false);
}

static esp_err_t app_js_get_handler(httpd_req_t *req)
{
    const uint8_t name_js[11] = {'A','P','P',' ',' ',' ',' ',' ','J','S',' '};
    return serve_sd_www_asset(req, name_js, "application/javascript", true);
}

static esp_err_t style_css_get_handler(httpd_req_t *req)
{
    const uint8_t name_css[11] = {'S','T','Y','L','E',' ',' ',' ','C','S','S'};
    return serve_sd_www_asset(req, name_css, "text/css", true);
}

static esp_err_t cmd_post_handler(httpd_req_t *req)
{
    wtf_sniffer_notify_http_activity();
    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(query, "c", val, sizeof(val)) == ESP_OK) {
            char c = val[0];
            if (c == 'E' || c == 'e') {
                wtf_safety_abort();
                wtf_dashboard_broadcast_alert("E-Stop activated: whattheflip probes High-Z, LEDs off");
            } else if (c == 'R' || c == 'r') {
                wtf_safety_rearm();
                wtf_dashboard_broadcast_alert("whattheflip resumed (Active)");
            } else if (c == 'C' || c == 'c') {
                wtf_fuzzer_cancel();
                wtf_sniffer_clear_stats();
                wtf_dashboard_clear_stats();
                wtf_dashboard_broadcast_alert("Stats cleared and fault injection stopped");
            } else if (c == 'U' || c == 'u') {
                wtf_fuzzer_inject_uart();
            } else if (c == 'A' || c == 'a') {
                wtf_fuzzer_inject_i2c_ack();
            } else if (c == 'S' || c == 's') {
                wtf_fuzzer_inject_i2c_stretch();
            } else if (c == 'W' || c == 'w') {
                wtf_fuzzer_inject_wireless_jam();
            } else if (c == 'J' || c == 'j') {
                wtf_fuzzer_inject_wifi_jam(wtf_sniffer_get_wifi_channel());
            }
        }
        if (httpd_query_key_value(query, "baud", val, sizeof(val)) == ESP_OK) {
            uint32_t b = (uint32_t)strtoul(val, NULL, 10);
            wtf_sniffer_set_uart_baud(b);
        }
        if (httpd_query_key_value(query, "uart_auto", val, sizeof(val)) == ESP_OK) {
            wtf_sniffer_request_uart_autodetect();
        }
        if (httpd_query_key_value(query, "iris_cmd", val, sizeof(val)) == ESP_OK) {
            if (strcmp(val, "20hz") == 0) {
                wtf_fuzzer_send_iris_cmd("WTF:CMD:RATE:20HZ");
            } else if (strcmp(val, "2hz") == 0) {
                wtf_fuzzer_send_iris_cmd("WTF:CMD:RATE:2HZ");
            }
        }
        if (httpd_query_key_value(query, "ch", val, sizeof(val)) == ESP_OK) {
            int ch = atoi(val);
            if (ch >= 1 && ch <= 13) {
                wtf_sniffer_request_wifi_burst((uint8_t)ch);
            }
        }
        if (httpd_query_key_value(query, "wifi_jam", val, sizeof(val)) == ESP_OK) {
            int ch = atoi(val);
            if (ch >= 1 && ch <= 13) {
                wtf_fuzzer_inject_wifi_jam((uint8_t)ch);
            }
        }
        if (httpd_query_key_value(query, "pin", val, sizeof(val)) == ESP_OK) {
            char dec_val[16];
            if (httpd_query_key_value(query, "dec", dec_val, sizeof(dec_val)) == ESP_OK) {
                uint8_t pin = (uint8_t)atoi(val);
                uint8_t dec = (uint8_t)atoi(dec_val);
                (void)wtf_discovery_remap_probe(pin, dec);
            }
        }
        if (httpd_query_key_value(query, "mode", val, sizeof(val)) == ESP_OK) {
            if (strcasecmp(val, "off") == 0) {
                wtf_discovery_set_tool_mode(WTF_TOOL_MODE_OFF);
            } else if (strcasecmp(val, "auto") == 0) {
                wtf_discovery_set_tool_mode(WTF_TOOL_MODE_AUTO);
            } else if (strcasecmp(val, "uart") == 0) {
                wtf_discovery_set_tool_mode(WTF_TOOL_MODE_UART);
            } else if (strcasecmp(val, "i2c") == 0) {
                wtf_discovery_set_tool_mode(WTF_TOOL_MODE_I2C);
            } else if (strcasecmp(val, "wifi") == 0) {
                wtf_discovery_set_tool_mode(WTF_TOOL_MODE_WIFI);
            } else if (strcasecmp(val, "zigbee") == 0) {
                wtf_discovery_set_tool_mode(WTF_TOOL_MODE_ZIGBEE);
            }
        }
        if (httpd_query_key_value(query, "zch", val, sizeof(val)) == ESP_OK) {
            int zch = atoi(val);
            if (zch >= 11 && zch <= 26) {
                wtf_sniffer_set_154_channel((uint8_t)zch);
            }
        }
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "OK", 2);
}

/* HTTP GET /api/pcap?offset=<bytes>&limit=<bytes>
 * Batched & chunked binary PCAP streaming endpoint so client JS can load and parse
 * large SD card captures incrementally without buffering the whole file in RAM.
 */
static esp_err_t api_pcap_chunk_get_handler(httpd_req_t *req)
{
    wtf_sniffer_notify_http_activity();
    size_t total_sz = wtf_storage_get_pcap_size();

    size_t req_offset = 0U;
    size_t req_limit  = 4096U; /* Default 4 KB batch (8 SD sectors) */

    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(query, "offset", val, sizeof(val)) == ESP_OK) {
            req_offset = (size_t)strtoul(val, NULL, 10);
        }
        if (httpd_query_key_value(query, "limit", val, sizeof(val)) == ESP_OK) {
            req_limit = (size_t)strtoul(val, NULL, 10);
            if (req_limit == 0U || req_limit > 16384U) {
                req_limit = 4096U;
            }
        }
    }

    char hdr_total[24];
    char hdr_off[24];
    snprintf(hdr_total, sizeof(hdr_total), "%u", (unsigned)total_sz);
    snprintf(hdr_off, sizeof(hdr_off), "%u", (unsigned)req_offset);

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Expose-Headers", "X-Pcap-Total-Bytes, X-Pcap-Offset");
    httpd_resp_set_hdr(req, "X-Pcap-Total-Bytes", hdr_total);
    httpd_resp_set_hdr(req, "X-Pcap-Offset", hdr_off);

    if (req_offset >= total_sz) {
        return httpd_resp_send(req, NULL, 0);
    }

    size_t end_offset = req_offset + req_limit;
    if (end_offset > total_sz) {
        end_offset = total_sz;
    }

    static uint8_t s_api_chunk[512];
    size_t cur = req_offset;
    while (cur < end_offset) {
        size_t want = end_offset - cur;
        if (want > sizeof(s_api_chunk)) {
            want = sizeof(s_api_chunk);
        }
        size_t got = wtf_storage_read_pcap(cur, s_api_chunk, want);
        if (got == 0U) {
            break;
        }
        esp_err_t err = httpd_resp_send_chunk(req, (const char *)s_api_chunk, got);
        if (err != ESP_OK) {
            return err;
        }
        cur += got;
    }

    return httpd_resp_send_chunk(req, NULL, 0);
}

/* HTTP GET /capture.pcap handler: streams captured PCAP to browser (FR-8, TC-4.2) */
static esp_err_t capture_pcap_get_handler(httpd_req_t *req)
{
    wtf_sniffer_notify_http_activity();
    size_t total_sz = wtf_storage_get_pcap_size();
    if (total_sz == 0U) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/vnd.tcpdump.pcap");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"capture.pcap\"");

    size_t offset = 0U;
    size_t end_offset = total_sz;
    char range_hdr[64];
    if (httpd_req_get_hdr_value_str(req, "Range", range_hdr, sizeof(range_hdr)) == ESP_OK) {
        unsigned r_start = 0U;
        unsigned r_end = (unsigned)(total_sz - 1U);
        if (sscanf(range_hdr, "bytes=%u-%u", &r_start, &r_end) >= 1) {
            if ((size_t)r_end >= total_sz) {
                r_end = (unsigned)(total_sz - 1U);
            }
            if ((size_t)r_start <= (size_t)r_end) {
                offset = (size_t)r_start;
                end_offset = (size_t)r_end + 1U;
                char cr_hdr[64];
                snprintf(cr_hdr, sizeof(cr_hdr), "bytes %u-%u/%u", r_start, r_end, (unsigned)total_sz);
                httpd_resp_set_status(req, "206 Partial Content");
                httpd_resp_set_hdr(req, "Content-Range", cr_hdr);
            }
        }
    }

    static uint8_t s_chunk_buf[512];
    while (offset < end_offset) {
        size_t to_read = end_offset - offset;
        if (to_read > sizeof(s_chunk_buf)) {
            to_read = sizeof(s_chunk_buf);
        }

        size_t bytes_read = wtf_storage_read_pcap(offset, s_chunk_buf, to_read);
        if (bytes_read == 0U) {
            break;
        }

        esp_err_t err = httpd_resp_send_chunk(req, (const char *)s_chunk_buf, bytes_read);
        if (err != ESP_OK) {
            return err;
        }
        offset += bytes_read;
    }

    /* Terminate chunked transfer */
    return httpd_resp_send_chunk(req, NULL, 0);
}
EXPORT void wtf_dashboard_broadcast_alert(const char *msg)
{
    if (!s_http_server || msg == NULL) {
        return;
    }

    s_alert_counter++;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    char alert_json[160];
    snprintf(alert_json, sizeof(alert_json),
             "{\"type\":\"alert\",\"msg\":\"%s\",\"alert_id\":%u,\"timestamp_ms\":%u}",
             msg, (unsigned)s_alert_counter, (unsigned)now_ms);

    tm_printf((UB *)"[TELEMETRY:ALERT] FAST FAULT ALERT: Latency < 200 ms (alert #%u)\n",
              (unsigned)s_alert_counter);
    tm_printf((UB *)"[DASHBOARD:ALERT] Broadcast alert frame in < 200 ms: %s\n", msg);

    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)alert_json,
        .len = strlen(alert_json),
    };

    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] != -1) {
            esp_err_t ret = httpd_ws_send_frame_async(s_http_server, s_ws_fds[i], &frame);
            if (ret != ESP_OK) {
                s_ws_fds[i] = -1;
            }
        }
    }
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* WebSocket Handshake */
        int fd = httpd_req_to_sockfd(req);
        for (int i = 0; i < MAX_WS_CLIENTS; i++) {
            if (s_ws_fds[i] == -1 || s_ws_fds[i] == fd) {
                s_ws_fds[i] = fd;
                break;
            }
        }
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    uint8_t buf[32];
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = buf;
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, sizeof(buf) - 1);
    if (ret == ESP_OK && ws_pkt.len > 0) {
        buf[ws_pkt.len] = '\0';
        const char *cmd = (const char *)buf;

        if (strcmp(cmd, "ESTOP") == 0) {
            wtf_safety_abort();
            wtf_dashboard_broadcast_alert("E-Stop activated: probes High-Z, LEDs off");
        } else if (strcmp(cmd, "CLEAR") == 0) {
            wtf_fuzzer_cancel();
            wtf_sniffer_clear_stats();
            wtf_dashboard_clear_stats();
        } else if (strcmp(cmd, "FUZZ_UART") == 0) {
            wtf_fuzzer_inject_uart();
        } else if (strcmp(cmd, "FUZZ_I2C_ACK") == 0) {
            wtf_fuzzer_inject_i2c_ack();
        } else if (strcmp(cmd, "FUZZ_I2C_STRETCH") == 0) {
            wtf_fuzzer_inject_i2c_stretch();
        }
    }
    return ESP_OK;
}

static uint32_t s_uart_cnt    = 0U;
static uint32_t s_i2c_cnt     = 0U;
static uint32_t s_wifi_cnt    = 0U;
static uint32_t s_ieee154_cnt = 0U;
static uint32_t s_fuzz_cnt    = 0U;

EXPORT void wtf_dashboard_clear_stats(void)
{
    s_uart_cnt    = 0U;
    s_i2c_cnt     = 0U;
    s_wifi_cnt    = 0U;
    s_ieee154_cnt = 0U;
    s_fuzz_cnt    = 0U;
    s_alert_counter = 0U;
}

static void json_escape_str(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0U) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t o = 0U;
    for (size_t i = 0U; src[i] != '\0' && o + 2U < dst_sz; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            if (o + 3U >= dst_sz) break;
            dst[o++] = '\\';
            dst[o++] = c;
        } else if ((unsigned char)c >= 32U && (unsigned char)c <= 126U) {
            dst[o++] = c;
        } else {
            dst[o++] = '.';
        }
    }
    dst[o] = '\0';
}

static int format_telemetry_json(char *json_buf, size_t max_len)
{
    bool u_lock = wtf_discovery_is_locked(PROTO_UART);
    bool i_lock = wtf_discovery_is_locked(PROTO_I2C);
    const char *safety_str = wtf_safety_is_aborted() ? "Stopped" : "Active";
    int uptime_s = (int)(esp_timer_get_time() / 1000000ULL);
    size_t pcap_bytes = wtf_storage_get_pcap_size();
    uint8_t wifi_ch   = wtf_sniffer_get_wifi_channel();
    uint8_t wifi_mode = wtf_sniffer_get_wifi_mode();
    uint8_t zigbee_ch = wtf_sniffer_get_154_channel();
    uint32_t uart_baud = wtf_sniffer_get_uart_baud();
    const char *tool_mode = wtf_discovery_get_tool_mode_str();
    int16_t ax = 0, ay = 0, az = 0, mx = 0, my = 0, mz = 0;
    wtf_sniffer_get_i2c_imu(&ax, &ay, &az, &mx, &my, &mz);

    int8_t iris_rssi = wtf_sniffer_get_iris_rssi();
    bool iris_verified = wtf_sniffer_is_iris_verified();

    char u_samp[72], w_samp[72], z_samp[72];
    char u_esc[140], w_esc[140], z_esc[140];
    char fu[64], fi[64], fw[64], fz[64], fb[64];
    bool u_clr = false, w_clr = false, z_clr = false;
    uint8_t w_sch = 6U, imask = 0U, emask = 0U;
    wtf_sniffer_get_ctf_snapshot(
        u_samp, sizeof(u_samp), &u_clr,
        w_samp, sizeof(w_samp), &w_clr, &w_sch,
        z_samp, sizeof(z_samp), &z_clr,
        fu, fi, fw, fz, fb, sizeof(fu),
        &imask, &emask);

    json_escape_str(u_esc, sizeof(u_esc), u_samp);
    json_escape_str(w_esc, sizeof(w_esc), w_samp);
    json_escape_str(z_esc, sizeof(z_esc), z_samp);

    return snprintf(json_buf, max_len,
                    "{\"type\":\"telemetry\","
                    "\"uart_pkts\":%u,\"i2c_pkts\":%u,\"wifi_pkts\":%u,\"ieee154_pkts\":%u,"
                    "\"fuzz_cycles\":%u,\"corrupted_frames\":%u,"
                    "\"wifi_ch\":%u,\"wifi_mode\":%u,\"zigbee_ch\":%u,\"uart_baud\":%u,"
                    "\"tool_mode\":\"%s\",\"safety\":\"%s\","
                    "\"uart_locked\":%s,\"i2c_locked\":%s,\"pcap_bytes\":%u,"
                    "\"iris_rssi\":%d,\"iris_verified\":%s,"
                    "\"accel\":{\"x\":%d,\"y\":%d,\"z\":%d},"
                    "\"mag\":{\"x\":%d,\"y\":%d,\"z\":%d},"
                    "\"uart_clear\":%s,\"uart_sample\":\"%s\","
                    "\"wifi_clear\":%s,\"wifi_sample_ch\":%u,\"wifi_sample\":\"%s\","
                    "\"z154_clear\":%s,\"z154_sample\":\"%s\","
                    "\"ctf\":{\"uart\":\"%s\",\"i2c\":\"%s\",\"wifi\":\"%s\",\"zigbee\":\"%s\",\"bonus\":\"%s\",\"imask\":%u,\"emask\":%u},"
                    "\"uptime_s\":%d}",
                    (unsigned)s_uart_cnt, (unsigned)s_i2c_cnt, (unsigned)s_wifi_cnt, (unsigned)s_ieee154_cnt,
                    (unsigned)s_fuzz_cnt, (unsigned)s_fuzz_cnt,
                    (unsigned)wifi_ch, (unsigned)wifi_mode, (unsigned)zigbee_ch, (unsigned)uart_baud,
                    tool_mode, safety_str,
                    u_lock ? "true" : "false", i_lock ? "true" : "false", (unsigned)pcap_bytes,
                    (int)iris_rssi, iris_verified ? "true" : "false",
                    (int)ax, (int)ay, (int)az, (int)mx, (int)my, (int)mz,
                    u_clr ? "true" : "false", u_esc,
                    w_clr ? "true" : "false", (unsigned)w_sch, w_esc,
                    z_clr ? "true" : "false", z_esc,
                    fu, fi, fw, fz, fb, (unsigned)imask, (unsigned)emask,
                    uptime_s);
}

static esp_err_t telemetry_get_handler(httpd_req_t *req)
{
    wtf_sniffer_notify_http_activity();
    char json_buf[1536];
    int len = format_telemetry_json(json_buf, sizeof(json_buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json_buf, (len > 0) ? (size_t)len : 0U);
}

static void broadcast_telemetry_frame(void)
{
    char json_buf[1536];
    int len = format_telemetry_json(json_buf, sizeof(json_buf));
    if (len <= 0) return;

    /* Emit full JSON snapshot for USB-serial bridge */
    esp_rom_printf("[DASHBOARD:JSON] %s\n", json_buf);

    if (!s_http_server) return;

    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json_buf,
        .len = (size_t)len,
    };

    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] != -1) {
            esp_err_t ret = httpd_ws_send_frame_async(s_http_server, s_ws_fds[i], &frame);
            if (ret != ESP_OK) {
                s_ws_fds[i] = -1;
            }
        }
    }
}

static void start_softap(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = DASHBOARD_SSID,
            .ssid_len = strlen(DASHBOARD_SSID),
            .channel = DASHBOARD_WIFI_CHANNEL,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
            .ssid_hidden = 0,
            .beacon_interval = 100,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();

    tm_printf((UB *)"[DASHBOARD] Wi-Fi SoftAP started: SSID='%s' Ch=%d IP=192.168.4.1\n",
              DASHBOARD_SSID, DASHBOARD_WIFI_CHANNEL);
}

static void start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_uri_handlers = 10;

    httpd_uri_t root_uri = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = root_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t app_js_uri = {
        .uri      = "/app.js",
        .method   = HTTP_GET,
        .handler  = app_js_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t style_css_uri = {
        .uri      = "/style.css",
        .method   = HTTP_GET,
        .handler  = style_css_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t api_pcap_uri = {
        .uri      = "/api/pcap",
        .method   = HTTP_GET,
        .handler  = api_pcap_chunk_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t telemetry_uri = {
        .uri      = "/telemetry",
        .method   = HTTP_GET,
        .handler  = telemetry_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t pcap_uri = {
        .uri      = "/capture.pcap",
        .method   = HTTP_GET,
        .handler  = capture_pcap_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t ws_uri = {
        .uri        = "/ws",
        .method     = HTTP_GET,
        .handler    = ws_handler,
        .user_ctx   = NULL,
        .is_websocket = true,
    };

    httpd_uri_t cmd_uri = {
        .uri      = "/cmd",
        .method   = HTTP_POST,
        .handler  = cmd_post_handler,
        .user_ctx = NULL,
    };

    if (httpd_start(&s_http_server, &config) == ESP_OK) {
        httpd_register_uri_handler(s_http_server, &root_uri);
        httpd_register_uri_handler(s_http_server, &app_js_uri);
        httpd_register_uri_handler(s_http_server, &style_css_uri);
        httpd_register_uri_handler(s_http_server, &api_pcap_uri);
        httpd_register_uri_handler(s_http_server, &telemetry_uri);
        httpd_register_uri_handler(s_http_server, &pcap_uri);
        httpd_register_uri_handler(s_http_server, &ws_uri);
        httpd_register_uri_handler(s_http_server, &cmd_uri);
        tm_printf((UB *)"[DASHBOARD] HTTP & WebSocket server online on port 80 (SD /WWW/ + /api/pcap chunking, FR-9, FR-10)\n");
    } else {
        tm_printf((UB *)"[DASHBOARD] ERROR: Failed to start HTTP server\n");
    }
}

EXPORT void wtf_dashboard_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        s_ws_fds[i] = -1;
    }

    start_softap();
    start_web_server();

    wtf_record_t rec;

    for (;;) {
        /* Drain all pending records from telemetry queue within 500 ms cycle (2 Hz broadcast rate) */
        int64_t cycle_end_us = esp_timer_get_time() + 500000LL;
        while (esp_timer_get_time() < cycle_end_us) {
            INT rcv_len = tk_rcv_mbf(g_mbf_telemetry, &rec, 50);
            if (rcv_len > 0) {
                if (rec.protocol == PROTO_UART) {
                    s_uart_cnt++;
                } else if (rec.protocol == PROTO_I2C) {
                    s_i2c_cnt++;
                } else if (rec.protocol == PROTO_WIFI) {
                    s_wifi_cnt++;
                } else if (rec.protocol == PROTO_IEEE802154) {
                    s_ieee154_cnt++;
                } else if (rec.protocol == PROTO_FAULT_ALERT) {
                    s_fuzz_cnt++;
                    if (g_mbf_storage > 0) {
                        (void)tk_snd_mbf(g_mbf_storage, &rec, sizeof(rec), TMO_POL);
                    }
                    wtf_dashboard_broadcast_alert("Hardware fault injected on bus");
                }
                bool emit_ser = (rec.protocol != PROTO_I2C) || ((s_i2c_cnt % 5U) == 1U) || (rec.length > 10U);
                if (emit_ser) {
                    char hex_str[WTF_PKT_DATA_MAX * 2 + 1];
                    static const char k_hex[] = "0123456789ABCDEF";
                    uint16_t n = (rec.length <= WTF_PKT_DATA_MAX) ? rec.length : WTF_PKT_DATA_MAX;
                    for (uint16_t hi = 0; hi < n; hi++) {
                        hex_str[hi * 2U]      = k_hex[(rec.data[hi] >> 4) & 0x0FU];
                        hex_str[hi * 2U + 1U] = k_hex[rec.data[hi] & 0x0FU];
                    }
                    hex_str[n * 2U] = '\0';
                    esp_rom_printf("[PCAP:PKT] %u,%u,%s\n", (unsigned)rec.protocol, (unsigned)rec.probe_id, hex_str);
                }
            }
        }

        /* 2 Hz WebSocket Broadcast (FR-9, TC-5.1) */
        broadcast_telemetry_frame();

        /* Periodic serial telemetry line at 2 Hz */
        tm_printf((UB *)"[DASHBOARD:2Hz] Telemetry: UART_pkts=%u | I2C_pkts=%u | WiFi_pkts=%u | 802154_pkts=%u | Alerts=%u | Safety=%s\n",
                  (unsigned)s_uart_cnt, (unsigned)s_i2c_cnt, (unsigned)s_wifi_cnt, (unsigned)s_ieee154_cnt,
                  (unsigned)s_fuzz_cnt,
                  wtf_safety_is_aborted() ? "Stopped" : "Active");
        /* Iris proximity/verified telemetry on same 2 Hz cycle */
        tm_printf((UB *)"[DASHBOARD:2Hz] Iris: RSSI=%d dBm | Verified=%s\n",
                  (int)wtf_sniffer_get_iris_rssi(),
                  wtf_sniffer_is_iris_verified() ? "true" : "false");
    }
}
