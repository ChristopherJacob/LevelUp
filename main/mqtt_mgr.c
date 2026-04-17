// MQTT telemetry + Home Assistant discovery publisher.
#include "mqtt_mgr.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/portmacro.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_check.h"
#include "mqtt_client.h"
#include "nvs.h"

#include "wifi_mgr.h"
#include "lwip/inet.h"

static const char *TAG = "mqtt_mgr";

#define MQTT_CONNECTED_BIT BIT0

static EventGroupHandle_t s_mqtt_events;
static esp_mqtt_client_handle_t s_client;
static TaskHandle_t s_pub_task = NULL;
static int64_t s_task_last_alive_us = 0;

static portMUX_TYPE s_angle_mux = portMUX_INITIALIZER_UNLOCKED;
static float s_last_roll = 0.0f;
static float s_last_pitch = 0.0f;
static float s_last_ax = 0.0f;
static float s_last_ay = 0.0f;
static float s_last_az = 0.0f;

static char s_device_id[32];
static char s_state_topic[128];
static char s_availability_topic[128];
static char s_discovery_roll_topic[128];
static char s_discovery_pitch_topic[128];
static char s_discovery_rssi_topic[128];
static char s_discovery_roll_in_topic[128];
static char s_discovery_pitch_in_topic[128];
static char s_discovery_accel_x_topic[128];
static char s_discovery_accel_y_topic[128];
static char s_discovery_accel_z_topic[128];

static float s_wheelbase_in = 133.0f;
static float s_trackwidth_in = 65.2f;

#define DEG2RAD (0.017453292519943295f)

/* MQTT config (keep in sync with wifi_mgr.c) */
#define NVS_NS_MQTT          "mqtt"
#define NVS_KEY_MQTT_URI     "uri"
#define NVS_KEY_MQTT_USER    "user"
#define NVS_KEY_MQTT_PASS    "pass"
#define NVS_KEY_MQTT_TOPIC   "topic"
#define NVS_KEY_MQTT_DISC    "disc"
#define NVS_KEY_MQTT_ENABLE  "enable"

static char s_mqtt_uri[128];
static char s_mqtt_user[64];
static char s_mqtt_pass[64];
static char s_mqtt_topic[32];
static char s_mqtt_disc[32];
static bool s_mqtt_enable = false;

// Load MQTT configuration from NVS (falling back to Kconfig defaults).
static void mqtt_mgr_load_config(void)
{
    strlcpy(s_mqtt_uri, CONFIG_LEVELUP_MQTT_BROKER_URI, sizeof(s_mqtt_uri));
    strlcpy(s_mqtt_user, CONFIG_LEVELUP_MQTT_USERNAME, sizeof(s_mqtt_user));
    strlcpy(s_mqtt_pass, CONFIG_LEVELUP_MQTT_PASSWORD, sizeof(s_mqtt_pass));
    strlcpy(s_mqtt_topic, CONFIG_LEVELUP_MQTT_TOPIC_PREFIX, sizeof(s_mqtt_topic));
    strlcpy(s_mqtt_disc, CONFIG_LEVELUP_MQTT_DISCOVERY_PREFIX, sizeof(s_mqtt_disc));
    s_mqtt_enable = false;

    nvs_handle_t h;
    if (nvs_open(NVS_NS_MQTT, NVS_READONLY, &h) != ESP_OK) return;

    size_t sz = 0;
    sz = sizeof(s_mqtt_uri);
    (void)nvs_get_str(h, NVS_KEY_MQTT_URI, s_mqtt_uri, &sz);
    sz = sizeof(s_mqtt_user);
    (void)nvs_get_str(h, NVS_KEY_MQTT_USER, s_mqtt_user, &sz);
    sz = sizeof(s_mqtt_pass);
    (void)nvs_get_str(h, NVS_KEY_MQTT_PASS, s_mqtt_pass, &sz);
    sz = sizeof(s_mqtt_topic);
    (void)nvs_get_str(h, NVS_KEY_MQTT_TOPIC, s_mqtt_topic, &sz);
    sz = sizeof(s_mqtt_disc);
    (void)nvs_get_str(h, NVS_KEY_MQTT_DISC, s_mqtt_disc, &sz);
    uint8_t en = 0;
    if (nvs_get_u8(h, NVS_KEY_MQTT_ENABLE, &en) == ESP_OK) {
        s_mqtt_enable = (en != 0);
    }

    nvs_close(h);
}

// Build per-device topic strings using STA MAC.
static void mqtt_mgr_build_topics(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id),
             "levelup_%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    snprintf(s_state_topic, sizeof(s_state_topic),
             "%s/%s/state", s_mqtt_topic, s_device_id);
    snprintf(s_availability_topic, sizeof(s_availability_topic),
             "%s/%s/availability", s_mqtt_topic, s_device_id);

    snprintf(s_discovery_roll_topic, sizeof(s_discovery_roll_topic),
             "%s/sensor/%s/roll/config", s_mqtt_disc, s_device_id);
    snprintf(s_discovery_pitch_topic, sizeof(s_discovery_pitch_topic),
             "%s/sensor/%s/pitch/config", s_mqtt_disc, s_device_id);
    snprintf(s_discovery_rssi_topic, sizeof(s_discovery_rssi_topic),
             "%s/sensor/%s/rssi/config", s_mqtt_disc, s_device_id);
    snprintf(s_discovery_roll_in_topic, sizeof(s_discovery_roll_in_topic),
             "%s/sensor/%s/roll_in/config", s_mqtt_disc, s_device_id);
    snprintf(s_discovery_pitch_in_topic, sizeof(s_discovery_pitch_in_topic),
             "%s/sensor/%s/pitch_in/config", s_mqtt_disc, s_device_id);
    snprintf(s_discovery_accel_x_topic, sizeof(s_discovery_accel_x_topic),
             "%s/sensor/%s/accel_x/config", s_mqtt_disc, s_device_id);
    snprintf(s_discovery_accel_y_topic, sizeof(s_discovery_accel_y_topic),
             "%s/sensor/%s/accel_y/config", s_mqtt_disc, s_device_id);
    snprintf(s_discovery_accel_z_topic, sizeof(s_discovery_accel_z_topic),
             "%s/sensor/%s/accel_z/config", s_mqtt_disc, s_device_id);
}

// Publish a single HA MQTT discovery config entry (retained QoS-1).
// opt: optional extra JSON fields, each starting with a comma (e.g. ",\"icon\":\"mdi:wifi\"").
// dev_block: the full "device":{...} JSON object string.
static void mqtt_pub_disc_entity(const char *config_topic,
                                 const char *name,
                                 const char *uniq_suffix,
                                 const char *value_key,
                                 const char *unit,
                                 const char *opt,
                                 const char *dev_block)
{
    char payload[768];
    int len = snprintf(payload, sizeof(payload),
        "{"
        "\"name\":\"%s\","
        "\"uniq_id\":\"%s_%s\","
        "\"state_topic\":\"%s\","
        "\"availability_topic\":\"%s\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"value_template\":\"{{ value_json.%s }}\","
        "\"unit_of_measurement\":\"%s\","
        "\"state_class\":\"measurement\""
        "%s,"   // optional fields (each prefixed with ,)
        "%s"    // device block
        "}",
        name,
        s_device_id, uniq_suffix,
        s_state_topic,
        s_availability_topic,
        value_key,
        unit,
        opt,
        dev_block);
    if (len > 0 && len < (int)sizeof(payload)) {
        esp_mqtt_client_publish(s_client, config_topic, payload, 0, 1, 1);
    } else {
        ESP_LOGW(TAG, "discovery payload overflow for '%s' (%d bytes)", name, len);
    }
}

// Publish Home Assistant MQTT discovery payloads (retained, QoS 1).
// Entities appear automatically in HA without manual YAML config.
static void mqtt_mgr_publish_discovery(void)
{
    if (!s_client) return;

    // Build configuration_url from current IP (best-effort, omitted if not connected).
    char ipbuf[16] = "";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info = {0};
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            snprintf(ipbuf, sizeof(ipbuf), IPSTR, IP2STR(&ip_info.ip));
        }
    }

    // Shared device block — identical for every entity so HA groups them.
    char dev[256];
    if (ipbuf[0]) {
        snprintf(dev, sizeof(dev),
            "\"device\":{"
            "\"identifiers\":[\"%s\"],"
            "\"name\":\"LevelUp\","
            "\"manufacturer\":\"LevelUp\","
            "\"model\":\"ESP32-S3 AMOLED\","
            "\"configuration_url\":\"http://%s\""
            "}",
            s_device_id, ipbuf);
    } else {
        snprintf(dev, sizeof(dev),
            "\"device\":{"
            "\"identifiers\":[\"%s\"],"
            "\"name\":\"LevelUp\","
            "\"manufacturer\":\"LevelUp\","
            "\"model\":\"ESP32-S3 AMOLED\""
            "}",
            s_device_id);
    }

    // Primary leveling sensors.
    mqtt_pub_disc_entity(s_discovery_roll_topic,    "LevelUp Roll",       "roll",
                         "roll_deg",  "\xc2\xb0",
                         ",\"icon\":\"mdi:axis-z-rotate-clockwise\","
                         "\"suggested_display_precision\":2",
                         dev);
    mqtt_pub_disc_entity(s_discovery_pitch_topic,   "LevelUp Pitch",      "pitch",
                         "pitch_deg", "\xc2\xb0",
                         ",\"icon\":\"mdi:axis-x-rotate-clockwise\","
                         "\"suggested_display_precision\":2",
                         dev);
    mqtt_pub_disc_entity(s_discovery_roll_in_topic, "LevelUp Roll (in)",  "roll_in",
                         "roll_in",   "in",
                         ",\"icon\":\"mdi:ruler\","
                         "\"suggested_display_precision\":1",
                         dev);
    mqtt_pub_disc_entity(s_discovery_pitch_in_topic,"LevelUp Pitch (in)", "pitch_in",
                         "pitch_in",  "in",
                         ",\"icon\":\"mdi:ruler\","
                         "\"suggested_display_precision\":1",
                         dev);

    // Diagnostic sensors.
    mqtt_pub_disc_entity(s_discovery_rssi_topic,  "LevelUp RSSI",    "rssi",
                         "rssi", "dBm",
                         ",\"device_class\":\"signal_strength\","
                         "\"entity_category\":\"diagnostic\","
                         "\"icon\":\"mdi:wifi\"",
                         dev);
    mqtt_pub_disc_entity(s_discovery_accel_x_topic, "LevelUp Accel X", "accel_x",
                         "accel_x", "g",
                         ",\"entity_category\":\"diagnostic\","
                         "\"icon\":\"mdi:axis-x-arrow\","
                         "\"suggested_display_precision\":3",
                         dev);
    mqtt_pub_disc_entity(s_discovery_accel_y_topic, "LevelUp Accel Y", "accel_y",
                         "accel_y", "g",
                         ",\"entity_category\":\"diagnostic\","
                         "\"icon\":\"mdi:axis-y-arrow\","
                         "\"suggested_display_precision\":3",
                         dev);
    mqtt_pub_disc_entity(s_discovery_accel_z_topic, "LevelUp Accel Z", "accel_z",
                         "accel_z", "g",
                         ",\"entity_category\":\"diagnostic\","
                         "\"icon\":\"mdi:axis-z-arrow\","
                         "\"suggested_display_precision\":3",
                         dev);

    ESP_LOGI(TAG, "HA discovery published (%s)", ipbuf[0] ? ipbuf : "no IP");
}

// Publish state payload at the configured rate.
static void mqtt_mgr_publish_state(void)
{
    if (!s_client) {
        return;
    }
    if (!wifi_mgr_is_connected()) {
        return;
    }

    float roll;
    float pitch;
    float ax, ay, az;
    float trackwidth_in, wheelbase_in;
    portENTER_CRITICAL(&s_angle_mux);
    roll = s_last_roll;
    pitch = s_last_pitch;
    ax = s_last_ax;
    ay = s_last_ay;
    az = s_last_az;
    trackwidth_in = s_trackwidth_in;
    wheelbase_in  = s_wheelbase_in;
    portEXIT_CRITICAL(&s_angle_mux);

    int rssi = 0;
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }

    float roll_in = tanf(roll * DEG2RAD) * trackwidth_in;
    float pitch_in = tanf(pitch * DEG2RAD) * wheelbase_in;

    char ipbuf[16] = "";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip = {0};
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
            snprintf(ipbuf, sizeof(ipbuf), IPSTR, IP2STR(&ip.ip));
        }
    }

    char payload[256];
    int len = snprintf(payload, sizeof(payload),
                       "{"
                       "\"roll_deg\":%.3f,"
                       "\"pitch_deg\":%.3f,"
                       "\"roll_in\":%.1f,"
                       "\"pitch_in\":%.1f,"
                       "\"accel_x\":%.4f,"
                       "\"accel_y\":%.4f,"
                       "\"accel_z\":%.4f,"
                       "\"rssi\":%d,"
                       "\"ip\":\"%s\","
                       "\"mode\":\"STA\""
                       "}",
                       roll, pitch, roll_in, pitch_in, ax, ay, az, rssi, ipbuf);
    if (len > 0) {
        esp_mqtt_client_publish(s_client, s_state_topic, payload, 0, 1, 0);
    }
}

// Periodic publish task.
static void mqtt_mgr_task(void *arg)
{
    (void)arg;
    const TickType_t delay_ticks =
        pdMS_TO_TICKS(1000 / CONFIG_LEVELUP_MQTT_PUBLISH_HZ);

    while (1) {
        s_task_last_alive_us = esp_timer_get_time();
        EventBits_t bits = xEventGroupGetBits(s_mqtt_events);
        if (bits & MQTT_CONNECTED_BIT) {
            mqtt_mgr_publish_state();
        }
        vTaskDelay(delay_ticks);
    }
}

// MQTT event handler: connection state and discovery.
static void mqtt_mgr_handle_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)event_data;

    (void)base;

    if (event_id == MQTT_EVENT_CONNECTED) {
        xEventGroupSetBits(s_mqtt_events, MQTT_CONNECTED_BIT);
        esp_mqtt_client_publish(s_client, s_availability_topic, "online", 0, 1, 1);
        mqtt_mgr_publish_discovery();
        ESP_LOGI(TAG, "MQTT connected");
        return;
    }

    if (event_id == MQTT_EVENT_DISCONNECTED) {
        xEventGroupClearBits(s_mqtt_events, MQTT_CONNECTED_BIT);
        ESP_LOGW(TAG, "MQTT disconnected");
    }
}

// Create and start the MQTT client using current config.
static esp_err_t mqtt_mgr_start_client(void)
{
    if (!s_mqtt_enable) {
        ESP_LOGI(TAG, "MQTT disabled via status page");
        return ESP_OK;
    }

    mqtt_mgr_build_topics();

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_mqtt_uri,
        .credentials.username = s_mqtt_user,
        .credentials.authentication.password = s_mqtt_pass,
        .session.keepalive = 30,
        .session.last_will = {
            .topic = s_availability_topic,
            .msg = "offline",
            .msg_len = 0,
            .qos = 1,
            .retain = 1,
        },
        .network.disable_auto_reconnect = false,
    };
    if (strlen(s_mqtt_user) == 0) {
        cfg.credentials.username = NULL;
    }
    if (strlen(s_mqtt_pass) == 0) {
        cfg.credentials.authentication.password = NULL;
    }

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, mqtt_mgr_handle_event, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));
    return ESP_OK;
}

// Stop and destroy the current client.
static void mqtt_mgr_stop_client(void)
{
    if (!s_client) return;
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    xEventGroupClearBits(s_mqtt_events, MQTT_CONNECTED_BIT);
}

// Initialize MQTT subsystem and start publish task if enabled.
esp_err_t mqtt_mgr_init(void)
{
#if !CONFIG_LEVELUP_MQTT_ENABLE
    ESP_LOGI(TAG, "MQTT disabled via config");
    return ESP_OK;
#else
    s_mqtt_events = xEventGroupCreate();
    if (!s_mqtt_events) {
        return ESP_ERR_NO_MEM;
    }

    mqtt_mgr_load_config();
    if (!s_pub_task) {
        if (xTaskCreate(mqtt_mgr_task, "mqtt_pub", 4096, NULL, 5, &s_pub_task) != pdPASS) {
            ESP_LOGE(TAG, "failed to create mqtt_pub task");
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_RETURN_ON_ERROR(mqtt_mgr_start_client(), TAG, "mqtt start failed");

    ESP_LOGI(TAG, "MQTT init done");
    return ESP_OK;
#endif
}

// Reload config from NVS and restart the client.
esp_err_t mqtt_mgr_restart(void)
{
#if !CONFIG_LEVELUP_MQTT_ENABLE
    ESP_LOGI(TAG, "MQTT disabled via config");
    return ESP_OK;
#else
    if (!s_mqtt_events) {
        s_mqtt_events = xEventGroupCreate();
        if (!s_mqtt_events) return ESP_ERR_NO_MEM;
    }

    if (!s_pub_task) {
        if (xTaskCreate(mqtt_mgr_task, "mqtt_pub", 4096, NULL, 5, &s_pub_task) != pdPASS) {
            ESP_LOGE(TAG, "failed to create mqtt_pub task");
            return ESP_ERR_NO_MEM;
        }
    }

    mqtt_mgr_stop_client();
    mqtt_mgr_load_config();
    return mqtt_mgr_start_client();
#endif
}

// Update vehicle dimensions used for inches conversion.
void mqtt_mgr_set_vehicle_config(float wheelbase_in, float trackwidth_in)
{
    if (wheelbase_in > 0.0f) {
        s_wheelbase_in = wheelbase_in;
    }
    if (trackwidth_in > 0.0f) {
        s_trackwidth_in = trackwidth_in;
    }
}

// Update latest filtered angles for the publish loop.
void mqtt_mgr_update_angles(float roll_deg, float pitch_deg)
{
    portENTER_CRITICAL(&s_angle_mux);
    s_last_roll = roll_deg;
    s_last_pitch = pitch_deg;
    portEXIT_CRITICAL(&s_angle_mux);
}

// Update latest raw accel values for diagnostics.
void mqtt_mgr_update_accel(float ax, float ay, float az)
{
    portENTER_CRITICAL(&s_angle_mux);
    s_last_ax = ax;
    s_last_ay = ay;
    s_last_az = az;
    portEXIT_CRITICAL(&s_angle_mux);
}

bool mqtt_mgr_is_connected(void)
{
    if (!s_mqtt_events) return false;
    EventBits_t bits = xEventGroupGetBits(s_mqtt_events);
    return (bits & MQTT_CONNECTED_BIT) != 0;
}

uint32_t mqtt_mgr_ms_since_alive(void)
{
    int64_t last = s_task_last_alive_us;
    if (last <= 0) return UINT32_MAX;
    int64_t age = esp_timer_get_time() - last;
    if (age < 0) age = 0;
    return (uint32_t)(age / 1000LL);
}
