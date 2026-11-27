#include "orbit_store/Store.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_sntp.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <nvs.h>

#include <ctime>

namespace orbit_store {
namespace {

constexpr const char* kTag = "store-wifi";
constexpr const char* kNetNamespace = "cpfw-net";
constexpr EventBits_t kConnected = BIT0;

EventGroupHandle_t s_events = nullptr;
bool s_inited = false;
bool s_started = false;
bool s_connected = false;
bool s_eventsRegistered = false;
esp_netif_t* s_staNetif = nullptr;

void wifiEvent(void*, esp_event_base_t base, int32_t id, void*) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        // L'AP puo' rifiutare il primo tentativo o essere lento: riprova.
        if (s_started) {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        if (s_events != nullptr) {
            xEventGroupSetBits(s_events, kConnected);
        }
    }
}

bool loadCredentials(std::string& ssid, std::string& password) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNetNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    char ssidBuf[65] = {};
    char passBuf[65] = {};
    size_t ssidLen = sizeof(ssidBuf);
    size_t passLen = sizeof(passBuf);
    const bool ok = nvs_get_str(handle, "ssid", ssidBuf, &ssidLen) == ESP_OK &&
                    nvs_get_str(handle, "pass", passBuf, &passLen) == ESP_OK &&
                    ssidBuf[0] != '\0';
    nvs_close(handle);
    if (ok) {
        ssid = ssidBuf;
        password = passBuf;
    }
    return ok;
}

// Migrazione: se OrbitOS non ha piu' il suo namespace, recupera la rete
// salvata da NucleoOS (namespace nucleocfg, blob JSON "networks").
bool loadNucleoCredentials(std::string& ssid, std::string& password) {
    nvs_handle_t handle = 0;
    if (nvs_open("nucleocfg", NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    char buffer[512] = {};
    size_t len = sizeof(buffer);
    esp_err_t err = nvs_get_blob(handle, "networks", buffer, &len);
    if (err != ESP_OK) {
        len = sizeof(buffer);
        err = nvs_get_str(handle, "networks", buffer, &len);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        return false;
    }
    const std::string json(buffer, len);
    auto extract = [&](const char* key) -> std::string {
        const size_t start = json.find(key);
        if (start == std::string::npos) {
            return std::string();
        }
        const size_t valueStart = start + std::strlen(key);
        const size_t end = json.find('"', valueStart);
        return end == std::string::npos ? std::string()
                                        : json.substr(valueStart, end - valueStart);
    };
    ssid = extract("\"ssid\":\"");
    password = extract("\"pass\":\"");
    return !ssid.empty();
}

void syncClock() {
    setenv("TZ", "UTC0", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    for (int i = 0; i < 150; ++i) {
        if (std::time(nullptr) > 1700000000) {
            ESP_LOGI(kTag, "orologio sincronizzato");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGW(kTag, "NTP non sincronizzato: TLS potrebbe fallire");
}

bool initWifi(std::string* error) {
    if (s_inited) {
        return true;
    }
    if (s_events == nullptr) {
        s_events = xEventGroupCreate();
    }
    if (s_events == nullptr) {
        if (error != nullptr) {
            *error = "Memoria insufficiente.";
        }
        return false;
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        if (error != nullptr) {
            *error = esp_err_to_name(err);
        }
        return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        if (error != nullptr) {
            *error = esp_err_to_name(err);
        }
        return false;
    }
    if (s_staNetif == nullptr) {
        s_staNetif = esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        if (error != nullptr) {
            *error = esp_err_to_name(err);
        }
        return false;
    }
    if (!s_eventsRegistered) {
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEvent,
                                            nullptr, nullptr);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEvent,
                                            nullptr, nullptr);
        s_eventsRegistered = true;
    }
    s_inited = true;
    s_started = false;
    s_connected = false;
    return true;
}

}  // namespace

bool wifiEnsureDriver() {
    std::string error;
    return initWifi(&error);
}

void wifiShutdownDriver() {
    if (s_started) {
        esp_wifi_stop();
        s_started = false;
    }
    if (s_inited) {
        esp_wifi_deinit();
        s_inited = false;
    }
    s_connected = false;
}

bool currentSsid(std::string& ssid) {
    std::string password;
    if (loadCredentials(ssid, password)) {
        return true;
    }
    return loadNucleoCredentials(ssid, password);
}

bool saveWifi(const std::string& ssid, const std::string& password) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNetNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    const bool ok = nvs_set_str(handle, "ssid", ssid.c_str()) == ESP_OK &&
                    nvs_set_str(handle, "pass", password.c_str()) == ESP_OK &&
                    nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    s_connected = false;
    return ok;
}

bool ensureWifi(std::string* error) {    if (s_connected) {
        return true;
    }
    if (!s_inited && !initWifi(error)) {
        return false;
    }

    // Prima il namespace di OrbitOS 2.x; se assente (per esempio dopo una
    // installazione di NucleoOS) usa la config STA che il driver WiFi ha in NVS.
    std::string ssid;
    std::string password;
    if (!loadCredentials(ssid, password)) {
        if (loadNucleoCredentials(ssid, password)) {
            ESP_LOGI(kTag, "uso WiFi da NucleoOS: %s", ssid.c_str());
        } else {
            wifi_config_t stored = {};
            if (esp_wifi_get_config(WIFI_IF_STA, &stored) == ESP_OK &&
                stored.sta.ssid[0] != '\0') {
                ssid = reinterpret_cast<const char*>(stored.sta.ssid);
                password = reinterpret_cast<const char*>(stored.sta.password);
                ESP_LOGI(kTag, "uso WiFi salvato dal driver: %s", ssid.c_str());
            } else {
                if (error != nullptr) {
                    *error = "Credenziali WiFi assenti.";
                }
                return false;
            }
        }
    }

    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char*>(config.sta.ssid), ssid.c_str(),
                 sizeof(config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(config.sta.password), password.c_str(),
                 sizeof(config.sta.password) - 1);
    if (s_started) {
        esp_wifi_disconnect();
    }
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &config);
    }
    if (err != ESP_OK) {
        if (error != nullptr) {
            *error = esp_err_to_name(err);
        }
        return false;
    }

    xEventGroupClearBits(s_events, kConnected);
    if (!s_started) {
        err = esp_wifi_start();
        if (err != ESP_OK) {
            if (error != nullptr) {
                *error = esp_err_to_name(err);
            }
            return false;
        }
        s_started = true;
    } else {
        esp_wifi_connect();
    }

    // Alcuni AP impiegano ~40 s (DHCP compreso): 30 s falliva appena prima
    // dell'IP e lo Store sembrava rotto.
    const EventBits_t bits = xEventGroupWaitBits(
        s_events, kConnected, pdFALSE, pdFALSE, pdMS_TO_TICKS(60000));
    if ((bits & kConnected) == 0) {
        if (error != nullptr) {
            *error = "WiFi non connesso.";
        }
        return false;
    }

    ESP_LOGI(kTag, "WiFi connesso: %s", ssid.c_str());
    syncClock();
    return true;
}

}  // namespace orbit_store
