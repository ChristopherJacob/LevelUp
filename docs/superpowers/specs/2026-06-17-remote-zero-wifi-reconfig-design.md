# Remote Zero + WiFi Reconfigure — Design Spec

**Date:** 2026-06-17
**Status:** Approved (design) — ready for implementation plan
**Branch:** feature/remote-zero-wifi-reconfig
**Components:** `main/mqtt_mgr.c/.h` (Feature A), `main/wifi_mgr.c/.h` (Feature B)

## Problem

Two small operability gaps after the leveling-guidance feature:
1. **No remote "set zero".** Zeroing requires the physical button or the on-device hold-to-zero. There is no way to zero from Home Assistant. (The web `/wizard/zero` endpoint exists but is CSRF-protected and session-bound, so it is not callable from HA/automations.)
2. **No on-demand WiFi reconfigure.** Changing the device's WiFi network currently relies on automatic AP fallback after repeated STA failures, or a full factory reset (which also wipes MQTT/hostname/config). There is no button to drop to AP setup mode on demand while preserving all other settings.

## Goals

- Add a Home Assistant **button** that zeroes the device, via a new inbound MQTT command path.
- Add a web **"Reconfigure WiFi"** button that switches the device into AP/captive-portal setup mode on demand, **without losing** any other configuration (MQTT, hostname, leveling orientation/mode, vehicle dimensions, calibration offsets).

## Non-goals

- Exposing other actions over MQTT now (the command topic is structured to allow it later, but only `"zero"` is implemented).
- On-device (AMOLED) UI for either feature.
- Changing the existing automatic AP-fallback behavior or the factory reset.
- Host unit tests (both features are MQTT/LVGL/WiFi-coupled; verification is on-device).

---

## Feature A — HA "Set Zero" button (MQTT command)

This is the first **inbound** MQTT path; MQTT was previously publish-only.

### Topic
- New per-device command topic built in `mqtt_mgr_build_topics()`:
  `s_cmd_topic = "<mqtt_topic_prefix>/<device_id>/cmd"` (same prefix/device-id scheme as the existing state/availability topics).

### Subscribe
- On `MQTT_EVENT_CONNECTED` (in `mqtt_mgr_handle_event`), after publishing availability, call
  `esp_mqtt_client_subscribe(s_client, s_cmd_topic, 1)` (QoS 1).

### Dispatch
- Add an `MQTT_EVENT_DATA` case to `mqtt_mgr_handle_event`:
  - The event provides `event->topic`/`event->topic_len` and `event->data`/`event->data_len` (note: not null-terminated; compare with explicit lengths).
  - If the topic matches `s_cmd_topic` (length-checked), dispatch on the payload:
    - payload equals `"zero"` (exact, length 4) → perform the zero action.
    - any other payload → `ESP_LOGW` and ignore.
  - Bound the payload: ignore if `data_len` is 0 or unreasonably large (e.g. > 32).

### Zero action
- Mirror the web `/wizard/zero` handler: take the LVGL lock, call `ui_zero_current()`, release.
  - Use a bounded lock (e.g. `lvgl_port_lock(1000)`); on failure `ESP_LOGW` and skip (user can re-press).
  - `ui_zero_current()` already persists the offset to NVS and updates the display + shows the "Zeroed" toast.
- `mqtt_mgr.c` must include `ui.h` and `lvgl_port.h` for this (confirm; add if missing).
- **Concurrency:** the dispatch runs in the MQTT client task. Lock ordering is safe: the only place that holds the LVGL lock and then takes an mqtt lock is `imu_task` (LVGL → `s_angle_mux`); this path takes only the LVGL lock, so there is no inversion.

### Discovery
- Add a Home Assistant **`button`** discovery entity (new discovery topic
  `s_discovery_zero_btn_topic = "<disc_prefix>/button/<device_id>/zero/config"`), published in `mqtt_mgr_publish_discovery()` using the shared `dev` device block:
  - `name`: "LevelUp Set Zero"
  - `uniq_id`: `<device_id>_zero`
  - `command_topic`: `s_cmd_topic`
  - `payload_press`: `"zero"`
  - `icon`: `mdi:crosshairs-gps`
  - availability via the existing `availability_topic`.
- A small dedicated publish helper (`mqtt_pub_disc_button`) mirrors the existing
  `mqtt_pub_disc_entity` / `mqtt_pub_disc_binary` helpers (button has no state/unit; it has `command_topic` + `payload_press`).

### Error handling
- Payload bounded and exact-matched; unknown commands logged and ignored.
- Lock-timeout logged and skipped.
- No state published for the button (stateless action entity).

---

## Feature B — "Reconfigure WiFi" button (drop to AP, non-destructive)

### Endpoint
- New `POST /wifi_setup` in `wifi_mgr.c`, CSRF-checked, following the existing POST handler pattern (length-bounded `recv` loop, `csrf_ok(body)`, free body).
- Behavior:
  1. If already in AP mode (`s_running_ap`), respond that setup mode is already active and do nothing else.
  2. Otherwise send an HTML response FIRST instructing the user:
     *"Switching to Wi‑Fi setup mode. Connect your phone to the **Leveler-XXXX** network (password `leveler-setup`) and open the setup page (the captive portal should appear, or browse to http://192.168.4.1)."*
  3. After responding, arm a **one-shot `esp_timer`** (~750 ms) whose callback calls `start_ap()`. The delay ensures the HTTP response is delivered before the STA link drops.

### Deferred AP switch
- A static `esp_timer_handle_t s_ap_switch_timer` (created once, or created on demand) with a callback `wifi_setup_timer_cb` that calls `start_ap()`.
- `start_ap()` already exists and is non-destructive: it switches to `WIFI_MODE_APSTA`, starts the SoftAP + captive DNS, sets `s_running_ap = true`. It does **not** erase NVS.
- The esp_timer callback context is acceptable for calling `start_ap()` (the existing `fallback_to_ap()` calls it from the WiFi event task; esp_wifi APIs are callable here). Verify on-device.

### Preservation guarantee
- No NVS namespace is erased. WiFi creds remain until/unless the user saves new ones via the existing `/save` flow (which writes only the WiFi namespace and reboots into STA). MQTT, hostname/network, leveling, and dimensions are untouched.
- If the user never saves new creds, a power cycle reconnects to the old network.

### Button (web UI)
- On the `/status` page (in `http_status_get`), add a "Reconfigure WiFi" button (in the Networking settings area), e.g.:
  `<button onclick="if(confirm('Switch to Wi‑Fi setup mode? This device will leave your network until reconfigured.')){fetch('/wifi_setup',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'csrf_token='+encodeURIComponent(_csrf)}).then(...)}">Reconfigure WiFi</button>`
  - Reuses the page's existing `_csrf`.
  - On success, show the returned guidance (or a short inline note) since the device is about to leave the network.
  - Respect the 512-byte `send_chunkf` limit: emit the button/markup via `send_chunk`.

### Registration
- Forward-declare `http_wifi_setup_post`; register `httpd_uri_t wifi_setup { .uri="/wifi_setup", .method=HTTP_POST, ... }`. Confirm `max_uri_handlers` has headroom (raise if needed, as was done for the leveling endpoints).

### Error handling
- CSRF required (403 on mismatch).
- No-op + informative response if already in AP mode.
- Timer-arm failure logged; if the timer cannot be created, fall back to calling `start_ap()` directly after the response (accepting that the response may not reach the browser).

---

## Testing / Verification (on-device)

Both features are hardware/transport-coupled; verify by flashing (`tools/flash_remote.sh`, or the `cj-mba.local` direct path if the alias IP is stale) and:

**Feature A:**
- HA shows a "LevelUp Set Zero" button; pressing it zeroes the device (on-screen "Zeroed" toast; roll/pitch offsets update; values re-baseline).
- Publishing `"zero"` to `<prefix>/<id>/cmd` manually (e.g. `mosquitto_pub`) also zeroes; an unknown payload does nothing (logged).

**Feature B:**
- On `/status`, "Reconfigure WiFi" prompts for confirmation, shows the setup-mode page, then the device appears as the `Leveler-XXXX` AP within ~1 s.
- Reconnect phone to the AP, set a new network, save → device reboots and joins the new network.
- Confirm MQTT broker settings, hostname, leveling orientation/mode, and vehicle dimensions are all still present after the switch (not reset).

## Future hooks (not implemented)

- The `cmd` topic + payload dispatch is the seam for future MQTT-triggered actions (e.g. switch Blocks/Ramps mode, trigger calibration), and eventually the air-leveling control path.
