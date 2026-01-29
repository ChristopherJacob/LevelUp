// wifi_mgr.c — drop-in replacement
//
// Features:
//  - First boot (no saved creds): starts SoftAP "Leveler-XXXX" (WPA2 password: "leveler-setup")
//    and serves a setup UI at http://192.168.4.1/
//  - Setup UI includes SSID scan + dropdown, plus manual SSID/password for hidden networks.
//  - Saves creds to NVS and reboots.
//  - Subsequent boots (saved creds): starts STA, connects, and serves a status UI at
//    http://<sta_ip>/status and JSON at /status.json
//  - Factory reset endpoint: POST /reset clears Wi-Fi creds + leveler offsets and reboots.
//  - wifi_mgr_update_angles(roll,pitch) lets IMU task publish latest readings for status page.
//
// Notes:
//  - Uses chunked HTTP responses to avoid snprintf truncation warnings with -Werror.
//  - Starts HTTP server in AP mode immediately; starts it in STA mode after STA_GOT_IP.
//  - AP uses WIFI_MODE_APSTA so scanning works while AP is up (iPhone friendly).

#include "wifi_mgr.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/portmacro.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "esp_err.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/inet.h"

/* ---------------- logging ---------------- */
static const char *TAG = "wifi_mgr";

/* ---------------- NVS keys ---------------- */
#define NVS_NS_WIFI   "wifi"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"

/* Offsets (from your ui.c) for factory reset */
#define NVS_NS_LEVELER   "leveler"
#define NVS_KEY_ROLL0    "roll0_md"
#define NVS_KEY_PITCH0   "pitch0_md"

/* ---------------- AP defaults ---------------- */
#define AP_IP_ADDR    "192.168.4.1"
#define AP_NETMASK    "255.255.255.0"
#define AP_GW_ADDR    "192.168.4.1"
#define AP_CHANNEL    6
#define AP_MAX_CONN   4
#define AP_PASSWORD   "leveler-setup"  // WPA2-PSK requires >= 8 chars

/* ---------------- state ---------------- */
static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0

static esp_netif_t *s_netif_sta = NULL;
static esp_netif_t *s_netif_ap  = NULL;

static httpd_handle_t s_httpd = NULL;
static bool s_http_started = false;

static bool s_have_creds = false;
static bool s_running_ap = false;

static bool s_wifi_inited = false;

/* ---------------- creds buffer ---------------- */
static char s_ssid[33] = {0};   // 32 + null
static char s_pass[65] = {0};   // 64 + null

/* ---------------- angle cache for /status ---------------- */
static portMUX_TYPE s_angle_mux = portMUX_INITIALIZER_UNLOCKED;
static float s_last_roll_deg = 0.0f;
static float s_last_pitch_deg = 0.0f;

/* =========================================================
 * Small helpers
 * ========================================================= */
static void safe_strcpy(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    if (!src) { dst[0] = 0; return; }
    strlcpy(dst, src, dst_len);
}

static void ip4_to_str(const esp_ip4_addr_t *a, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    if (!a) { safe_strcpy(out, out_len, ""); return; }
    char tmp[16];
    snprintf(tmp, sizeof(tmp), IPSTR, IP2STR(a));
    safe_strcpy(out, out_len, tmp);
}

static void url_decode_inplace(char *s)
{
    // minimal + and %xx decode, enough for SSID/pass
    if (!s) return;
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') { *o++ = ' '; continue; }
        if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
            continue;
        }
        *o++ = *p;
    }
    *o = 0;
}

static bool form_get_value(const char *body, const char *key, char *out, size_t out_len)
{
    // parse key=value&key2=value2 (tiny form parser)
    if (!body || !key || !out || out_len == 0) return false;

    const size_t klen = strlen(key);
    const char *p = body;

    while (p && *p) {
        const char *eq = strchr(p, '=');
        if (!eq) break;

        const char *amp = strchr(eq + 1, '&');
        size_t name_len = (size_t)(eq - p);

        if (name_len == klen && strncmp(p, key, klen) == 0) {
            size_t val_len = amp ? (size_t)(amp - (eq + 1)) : strlen(eq + 1);
            if (val_len >= out_len) val_len = out_len - 1;
            memcpy(out, eq + 1, val_len);
            out[val_len] = 0;
            url_decode_inplace(out);
            return true;
        }

        p = amp ? amp + 1 : NULL;
    }
    return false;
}

/* =========================================================
 * NVS helpers
 * ========================================================= */
static bool nvs_load_wifi(char *ssid_out, size_t ssid_len, char *pass_out, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READONLY, &h) != ESP_OK) return false;

    size_t ssid_sz = ssid_len;
    size_t pass_sz = pass_len;

    esp_err_t e1 = nvs_get_str(h, NVS_KEY_SSID, ssid_out, &ssid_sz);
    esp_err_t e2 = nvs_get_str(h, NVS_KEY_PASS, pass_out, &pass_sz);

    nvs_close(h);
    return (e1 == ESP_OK && e2 == ESP_OK && ssid_out[0] != '\0');
}

static esp_err_t nvs_save_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS_WIFI, NVS_READWRITE, &h), TAG, "nvs_open failed");

    ESP_RETURN_ON_ERROR(nvs_set_str(h, NVS_KEY_SSID, ssid), TAG, "nvs_set_str ssid failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, NVS_KEY_PASS, pass ? pass : ""), TAG, "nvs_set_str pass failed");

    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void nvs_factory_reset(void)
{
    // Clear Wi-Fi creds
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }

    // Clear offsets (keep consistent with your ui.c keys)
    if (nvs_open(NVS_NS_LEVELER, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_ROLL0);
        nvs_erase_key(h, NVS_KEY_PITCH0);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* =========================================================
 * Wi-Fi scan helpers (AP setup)
 * ========================================================= */
typedef struct {
    char ssid[33];
    int  rssi;
    wifi_auth_mode_t auth;
} ap_item_t;

static int do_scan(ap_item_t *out, int max_items)
{
    if (!out || max_items <= 0) return 0;

    // Scan while AP is up: requires APSTA mode (we use that in start_ap()).
    wifi_scan_config_t sc = {
        .ssid = 0,
        .bssid = 0,
        .channel = 0,
        .show_hidden = true
    };

    esp_err_t err = esp_wifi_scan_start(&sc, true /* block */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t n = 0;
    err = esp_wifi_scan_get_ap_num(&n);
    if (err != ESP_OK || n == 0) return 0;

    wifi_ap_record_t *recs = (wifi_ap_record_t *)calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) return 0;

    uint16_t got = n;
    err = esp_wifi_scan_get_ap_records(&got, recs);
    if (err != ESP_OK) {
        free(recs);
        return 0;
    }

    // copy unique SSIDs (simple de-dupe)
    int out_n = 0;
    for (int i = 0; i < got && out_n < max_items; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid || ssid[0] == 0) continue;

        bool dup = false;
        for (int j = 0; j < out_n; j++) {
            if (strcmp(out[j].ssid, ssid) == 0) { dup = true; break; }
        }
        if (dup) continue;

        safe_strcpy(out[out_n].ssid, sizeof(out[out_n].ssid), ssid);
        out[out_n].rssi = recs[i].rssi;
        out[out_n].auth = recs[i].authmode;
        out_n++;
    }

    free(recs);
    return out_n;
}

/* =========================================================
 * HTTP server helpers (chunked responses)
 * ========================================================= */
static esp_err_t send_chunk(httpd_req_t *req, const char *s)
{
    if (!s) s = "";
    return httpd_resp_send_chunk(req, s, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_chunkf(httpd_req_t *req, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return ESP_FAIL;
    return httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* =========================================================
 * HTML: Setup page (with scan dropdown)
 * ========================================================= */
static esp_err_t http_root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");

    // Simple single page:
    // - button to scan (AJAX) -> populates select
    // - manual SSID/password fields remain
    send_chunk(req,
        "<!doctype html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
        "<title>Leveler Wi-Fi Setup</title>"
        "<style>"
        "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;margin:24px;}"
        ".card{max-width:520px;margin:0 auto;padding:18px;border:1px solid #ddd;border-radius:18px;}"
        "label{display:block;margin:12px 0 6px;font-weight:600;}"
        "input,select{width:100%;font-size:16px;padding:10px;border:1px solid #ccc;border-radius:10px;}"
        "button{margin-top:14px;width:100%;padding:12px;font-size:16px;border:0;border-radius:12px;background:#111;color:#fff;}"
        ".muted{color:#666;font-size:13px}"
        "</style>"
        "</head><body><div class='card'>"
        "<h2>Connect Leveler to Wi-Fi</h2>"
        "<p class='muted'>Select a network from the scan, or type one (hidden SSIDs supported).</p>"
        "<button id='scanBtn' type='button'>Scan for Wi-Fi Networks</button>"
        "<label>Networks</label>"
        "<select id='netSelect'><option value=''>Press \"Scan for Wi-Fi Networks\" Fist</option></select>"
        "<form method='POST' action='/save'>"
        "<label>SSID</label><input id='ssid' name='ssid' maxlength='32' required>"
        "<label>Password</label><input name='pass' maxlength='64' type='password'>"
        "<button type='submit'>Save &amp; Reboot</button>"
        "</form>"
        "<p class='muted' style='margin-top:10px'>Status: <a href='/status'>/status</a> &nbsp; JSON: <a href='/status.json'>/status.json</a></p>"
        "<script>"
        "const sel=document.getElementById('netSelect');"
        "const ssid=document.getElementById('ssid');"
        "sel.addEventListener('change',()=>{ if(sel.value) ssid.value=sel.value; });"
        "document.getElementById('scanBtn').addEventListener('click', async ()=>{"
        "  sel.innerHTML='<option>Scanning for networks</option>';"
        "  try{"
        "    const r=await fetch('/scan.json',{cache:'no-store'});"
        "    const j=await r.json();"
        "    const items=j.networks||[];"
        "    sel.innerHTML='<option value=\"\">Select SSID</option>';"
        "    items.forEach(n=>{"
        "      const o=document.createElement('option');"
        "      o.value=n.ssid;"
        "      o.textContent=`${n.ssid}  (${n.rssi} dBm)`;"
        "      sel.appendChild(o);"
        "    });"
        "    if(items.length===0) sel.innerHTML='<option value=\"\">No networks found</option>';"
        "  }catch(e){"
        "    sel.innerHTML='<option value=\"\">Scan failed</option>';"
        "  }"
        "});"
        "</script>"
        "</div></body></html>"
    );

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* =========================================================
 * HTTP: /scan.json
 * ========================================================= */
static esp_err_t http_scan_json_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    ap_item_t items[20];
    int n = do_scan(items, (int)(sizeof(items)/sizeof(items[0])));

    send_chunk(req, "{");
    send_chunkf(req, "\"count\":%d,", n);
    send_chunk(req, "\"networks\":[");
    for (int i = 0; i < n; i++) {
        const char *comma = (i == n - 1) ? "" : ",";
        // Minimal JSON escaping for SSID (handle quotes/backslashes)
        char esc[80] = {0};
        const char *in = items[i].ssid;
        char *o = esc;
        size_t left = sizeof(esc) - 1;
        while (*in && left > 0) {
            if (*in == '\"' || *in == '\\') {
                if (left < 2) break;
                *o++ = '\\'; *o++ = *in++; left -= 2;
            } else {
                *o++ = *in++; left--;
            }
        }
        *o = 0;

        send_chunkf(req, "{\"ssid\":\"%s\",\"rssi\":%d}%s", esc, items[i].rssi, comma);
    }
    send_chunk(req, "]}");

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* =========================================================
 * HTTP: /save (POST)
 * ========================================================= */
static esp_err_t http_save_post(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad content length");
        return ESP_OK;
    }

    char *body = (char *)calloc(1, (size_t)total + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No mem");
        return ESP_OK;
    }

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_OK;
        }
        received += r;
    }
    body[received] = 0;

    char ssid[33] = {0};
    char pass[65] = {0};

    if (!form_get_value(body, "ssid", ssid, sizeof(ssid))) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_OK;
    }
    (void)form_get_value(body, "pass", pass, sizeof(pass)); // optional for open nets

    free(body);

    ESP_LOGI(TAG, "Saving Wi-Fi SSID='%s' (pass len=%d)", ssid, (int)strlen(pass));
    if (nvs_save_wifi(ssid, pass) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<html><body><h3>Saved!</h3><p>Rebooting...</p></body></html>");

    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
    return ESP_OK;
}

/* =========================================================
 * HTTP: /status.json
 * ========================================================= */
static esp_err_t http_status_json_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    // Angles
    float roll, pitch;
    portENTER_CRITICAL(&s_angle_mux);
    roll = s_last_roll_deg;
    pitch = s_last_pitch_deg;
    portEXIT_CRITICAL(&s_angle_mux);

    // IP info
    char ipbuf[16] = {0}, gwbuf[16] = {0}, nmbuf[16] = {0};
    int rssi = 0;

    if (!s_running_ap && s_netif_sta) {
        esp_netif_ip_info_t ip = {0};
        if (esp_netif_get_ip_info(s_netif_sta, &ip) == ESP_OK) {
            ip4_to_str(&ip.ip, ipbuf, sizeof(ipbuf));
            ip4_to_str(&ip.gw, gwbuf, sizeof(gwbuf));
            ip4_to_str(&ip.netmask, nmbuf, sizeof(nmbuf));
        }
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
    } else if (s_running_ap && s_netif_ap) {
        esp_netif_ip_info_t ip = {0};
        if (esp_netif_get_ip_info(s_netif_ap, &ip) == ESP_OK) {
            ip4_to_str(&ip.ip, ipbuf, sizeof(ipbuf));
            ip4_to_str(&ip.gw, gwbuf, sizeof(gwbuf));
            ip4_to_str(&ip.netmask, nmbuf, sizeof(nmbuf));
        }
    }

    send_chunk(req, "{");
    send_chunkf(req, "\"mode\":\"%s\",", s_running_ap ? "AP" : "STA");
    send_chunkf(req, "\"have_creds\":%s,", s_have_creds ? "true" : "false");
    send_chunkf(req, "\"configured_ssid\":\"%s\",", s_have_creds ? s_ssid : "");
    send_chunkf(req, "\"connected\":%s,", wifi_mgr_is_connected() ? "true" : "false");
    send_chunkf(req, "\"ip\":\"%s\",", ipbuf);
    send_chunkf(req, "\"gateway\":\"%s\",", gwbuf);
    send_chunkf(req, "\"netmask\":\"%s\",", nmbuf);
    send_chunkf(req, "\"rssi\":%d,", rssi);
    send_chunkf(req, "\"roll_deg\":%.3f,", roll);
    send_chunkf(req, "\"pitch_deg\":%.3f", pitch);
    send_chunk(req, "}");

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* =========================================================
 * HTTP: /status (HTML)
 * ========================================================= */
static esp_err_t http_status_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    float roll, pitch;
    portENTER_CRITICAL(&s_angle_mux);
    roll = s_last_roll_deg;
    pitch = s_last_pitch_deg;
    portEXIT_CRITICAL(&s_angle_mux);

    char ipbuf[16] = {0}, gwbuf[16] = {0}, nmbuf[16] = {0};
    int rssi = 0;

    if (!s_running_ap && s_netif_sta) {
        esp_netif_ip_info_t ip = {0};
        if (esp_netif_get_ip_info(s_netif_sta, &ip) == ESP_OK) {
            ip4_to_str(&ip.ip, ipbuf, sizeof(ipbuf));
            ip4_to_str(&ip.gw, gwbuf, sizeof(gwbuf));
            ip4_to_str(&ip.netmask, nmbuf, sizeof(nmbuf));
        }
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
    } else if (s_running_ap && s_netif_ap) {
        esp_netif_ip_info_t ip = {0};
        if (esp_netif_get_ip_info(s_netif_ap, &ip) == ESP_OK) {
            ip4_to_str(&ip.ip, ipbuf, sizeof(ipbuf));
            ip4_to_str(&ip.gw, gwbuf, sizeof(gwbuf));
            ip4_to_str(&ip.netmask, nmbuf, sizeof(nmbuf));
        }
    }

    send_chunk(req,
        "<!doctype html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
        "<title>Leveler Status</title>"
        "<style>"
        "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;margin:24px;}"
        ".card{max-width:860px;margin:0 auto;padding:18px;border:1px solid #ddd;border-radius:18px;}"
        "table{width:100%;border-collapse:collapse;margin-top:10px}"
        "td{padding:10px 8px;border-bottom:1px solid #eee;vertical-align:top}"
        ".muted{color:#666;font-size:13px}"
        "button{margin-top:14px;width:100%;padding:12px;font-size:16px;border:0;border-radius:12px;background:#b00020;color:#fff;}"
        "a{color:#005bd1;text-decoration:none}"
        "</style></head><body><div class='card'>"
        "<h2>Leveler Status</h2>"
    );

    send_chunk(req, "<table>");
    send_chunkf(req, "<tr><td><b>Mode</b></td><td>%s</td></tr>", s_running_ap ? "AP (setup)" : "STA");
    send_chunkf(req, "<tr><td><b>Configured SSID</b></td><td>%s</td></tr>", s_have_creds ? s_ssid : "(none)");
    send_chunkf(req, "<tr><td><b>Connected</b></td><td>%s</td></tr>", wifi_mgr_is_connected() ? "YES" : "NO");
    send_chunkf(req, "<tr><td><b>IP</b></td><td>%s</td></tr>", ipbuf[0] ? ipbuf : "(none)");
    send_chunkf(req, "<tr><td><b>Gateway</b></td><td>%s</td></tr>", gwbuf[0] ? gwbuf : "(none)");
    send_chunkf(req, "<tr><td><b>Netmask</b></td><td>%s</td></tr>", nmbuf[0] ? nmbuf : "(none)");
    send_chunkf(req, "<tr><td><b>RSSI</b></td><td>%d</td></tr>", rssi);
    send_chunkf(req, "<tr><td><b>Roll</b></td><td>%.3f&deg;</td></tr>", roll);
    send_chunkf(req, "<tr><td><b>Pitch</b></td><td>%.3f&deg;</td></tr>", pitch);
    send_chunk(req, "</table>");

    send_chunk(req,
        "<p class='muted' style='margin-top:10px'>"
        "JSON: <a href='/status.json'>/status.json</a>"
        "</p>"
        "<form method='POST' action='/reset' "
        "onsubmit=\"return confirm('Factory reset? This clears Wi-Fi + offsets and reboots.');\">"
        "<button type='submit'>Factory Reset</button>"
        "</form>"
        "<p class='muted' style='margin-top:10px'>Setup: <a href='/'>/</a></p>"
        "</div></body></html>"
    );

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* =========================================================
 * HTTP: /reset (POST)
 * ========================================================= */
static esp_err_t http_reset_post(httpd_req_t *req)
{
    (void)req;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<html><body><h3>Resetting…</h3><p>Rebooting…</p></body></html>");

    nvs_factory_reset();

    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
    return ESP_OK;
}

/* =========================================================
 * HTTP server start / ensure
 * ========================================================= */
static httpd_handle_t start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    // Keep defaults for stack/priority unless you need to tune.

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) return NULL;

    // Root (setup UI)
    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = http_root_get,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &root);

    // Scan
    httpd_uri_t scan = {
        .uri = "/scan.json",
        .method = HTTP_GET,
        .handler = http_scan_json_get,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &scan);

    // Save
    httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = http_save_post,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &save);

    // Status
    httpd_uri_t status = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = http_status_get,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &status);

    httpd_uri_t status_json = {
        .uri = "/status.json",
        .method = HTTP_GET,
        .handler = http_status_json_get,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &status_json);

    // Reset
    httpd_uri_t reset = {
        .uri = "/reset",
        .method = HTTP_POST,
        .handler = http_reset_post,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &reset);

    // Captive portal-ish wildcard: serve setup UI for unknown GETs
    httpd_uri_t wildcard = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = http_root_get,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &wildcard);

    return server;
}

static void ensure_http_server_started(void)
{
    if (s_http_started) return;

    s_httpd = start_http_server();
    if (s_httpd) {
        s_http_started = true;
        ESP_LOGI(TAG, "HTTP server started (mode=%s)", s_running_ap ? "AP" : "STA");
    } else {
        ESP_LOGW(TAG, "HTTP server failed to start");
    }
}

/* =========================================================
 * Wi-Fi event handler
 * ========================================================= */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA start -> connect");
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGW(TAG, "STA disconnected -> retry");
                xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
                esp_wifi_connect();
                break;

            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
                ESP_LOGI(TAG, "AP client connected: " MACSTR " AID=%d", MAC2STR(e->mac), e->aid);
                break;
            }

            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
                ESP_LOGI(TAG, "AP client disconnected: " MACSTR " AID=%d", MAC2STR(e->mac), e->aid);
                break;
            }

            default:
                break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
            ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&e->ip_info.ip));
            xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);

            // <<< KEY FIX: start HTTP server in STA mode after we have an IP >>>
            ensure_http_server_started();
        }
#ifdef IP_EVENT_AP_STAIPASSIGNED
        else if (id == IP_EVENT_AP_STAIPASSIGNED) {
            ip_event_ap_staipassigned_t *e = (ip_event_ap_staipassigned_t *)data;
            ESP_LOGI(TAG, "AP assigned client IP: " IPSTR, IP2STR(&e->ip));
        }
#endif
    }
}

/* =========================================================
 * Wi-Fi init once
 * ========================================================= */
static esp_err_t wifi_init_once(void)
{
    if (s_wifi_inited) return ESP_OK;

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) return err;

    // Register handlers once
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL),
                        TAG, "wifi event reg failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL),
                        TAG, "ip event reg failed");

    s_wifi_inited = true;
    return ESP_OK;
}

/* =========================================================
 * STA mode
 * ========================================================= */
static esp_err_t start_sta(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "Starting STA: ssid='%s'", ssid);

    s_running_ap = false;

    if (!s_netif_sta) {
        s_netif_sta = esp_netif_create_default_wifi_sta();
        if (!s_netif_sta) return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(wifi_init_once(), TAG, "wifi_init_once failed");

    wifi_config_t cfg = {0};
    safe_strcpy((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), ssid);
    safe_strcpy((char *)cfg.sta.password, sizeof(cfg.sta.password), pass ? pass : "");
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode sta failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    // iPhone + some routers behave better with PS disabled during onboarding/testing
    esp_wifi_set_ps(WIFI_PS_NONE);

    // HTTP server will start on STA_GOT_IP (see handler)
    return ESP_OK;
}

/* =========================================================
 * AP mode (setup)
 * ========================================================= */
static esp_err_t start_ap(void)
{
    s_running_ap = true;

    if (!s_netif_ap) {
        s_netif_ap = esp_netif_create_default_wifi_ap();
        if (!s_netif_ap) return ESP_FAIL;
    }

    // Set AP IP to 192.168.4.1
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = esp_ip4addr_aton(AP_IP_ADDR);
    ip.gw.addr = esp_ip4addr_aton(AP_GW_ADDR);
    ip.netmask.addr = esp_ip4addr_aton(AP_NETMASK);

    ESP_ERROR_CHECK(esp_netif_dhcps_stop(s_netif_ap));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_netif_ap, &ip));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_netif_ap));

    ESP_RETURN_ON_ERROR(wifi_init_once(), TAG, "wifi_init_once failed");

    // Generate SSID like "Leveler-1A2B"
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap_ssid[33];
    snprintf(ap_ssid, sizeof(ap_ssid), "Leveler-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap_cfg = {0};
    safe_strcpy((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), ap_ssid);
    ap_cfg.ap.ssid_len = (uint8_t)strlen(ap_ssid);
    ap_cfg.ap.channel = AP_CHANNEL;
    ap_cfg.ap.max_connection = AP_MAX_CONN;

    // WPA2 makes iPhone happier vs OPEN in some environments.
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    safe_strcpy((char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password), AP_PASSWORD);

    // IMPORTANT: APSTA so scan works while AP is up
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set mode apsta failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "set ap cfg failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAG, "AP started: SSID=%s  PASS=%s  (browse http://%s/)",
             ap_ssid, AP_PASSWORD, AP_IP_ADDR);

    // Start HTTP server immediately in AP mode
    ensure_http_server_started();

    return ESP_OK;
}

/* =========================================================
 * Public API
 * ========================================================= */
esp_err_t wifi_mgr_init(void)
{
    // NVS init (required for Wi-Fi + saved creds)
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else if (nvs_err != ESP_OK) {
        ESP_ERROR_CHECK(nvs_err);
    }

    // These may already be initialized elsewhere; treat invalid-state as OK.
    esp_err_t e;

    e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(e, TAG, "esp_netif_init failed");
    }

    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(e, TAG, "event loop create failed");
    }

    if (!s_wifi_events) {
        s_wifi_events = xEventGroupCreate();
        if (!s_wifi_events) return ESP_ERR_NO_MEM;
    }

    // reset per-boot server guard
    s_http_started = false;

    // Load creds
    memset(s_ssid, 0, sizeof(s_ssid));
    memset(s_pass, 0, sizeof(s_pass));
    s_have_creds = nvs_load_wifi(s_ssid, sizeof(s_ssid), s_pass, sizeof(s_pass));
    ESP_LOGI(TAG, "Have saved creds? %s", s_have_creds ? "YES" : "NO");

    if (s_have_creds) {
        return start_sta(s_ssid, s_pass);
    } else {
        return start_ap();
    }
}

bool wifi_mgr_is_connected(void)
{
    if (!s_wifi_events) return false;
    return (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

void wifi_mgr_update_angles(float roll_deg, float pitch_deg)
{
    portENTER_CRITICAL(&s_angle_mux);
    s_last_roll_deg = roll_deg;
    s_last_pitch_deg = pitch_deg;
    portEXIT_CRITICAL(&s_angle_mux);
}
