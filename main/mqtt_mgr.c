#include "mqtt_mgr.h"

#include <string.h>
#include <stdio.h>

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
#include "mqtt_client.h"

#include "wifi_mgr.h"
#include "lwip/inet.h"

static const char *TAG = "mqtt_mgr";

#define MQTT_CONNECTED_BIT BIT0

static EventGroupHandle_t s_mqtt_events;
static esp_mqtt_client_handle_t s_client;

static portMUX_TYPE s_angle_mux = portMUX_INITIALIZER_UNLOCKED;
static float s_last_roll = 0.0f;
static float s_last_pitch = 0.0f;

static char s_device_id[32];
static char s_state_topic[128];
static char s_availability_topic[128];
static char s_discovery_roll_topic[128];
static char s_discovery_pitch_topic[128];
static char s_discovery_rssi_topic[128];

static void mqtt_mgr_build_topics(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id),
             "levelup_%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    snprintf(s_state_topic, sizeof(s_state_topic),
             "%s/%s/state", CONFIG_LEVELUP_MQTT_TOPIC_PREFIX, s_device_id);
    snprintf(s_availability_topic, sizeof(s_availability_topic),
             "%s/%s/availability", CONFIG_LEVELUP_MQTT_TOPIC_PREFIX, s_device_id);

    snprintf(s_discovery_roll_topic, sizeof(s_discovery_roll_topic),
             "%s/sensor/%s/roll/config", CONFIG_LEVELUP_MQTT_DISCOVERY_PREFIX, s_device_id);
    snprintf(s_discovery_pitch_topic, sizeof(s_discovery_pitch_topic),
             "%s/sensor/%s/pitch/config", CONFIG_LEVELUP_MQTT_DISCOVERY_PREFIX, s_device_id);
    snprintf(s_discovery_rssi_topic, sizeof(s_discovery_rssi_topic),
             "%s/sensor/%s/rssi/config", CONFIG_LEVELUP_MQTT_DISCOVERY_PREFIX, s_device_id);
}

static void mqtt_mgr_publish_discovery(void)
{
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
}

static void mqtt_mgr_publish_state(void)
{
    if (!wifi_mgr_is_connected()) {
        return;
    }

    float roll;
    float pitch;
    portENTER_CRITICAL(&s_angle_mux);
    roll = s_last_roll;
    pitch = s_last_pitch;
    portEXIT_CRITICAL(&s_angle_mux);

    int rssi = 0;
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }

    char ipbuf[16] = "";
    const esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
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
                       "\"rssi\":%d,"
                       "\"ip\":\"%s\","
                       "\"mode\":\"STA\""
                       "}",
                       roll, pitch, rssi, ipbuf);
    if (len > 0) {
        esp_mqtt_client_publish(s_client, s_state_topic, payload, 0, 0, 0);
    }
}

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

static void mqtt_mgr_handle_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)event_data;

    if (base != MQTT_EVENTS) {
        return;
    }

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

    mqtt_mgr_build_topics();

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_LEVELUP_MQTT_BROKER_URI,
        .credentials.username = CONFIG_LEVELUP_MQTT_USERNAME,
        .credentials.authentication.password = CONFIG_LEVELUP_MQTT_PASSWORD,
        .session.keepalive = 30,
        .network.disable_auto_reconnect = false,
        .lwt_topic = s_availability_topic,
        .lwt_msg = "offline",
        .lwt_retain = true,
        .lwt_qos = 1,
    };
    if (strlen(CONFIG_LEVELUP_MQTT_USERNAME) == 0) {
        cfg.credentials.username = NULL;
    }
    if (strlen(CONFIG_LEVELUP_MQTT_PASSWORD) == 0) {
        cfg.credentials.authentication.password = NULL;
    }

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, mqtt_mgr_handle_event, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));

    xTaskCreate(mqtt_mgr_task, "mqtt_pub", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "MQTT init done");
    return ESP_OK;
#endif
}

void mqtt_mgr_update_angles(float roll_deg, float pitch_deg)
{
    portENTER_CRITICAL(&s_angle_mux);
    s_last_roll = roll_deg;
    s_last_pitch = pitch_deg;
    portEXIT_CRITICAL(&s_angle_mux);
}
