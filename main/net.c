#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mdns.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "net.h"
#include "settings.h"
#include "setup_screen.h"

/*
 * The radio is in the chip, or in a chip beside it that esp_hosted talks to.
 * Wi-Fi is the same calls either way. The Bluetooth controller is not: the
 * one in the chip is started by the stack and gives its memory back when it
 * is not needed, the one beside is started here and has no memory of this
 * chip to give back.
 */
#if CONFIG_ESP_HOSTED_HOST_FEAT_BT

#include "esp_hosted_bt_host_stack.h"

static esp_err_t bluetooth_controller_start(void)
{
    esp_hosted_bt_host_stack_cfg_t config = ESP_HOSTED_BT_HOST_STACK_CONFIG_DEFAULT();
    return esp_hosted_bt_host_stack_setup(&config);
}

static void bluetooth_controller_release(void)
{
}

#elif CONFIG_BT_CONTROLLER_ENABLED

#include "esp_bt.h"

static esp_err_t bluetooth_controller_start(void)
{
    return ESP_OK;
}

static void bluetooth_controller_release(void)
{
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
}

#else
#error "The setup of Wi-Fi goes over BLE: a Bluetooth controller is needed, in the chip or through esp_hosted"
#endif

static const char *TAG = "net";

/* The QR code carries both, so the proof of possession is no secret: it only pairs the phone with this device. */
#define PROV_POP       "morphing"
#define PROV_PREFIX    "CLOCK-"
#define PROV_TRANSPORT "ble"

/* A lost connection is tried again after so long. */
#define RECONNECT_MS 2000
/*
 * A network may have several access points under one name. The strongest
 * one heard is joined, but a look at the channels now and then misses the
 * near one, and the station does not move to it later by itself. So a
 * connection to an access point heard worse than this is given up and made
 * anew, so many times; then any will do.
 */
#define GOOD_RSSI  (-65)
#define WEAK_TRIES 2

enum {
    EVENT_ACCEPTED = BIT0, /* connected with what the phone gave */
    EVENT_RECEIVED = BIT1,
    EVENT_REFUSED = BIT2,
};

static esp_netif_t       *netif;
static EventGroupHandle_t events;
static bool               connected;
static esp_timer_handle_t reconnect_timer;
static int                weak_tries; /* connections given up in a row for a weak signal */
static volatile bool       time_synced;

/* The strongest access point of the network, which takes a look at all the channels, not the first one heard. */
static void choose_strongest(void)
{
    wifi_config_t config;
    if (esp_wifi_get_config(WIFI_IF_STA, &config) != ESP_OK) return;
    if (config.sta.scan_method == WIFI_ALL_CHANNEL_SCAN && config.sta.sort_method == WIFI_CONNECT_AP_BY_SIGNAL && !config.sta.bssid_set) return;
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.bssid_set = false;
    esp_wifi_set_config(WIFI_IF_STA, &config);
}

/*
 * The network kept is safe while the setup only waits: the manager empties
 * the copy in the memory, not the one in NVS. But what the phone sends goes
 * to NVS at once, before it is tried, and a wrong password leaves the device
 * with no network at all. So the network is put aside before the setup and
 * taken back unless another one has been accepted: when the setup ends, or
 * at the next start if the power has gone in between.
 */
#define KEEP_NAMESPACE "net"
#define KEEP_KEY       "wifi_kept"

static void keep_network(void)
{
    wifi_config_t config;
    nvs_handle_t  handle;
    if (esp_wifi_get_config(WIFI_IF_STA, &config) != ESP_OK || !config.sta.ssid[0]) return;
    if (nvs_open(KEEP_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    esp_err_t ret = nvs_set_blob(handle, KEEP_KEY, &config.sta, sizeof(config.sta));
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    if (ret != ESP_OK) ESP_LOGW(TAG, "The network is not put aside: %s", esp_err_to_name(ret));
}

/* restore: the network put aside is the network again; otherwise it is only forgotten */
static void end_keeping(bool restore)
{
    wifi_config_t config = {0};
    size_t        size = sizeof(config.sta);
    nvs_handle_t  handle;
    if (nvs_open(KEEP_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_get_blob(handle, KEEP_KEY, &config.sta, &size) == ESP_OK) {
        if (restore && size == sizeof(config.sta)) {
            esp_wifi_set_storage(WIFI_STORAGE_FLASH);
            esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &config);
            ESP_LOGW(TAG, "No other network is accepted, \"%s\" is taken back: %s", (const char *)config.sta.ssid, esp_err_to_name(ret));
        }
        nvs_erase_key(handle, KEEP_KEY);
        nvs_commit(handle);
    }
    nvs_close(handle);
}

static void on_time_synced(struct timeval *tv)
{
    struct tm tm;
    char      text[32];
    localtime_r(&tv->tv_sec, &tm);
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &tm);
    ESP_LOGI(TAG, "Time from the network: %s", text);
    time_synced = true;
}

static void reconnect(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void station_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = data;
        /* a connection given up here is no news */
        if (event->reason != WIFI_REASON_ASSOC_LEAVE) {
            ESP_LOGW(TAG, "%s, reason %d; again in %d s", connected ? "Wi-Fi is lost" : "No connection", event->reason, RECONNECT_MS / 1000);
        }
        connected = false;
        esp_timer_stop(reconnect_timer);
        esp_timer_start_once(reconnect_timer, RECONNECT_MS * 1000LL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        wifi_ap_record_t   ap = {0};
        esp_wifi_sta_get_ap_info(&ap);
        if (ap.rssi < GOOD_RSSI && weak_tries < WEAK_TRIES) {
            ++weak_tries;
            ESP_LOGW(TAG, MACSTR " is heard at %d dBm: looking for a better access point", MAC2STR(ap.bssid), ap.rssi);
            esp_wifi_disconnect();
            return;
        }
        weak_tries = 0;
        ESP_LOGI(TAG, "Connected to " MACSTR " at %d dBm, address " IPSTR, MAC2STR(ap.bssid), ap.rssi, IP2STR(&event->ip_info.ip));
        connected = true;
        /* asked now: a request sent before there is a network is repeated only much later */
        esp_netif_sntp_start();
    }
}

static void provisioning_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case NETWORK_PROV_WIFI_CRED_RECV: {
        wifi_sta_config_t *config = data;
        ESP_LOGI(TAG, "Got the password of \"%s\"", (const char *)config->ssid);
        xEventGroupSetBits(events, EVENT_RECEIVED);
        break;
    }
    case NETWORK_PROV_WIFI_CRED_FAIL: {
        network_prov_wifi_sta_fail_reason_t *reason = data;
        ESP_LOGE(TAG, "No connection: %s", *reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR ? "wrong password" : "no such network");
        /* the phone may give another one */
        network_prov_mgr_reset_wifi_sm_state_on_failure();
        xEventGroupSetBits(events, EVENT_REFUSED);
        break;
    }
    case NETWORK_PROV_WIFI_CRED_SUCCESS:
        end_keeping(false);
        xEventGroupSetBits(events, EVENT_ACCEPTED);
        break;
    default:
        break;
    }
}

esp_err_t net_init(void)
{
    if (netif) return ESP_OK;
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) return ret;
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;
    events = xEventGroupCreate();
    if (!events) return ESP_ERR_NO_MEM;
    netif = esp_netif_create_default_wifi_sta();
    if (!netif) return ESP_FAIL;
    /* named before any way of starting Wi-Fi: the router keeps the name it has heard first, and without one that is "espressif" */
    settings_t settings;
    settings_get(&settings);
    esp_netif_set_hostname(netif, settings.host_name);
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&config);
    if (ret != ESP_OK) return ret;
    /* a setup that the power has cut short */
    esp_wifi_set_mode(WIFI_MODE_STA);
    end_keeping(true);
    return ESP_OK;
}

bool net_is_provisioned(void)
{
    wifi_config_t config;
    return esp_wifi_get_config(WIFI_IF_STA, &config) == ESP_OK && config.sta.ssid[0];
}

bool net_is_connected(void)
{
    return connected;
}

bool net_has_time(void)
{
    return time_synced;
}

bool net_get_rssi(int *rssi)
{
    wifi_ap_record_t ap;
    if (!connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    *rssi = ap.rssi;
    return true;
}

bool net_get_address(char *text, size_t size)
{
    esp_netif_ip_info_t info;
    if (!connected || esp_netif_get_ip_info(netif, &info) != ESP_OK || !info.ip.addr) return false;
    snprintf(text, size, IPSTR, IP2STR(&info.ip));
    return true;
}

esp_err_t net_start(void)
{
    bluetooth_controller_release();

    const esp_timer_create_args_t timer = {.callback = reconnect, .name = "reconnect"};
    esp_err_t                     ret = esp_timer_create(&timer, &reconnect_timer);
    if (ret != ESP_OK) return ret;
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, station_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, station_handler, NULL);

    /* the same name answers in the network as <name>.local */
    settings_t settings;
    settings_get(&settings);
    ret = mdns_init();
    if (ret == ESP_OK) ret = mdns_hostname_set(settings.host_name);
    if (ret != ESP_OK) ESP_LOGW(TAG, "No name in the network: %s", esp_err_to_name(ret));

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp.sync_cb = on_time_synced;
    sntp.start = false;
    ret = esp_netif_sntp_init(&sntp);
    if (ret != ESP_OK) ESP_LOGW(TAG, "SNTP does not start: %s", esp_err_to_name(ret));

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) return ret;
    choose_strongest();
    ret = esp_wifi_start();
    if (ret != ESP_OK) return ret;
    /*
     * The clock is fed from the mains, and a sleeping radio answers late:
     * every twelfth connection to the page waited 0.4–1 s for a beacon.
     * Bluetooth is off by now; with it on the radio must sleep.
     */
    esp_wifi_set_ps(WIFI_PS_NONE);
    return ESP_OK;
}

void net_forget(void)
{
    wifi_config_t empty = {0};
    esp_wifi_disconnect();
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &empty);
    if (ret != ESP_OK) ESP_LOGE(TAG, "The network is not forgotten: %s", esp_err_to_name(ret));
}

bool net_provision(dgx_screen_t *screen, uint32_t timeout_ms)
{
    esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, provisioning_handler, NULL);
    network_prov_mgr_config_t config = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
    };
    keep_network();
    esp_err_t ret = bluetooth_controller_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "The Bluetooth controller does not start: %s", esp_err_to_name(ret));
        end_keeping(false);
        return false;
    }
    if (network_prov_mgr_init(config) != ESP_OK) {
        end_keeping(false);
        return false;
    }
    /* the phone is told the outcome, then the service is stopped here */
    network_prov_mgr_disable_auto_stop(1000);

    char    name[sizeof(PROV_PREFIX "XXXXXX")];
    uint8_t mac[6];
    settings_device_mac(mac);
    snprintf(name, sizeof(name), PROV_PREFIX "%02X%02X%02X", mac[3], mac[4], mac[5]);
    if (network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1, PROV_POP, name, NULL) != ESP_OK) {
        network_prov_mgr_deinit();
        end_keeping(false);
        return false;
    }
    ESP_LOGI(TAG, "Wi-Fi setup over BLE as %s", name);

    char payload[128];
    snprintf(payload, sizeof(payload), "{\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"%s\"}", name, PROV_POP,
             PROV_TRANSPORT);
    setup_screen_draw(screen, "Wi-Fi: ESP BLE Provisioning", payload, name);

    bool    accepted = false;
    int64_t deadline_us = esp_timer_get_time() + timeout_ms * 1000LL;
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (timeout_ms) {
            int64_t left_us = deadline_us - esp_timer_get_time();
            if (left_us <= 0) break;
            wait = pdMS_TO_TICKS(left_us / 1000);
        }
        EventBits_t bits = xEventGroupWaitBits(events, EVENT_ACCEPTED | EVENT_RECEIVED | EVENT_REFUSED, pdTRUE, pdFALSE, wait);
        if (bits & EVENT_ACCEPTED) {
            setup_screen_state(screen, "connected");
            accepted = true;
            break;
        }
        if (bits & EVENT_REFUSED) {
            setup_screen_state(screen, "no connection, try again");
            /* the time starts again: somebody is at it */
            deadline_us = esp_timer_get_time() + timeout_ms * 1000LL;
        } else if (bits & EVENT_RECEIVED) {
            setup_screen_state(screen, "connecting…");
        }
    }
    if (accepted) vTaskDelay(pdMS_TO_TICKS(3000));
    network_prov_mgr_deinit();
    if (!accepted) end_keeping(true);
    return accepted;
}
