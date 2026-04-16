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
static bool s_task_started = false;

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
}

// Publish Home Assistant discovery payloads (retained).
static void mqtt_mgr_publish_discovery(void)
{
    if (!s_client) {
        return;
    }
    char payload[512];

    int len = snprintf(payload, sizeof(payload),
                       "{"
                       "\"name\":\"LevelUp Roll\","
                       "\"uniq_id\":\"%s_roll\","
                       "\"state_topic\":\"%s\","
                       "\"value_template\":\"{{ value_json.roll_deg }}\","
                       "\"unit_of_measurement\":\"°\","
                       "\"availability_topic\":\"%s\","
                       "\"payload_available\":\"online\","
                       "\"payload_not_available\":\"offline\","
                       "\"device\":{"
                       "\"identifiers\":[\"%s\"],"
                       "\"name\":\"LevelUp\","
                       "\"manufacturer\":\"LevelUp\","
                       "\"model\":\"ESP32\""
                       "}"
                       "}",
                       s_device_id, s_state_topic, s_availability_topic, s_device_id);
    if (len > 0) {
        esp_mqtt_client_publish(s_client, s_discovery_roll_topic, payload, 0, 1, 1);
    }

    len = snprintf(payload, sizeof(payload),
                   "{"
                   "\"name\":\"LevelUp Pitch\","
                   "\"uniq_id\":\"%s_pitch\","
                   "\"state_topic\":\"%s\","
                   "\"value_template\":\"{{ value_json.pitch_deg }}\","
                   "\"unit_of_measurement\":\"°\","
                   "\"availability_topic\":\"%s\","
                   "\"payload_available\":\"online\","
                   "\"payload_not_available\":\"offline\","
                   "\"device\":{"
                   "\"identifiers\":[\"%s\"],"
                   "\"name\":\"LevelUp\","
                   "\"manufacturer\":\"LevelUp\","
                   "\"model\":\"ESP32\""
                   "}"
                   "}",
                   s_device_id, s_state_topic, s_availability_topic, s_device_id);
    if (len > 0) {
        esp_mqtt_client_publish(s_client, s_discovery_pitch_topic, payload, 0, 1, 1);
    }

    len = snprintf(payload, sizeof(payload),
                   "{"
                   "\"name\":\"LevelUp RSSI\","
                   "\"uniq_id\":\"%s_rssi\","
                   "\"state_topic\":\"%s\","
                   "\"value_template\":\"{{ value_json.rssi }}\","
                   "\"unit_of_measurement\":\"dBm\","
                   "\"availability_topic\":\"%s\","
                   "\"payload_available\":\"online\","
                   "\"payload_not_available\":\"offline\","
                   "\"device\":{"
                   "\"identifiers\":[\"%s\"],"
                   "\"name\":\"LevelUp\","
                   "\"manufacturer\":\"LevelUp\","
                   "\"model\":\"ESP32\""
                   "}"
                   "}",
                   s_device_id, s_state_topic, s_availability_topic, s_device_id);
    if (len > 0) {
        esp_mqtt_client_publish(s_client, s_discovery_rssi_topic, payload, 0, 1, 1);
    }

    len = snprintf(payload, sizeof(payload),
                   "{"
                   "\"name\":\"LevelUp Roll (in)\","
                   "\"uniq_id\":\"%s_roll_in\","
                   "\"state_topic\":\"%s\","
                   "\"value_template\":\"{{ value_json.roll_in }}\","
                   "\"unit_of_measurement\":\"in\","
                   "\"availability_topic\":\"%s\","
                   "\"payload_available\":\"online\","
                   "\"payload_not_available\":\"offline\","
                   "\"device\":{"
                   "\"identifiers\":[\"%s\"],"
                   "\"name\":\"LevelUp\","
                   "\"manufacturer\":\"LevelUp\","
                   "\"model\":\"ESP32\""
                   "}"
                   "}",
                   s_device_id, s_state_topic, s_availability_topic, s_device_id);
    if (len > 0) {
        esp_mqtt_client_publish(s_client, s_discovery_roll_in_topic, payload, 0, 1, 1);
    }

    len = snprintf(payload, sizeof(payload),
                   "{"
                   "\"name\":\"LevelUp Pitch (in)\","
                   "\"uniq_id\":\"%s_pitch_in\","
                   "\"state_topic\":\"%s\","
                   "\"value_template\":\"{{ value_json.pitch_in }}\","
                   "\"unit_of_measurement\":\"in\","
                   "\"availability_topic\":\"%s\","
                   "\"payload_available\":\"online\","
                   "\"payload_not_available\":\"offline\","
                   "\"device\":{"
                   "\"identifiers\":[\"%s\"],"
                   "\"name\":\"LevelUp\","
                   "\"manufacturer\":\"LevelUp\","
                   "\"model\":\"ESP32\""
                   "}"
                   "}",
                   s_device_id, s_state_topic, s_availability_topic, s_device_id);
    if (len > 0) {
        esp_mqtt_client_publish(s_client, s_discovery_pitch_in_topic, payload, 0, 1, 1);
    }
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
    portENTER_CRITICAL(&s_angle_mux);
    roll = s_last_roll;
    pitch = s_last_pitch;
    ax = s_last_ax;
    ay = s_last_ay;
    az = s_last_az;
    portEXIT_CRITICAL(&s_angle_mux);

    int rssi = 0;
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }

    float roll_in = tanf(roll * DEG2RAD) * s_trackwidth_in;
    float pitch_in = tanf(pitch * DEG2RAD) * s_wheelbase_in;

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
        esp_mqtt_client_publish(s_client, s_state_topic, payload, 0, 0, 0);
    }
}

// Periodic publish task.
static void mqtt_mgr_task(void *arg)
{
    (void)arg;
    const TickType_t delay_ticks =
        pdMS_TO_TICKS(1000 / CONFIG_LEVELUP_MQTT_PUBLISH_HZ);

    while (1) {
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
    if (!s_task_started) {
        if (xTaskCreate(mqtt_mgr_task, "mqtt_pub", 4096, NULL, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "failed to create mqtt_pub task");
            return ESP_ERR_NO_MEM;
        }
        s_task_started = true;
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

    if (!s_task_started) {
        if (xTaskCreate(mqtt_mgr_task, "mqtt_pub", 4096, NULL, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "failed to create mqtt_pub task");
            return ESP_ERR_NO_MEM;
        }
        s_task_started = true;
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
