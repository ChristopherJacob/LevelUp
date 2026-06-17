# Remote Zero + WiFi Reconfigure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Home Assistant "Set Zero" button (via a new inbound MQTT command topic) and a web "Reconfigure WiFi" button that drops the device into AP setup mode without losing any other configuration.

**Architecture:** Feature A adds the first inbound MQTT path — a per-device `cmd` topic the device subscribes to, with payload dispatch (`"zero"` → `ui_zero_current()` under the LVGL lock), plus an HA `button` discovery entity. Feature B adds a CSRF-checked `POST /wifi_setup` that responds first then arms a one-shot `esp_timer` to call the existing non-destructive `start_ap()`, plus a confirm-guarded button on `/status`.

**Tech Stack:** ESP-IDF 5.5.1, esp-mqtt, esp_http_server, esp_timer, LVGL 9.

**Build/flash reminders:**
- Build: `bash -c "source /Users/cjacob/esp/v5.5.1/esp-idf/export.sh 2>/dev/null && idf.py build"`
- Flash: `bash tools/flash_remote.sh`; if it times out (stale alias IP), flash over mDNS directly — `cjacob@cj-mba.local`, key `~/.ssh/id_ed25519_cjmba`, port `/dev/tty.usbmodem11201` (see memory `feedback_workflow`).
- `wifi_mgr.c` `send_chunkf` has a 512-byte buffer — large HTML/JS via `send_chunk`; only small substitutions via `send_chunkf`.

**Testing note:** Both features are MQTT/WiFi/LVGL-coupled; there are no host unit tests. Each task's gate is a clean `idf.py build`; end-to-end behavior is verified on-device in the final task.

---

## File Structure

| File | Responsibility | Change |
|------|----------------|--------|
| `main/mqtt_mgr.c` | cmd topic + subscribe + DATA dispatch (zero); HA button discovery | Modify |
| `main/wifi_mgr.c` | `POST /wifi_setup` + deferred AP-switch timer; `/status` Reconfigure WiFi button | Modify |
| `docs/mqtt-discovery.md` | document the new Set Zero button + command topic | Modify |

No header changes are required (no new cross-module public functions — the zero action reuses `ui_zero_current()` from `ui.h`, and `start_ap()` is file-local to `wifi_mgr.c`).

---

## Task 1: MQTT command topic — subscribe + zero dispatch

**Files:**
- Modify: `main/mqtt_mgr.c` (includes ~line 25; topic statics ~44-60; `mqtt_mgr_build_topics` ~138-155; `mqtt_mgr_handle_event` ~430-446)

- [ ] **Step 1: Add includes for the zero action.** In `main/mqtt_mgr.c`, with the other project includes (right after `#include "leveling.h"` at line 25), add:

```c
#include "ui.h"
#include "lvgl_port.h"
```

- [ ] **Step 2: Declare the command + button-discovery topic strings.** Near the other `static char s_discovery_*_topic[128];` declarations (just after `s_discovery_is_level_topic` ~line 60), add:

```c
static char s_cmd_topic[128];
static char s_discovery_zero_btn_topic[128];
```

- [ ] **Step 3: Build the command topic.** In `mqtt_mgr_build_topics()`, after the `s_discovery_is_level_topic` snprintf (~line 153-155), add:

```c
    snprintf(s_cmd_topic, sizeof(s_cmd_topic),
             "%s/%s/cmd", s_mqtt_topic, s_device_id);
    snprintf(s_discovery_zero_btn_topic, sizeof(s_discovery_zero_btn_topic),
             "%s/button/%s/zero/config", s_mqtt_disc, s_device_id);
```

- [ ] **Step 4: Subscribe on connect.** In `mqtt_mgr_handle_event`, in the `MQTT_EVENT_CONNECTED` block, after the existing availability publish (`esp_mqtt_client_publish(s_client, s_availability_topic, "online", 0, 1, 1);`, ~line 439) and before `mqtt_mgr_publish_discovery();`, add:

```c
        esp_mqtt_client_subscribe(s_client, s_cmd_topic, 1);
```

- [ ] **Step 5: Handle inbound command data.** In `mqtt_mgr_handle_event`, after the `MQTT_EVENT_DISCONNECTED` block (~line 445-447), add a new case:

```c
    if (event_id == MQTT_EVENT_DATA) {
        esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
        if (event && event->topic && event->data &&
            event->topic_len == (int)strlen(s_cmd_topic) &&
            strncmp(event->topic, s_cmd_topic, event->topic_len) == 0) {
            if (event->data_len == 4 && strncmp(event->data, "zero", 4) == 0) {
                if (lvgl_port_lock(1000)) {
                    ui_zero_current();
                    lvgl_port_unlock();
                    ESP_LOGI(TAG, "zeroed via MQTT cmd");
                } else {
                    ESP_LOGW(TAG, "zero cmd: LVGL lock timeout, skipped");
                }
            } else {
                ESP_LOGW(TAG, "ignoring unknown cmd payload (%d bytes)", event->data_len);
            }
        }
        return;
    }
```

- [ ] **Step 6: Build.**

Run: `bash -c "source /Users/cjacob/esp/v5.5.1/esp-idf/export.sh 2>/dev/null && idf.py build"`
Expected: `Project build complete`. (`ui.h` provides `ui_zero_current`; `lvgl_port.h` provides `lvgl_port_lock`/`unlock`. `esp_mqtt_event_handle_t` comes from `mqtt_client.h`, already included. `strlen`/`strncmp` from `<string.h>`, already included.)

- [ ] **Step 7: Commit.**

```bash
git add main/mqtt_mgr.c
git commit -m "feat(mqtt): subscribe to cmd topic and zero on 'zero' payload"
```

---

## Task 2: HA "Set Zero" button discovery

**Files:**
- Modify: `main/mqtt_mgr.c` (`mqtt_pub_disc_binary` ~199-222; `mqtt_mgr_publish_discovery` ~226-330)

- [ ] **Step 1: Add a button discovery helper.** Immediately after the `mqtt_pub_disc_binary(...)` function definition (~line 222), add:

```c
// Publish a button discovery entry (stateless action; uses command_topic/payload_press).
static void mqtt_pub_disc_button(const char *config_topic, const char *name,
                                 const char *uniq_suffix, const char *cmd_topic,
                                 const char *payload_press, const char *opt,
                                 const char *dev_block)
{
    char payload[640];
    int len = snprintf(payload, sizeof(payload),
        "{"
        "\"name\":\"%s\","
        "\"uniq_id\":\"%s_%s\","
        "\"command_topic\":\"%s\","
        "\"payload_press\":\"%s\","
        "\"availability_topic\":\"%s\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\""
        "%s,%s}",
        name, s_device_id, uniq_suffix, cmd_topic, payload_press,
        s_availability_topic, opt, dev_block);
    if (len > 0 && len < (int)sizeof(payload)) {
        esp_mqtt_client_publish(s_client, config_topic, payload, 0, 1, 1);
    } else {
        ESP_LOGW(TAG, "button discovery overflow for '%s' (%d bytes)", name, len);
    }
}
```

- [ ] **Step 2: Publish the Set Zero button.** In `mqtt_mgr_publish_discovery()`, after the `is_level` binary_sensor publish (the `mqtt_pub_disc_binary(s_discovery_is_level_topic, ...)` call ~line 327-328) and before the final `ESP_LOGI(...)`, add:

```c
    mqtt_pub_disc_button(s_discovery_zero_btn_topic, "LevelUp Set Zero", "zero",
                         s_cmd_topic, "zero",
                         ",\"icon\":\"mdi:crosshairs-gps\"", dev);
```

- [ ] **Step 3: Build.**

Run: `bash -c "source /Users/cjacob/esp/v5.5.1/esp-idf/export.sh 2>/dev/null && idf.py build"`
Expected: `Project build complete`.

- [ ] **Step 4: Commit.**

```bash
git add main/mqtt_mgr.c
git commit -m "feat(mqtt): publish HA Set Zero button via discovery"
```

---

## Task 3: `POST /wifi_setup` endpoint + deferred AP switch

**Files:**
- Modify: `main/wifi_mgr.c` (forward decls ~890-893; new handler near `http_network_save_post` ~1330; handler registration block ~3040-3247 area)

`esp_timer.h` is already included (line 43). `start_ap()` is forward-declared (line 890) and `s_running_ap` exists.

- [ ] **Step 1: Add the timer state + callback + handler.** Add the following just BEFORE `http_network_save_post` (~line 1330). The timer is created lazily on first use:

```c
/* =========================================================
 * HTTP: /wifi_setup (POST) — drop to AP setup mode, non-destructive
 * ========================================================= */
static esp_timer_handle_t s_ap_switch_timer = NULL;

// Fires shortly after the HTTP response so the browser receives it before STA drops.
static void wifi_setup_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "User requested Wi-Fi setup: switching to AP mode");
    start_ap();
}

static esp_err_t http_wifi_setup_post(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad content length");
        return ESP_OK;
    }
    char *body = (char *)calloc(1, (size_t)total + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No mem");
        return ESP_OK;
    }
    int rcvd = 0;
    while (rcvd < total) {
        int r = httpd_req_recv(req, body + rcvd, total - rcvd);
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_OK;
        }
        rcvd += r;
    }
    body[rcvd] = '\0';
    if (!csrf_ok(body)) {
        free(body);
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "CSRF token invalid");
        return ESP_OK;
    }
    free(body);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (s_running_ap) {
        httpd_resp_sendstr(req,
            "<html><body style='font-family:system-ui;margin:40px'>"
            "<h3>Already in setup mode.</h3>"
            "<p>Connect to the <b>Leveler-XXXX</b> Wi-Fi and open the setup page.</p>"
            "</body></html>");
        return ESP_OK;
    }

    httpd_resp_sendstr(req,
        "<html><body style='font-family:system-ui;margin:40px'>"
        "<h3>Switching to Wi-Fi setup mode\xe2\x80\xa6</h3>"
        "<p>Connect your phone to the <b>Leveler-XXXX</b> Wi-Fi network "
        "(password <code>leveler-setup</code>), then open the setup page "
        "(the captive portal should appear automatically, or browse to "
        "<a href='http://192.168.4.1'>http://192.168.4.1</a>).</p>"
        "<p>Your other settings (MQTT, hostname, calibration, vehicle "
        "dimensions) are preserved.</p>"
        "</body></html>");

    // Defer the actual AP switch so the response reaches the browser first.
    if (!s_ap_switch_timer) {
        const esp_timer_create_args_t targs = {
            .callback = wifi_setup_timer_cb,
            .name = "ap_switch",
        };
        if (esp_timer_create(&targs, &s_ap_switch_timer) != ESP_OK) {
            ESP_LOGW(TAG, "ap_switch timer create failed; switching to AP now");
            start_ap();
            return ESP_OK;
        }
    }
    esp_timer_start_once(s_ap_switch_timer, 750000); // 750 ms
    return ESP_OK;
}
```

- [ ] **Step 2: Forward-declare the handler.** With the other `static esp_err_t http_*` prototypes (near line 890-893, beside `static esp_err_t http_network_save_post(httpd_req_t *req);`), add:

```c
static esp_err_t http_wifi_setup_post(httpd_req_t *req);
```

- [ ] **Step 3: Register the route.** In the handler-registration block (where `httpd_register_uri_handler` calls live, alongside `network_save` / `display_save` / `leveling_mode`), add:

```c
    httpd_uri_t wifi_setup = {
        .uri = "/wifi_setup",
        .method = HTTP_POST,
        .handler = http_wifi_setup_post,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &wifi_setup);
```

Then find the `httpd_config_t` setup (search for `max_uri_handlers`) and confirm there is room for one more handler (it was raised to 28 for the leveling endpoints; current usage is 27 after this addition, so no change is needed — but verify the number is `>=` the count of `httpd_register_uri_handler` calls; if not, raise it to cover them plus 2 headroom).

- [ ] **Step 4: Build.**

Run: `bash -c "source /Users/cjacob/esp/v5.5.1/esp-idf/export.sh 2>/dev/null && idf.py build"`
Expected: `Project build complete`.

- [ ] **Step 5: Commit.**

```bash
git add main/wifi_mgr.c
git commit -m "feat(wifi_mgr): /wifi_setup drops to AP mode (deferred, non-destructive)"
```

---

## Task 4: "Reconfigure WiFi" button on /status

**Files:**
- Modify: `main/wifi_mgr.c` (`http_status_get`, the Networking settings section — find `action='/network_save'`)

- [ ] **Step 1: Add the button.** In `http_status_get`, locate the Networking settings block (search for `action='/network_save'`). Immediately AFTER that network form's closing markup (still inside the Networking `<details>` section), add this `send_chunk` (must be `send_chunk`, not `send_chunkf` — it exceeds 512 bytes and has format-like `%` characters):

```c
    send_chunk(req,
        "<div style='margin-top:12px;padding-top:12px;border-top:1px solid var(--border)'>"
        "<button type='button' class='wbtn sec' "
        "onclick=\"if(confirm('Switch to Wi-Fi setup mode? This device will leave "
        "your network until reconfigured. Your other settings are kept.')){"
        "fetch('/wifi_setup',{method:'POST',"
        "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:'csrf_token='+encodeURIComponent(_csrf)})"
        ".then(function(r){return r.text();})"
        ".then(function(t){document.open();document.write(t);document.close();})"
        "['catch'](function(){alert('Request failed');});}\">"
        "Reconfigure Wi-Fi</button>"
        "<div style='font-size:12px;color:var(--muted);margin-top:6px'>"
        "Puts the device in setup mode so you can join a different network.</div>"
        "</div>"
    );
```

Notes for the implementer:
- The status page already defines `var _csrf` (used by other POSTs on the page) — do NOT redefine it; this button reuses it.
- The `onclick` attribute uses double quotes, so every JS string inside uses single quotes (no inner double quotes) — keep it that way to avoid breaking the C string / HTML attribute.
- `wbtn sec` is the existing secondary-button class used elsewhere; if the status page doesn't share that CSS class, drop the `class` attribute (the button still works) — verify by checking nearby buttons on the page.

- [ ] **Step 2: Build.**

Run: `bash -c "source /Users/cjacob/esp/v5.5.1/esp-idf/export.sh 2>/dev/null && idf.py build"`
Expected: `Project build complete`.

- [ ] **Step 3: Sanity-check the emitted button JS parses.** Extract the onclick JS body and validate (catches quoting/paren mistakes that the C compiler can't):

```bash
cat > /tmp/btn.js <<'EOF'
if(confirm('x')){fetch('/wifi_setup',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'csrf_token='+encodeURIComponent(_csrf)}).then(function(r){return r.text();}).then(function(t){document.open();document.write(t);document.close();})['catch'](function(){alert('Request failed');});}
EOF
node --check /tmp/btn.js && echo "JS OK"
```
Expected: `JS OK` (node is available on this host; if not, skip this step).

- [ ] **Step 4: Commit.**

```bash
git add main/wifi_mgr.c
git commit -m "feat(wifi_mgr): add Reconfigure Wi-Fi button to /status"
```

---

## Task 5: Docs + on-device verification

**Files:**
- Modify: `docs/mqtt-discovery.md`

- [ ] **Step 1: Document the Set Zero button.** In `docs/mqtt-discovery.md`, add a short subsection: a new Home Assistant **button** entity "LevelUp Set Zero" appears via discovery; pressing it publishes `zero` to the command topic `<topic_prefix>/<device_id>/cmd`, which zeroes the device. Note the command topic is the extensible inbound path (only `zero` is handled today).

- [ ] **Step 2: Commit docs.**

```bash
git add docs/mqtt-discovery.md
git commit -m "docs: document MQTT Set Zero button and command topic"
```

- [ ] **Step 3: Build + flash.**

Run: `bash -c "source /Users/cjacob/esp/v5.5.1/esp-idf/export.sh 2>/dev/null && idf.py build" && bash tools/flash_remote.sh`
(If `flash_remote.sh` times out due to the stale alias IP, flash directly over `cjacob@cj-mba.local` per the memory note.)

- [ ] **Step 4: Verify on-device.**

**Feature A (Set Zero):**
- In Home Assistant, a "LevelUp Set Zero" button appears on the device card. Press it → the device shows the "Zeroed" toast and roll/pitch re-baseline.
- Manual check: `mosquitto_pub -t '<prefix>/<device_id>/cmd' -m zero` zeroes it; `-m bogus` does nothing (logged).

**Feature B (Reconfigure Wi-Fi):**
- On `/status`, click "Reconfigure Wi-Fi" → confirm prompt → the setup-mode page renders → within ~1 s the device appears as the `Leveler-XXXX` AP.
- Join the AP from a phone, set a (possibly different) network, save → device reboots and joins it.
- Confirm MQTT broker settings, hostname, leveling orientation/mode, and vehicle dimensions all survived (not reset).

---

## Self-Review

**Spec coverage:**
- Inbound `cmd` topic + subscribe + payload dispatch (`zero`) — Task 1. ✓
- Zero under LVGL lock, mirrors web handler, safe lock order — Task 1. ✓
- HA `button` discovery entity — Task 2. ✓
- `POST /wifi_setup` CSRF + respond-then-defer + non-destructive `start_ap()` — Task 3. ✓
- One-shot esp_timer (~750 ms), no-op if already AP, timer-create fallback — Task 3. ✓
- `/status` confirm-guarded button reusing `_csrf` — Task 4. ✓
- Docs — Task 5. ✓
- On-device verification incl. config-preservation check — Task 5. ✓
- Extensible cmd topic for future actions — Task 1 structure (payload dispatch). ✓

**Placeholder scan:** No TBD/TODO; all code blocks complete. The two "verify/adapt if the page differs" notes (max_uri_handlers count, `wbtn sec` class) are concrete conditional instructions, not placeholders.

**Type/identifier consistency:** `s_cmd_topic`, `s_discovery_zero_btn_topic`, `mqtt_pub_disc_button`, `wifi_setup_timer_cb`, `s_ap_switch_timer`, `http_wifi_setup_post` are used consistently across tasks. `ui_zero_current()` / `lvgl_port_lock()` / `lvgl_port_unlock()` / `start_ap()` / `csrf_ok()` / `form_get_value()` match existing signatures in the codebase. The HA button payload (`payload_press:"zero"`) matches the dispatch (`data_len==4 && strncmp(...,"zero",4)`).
