// OrbitOS 3.0 — entry point ESP-IDF.
// M1: Home portata dal firmware 2.x, tre voci (Store, Giochi, Impostazioni).

#include <cstdio>
#include <cerrno>

#include "Display.h"
#include "Home.h"
#include "Navigation.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "orbit_debug/Console.h"
#include "orbit_hal/display.h"
#include "orbit_mascot/Mascot.h"
#include "orbit_settings/Settings.h"

#if defined(ORBIT_DIAG_CHAT)
#include "orbit_chat/Chat.h"
#endif

#ifdef ORBIT_DIAG_STORE
#include "orbit_store/Store.h"
#include "orbit_wren/GameRuntime.h"
#endif

static const char *TAG = "orbit";

#ifdef ORBIT_DIAG_SELFTEST
void orbitSelfTest();
#endif

#ifdef ORBIT_DIAG_STORE
static void diagStore() {
    ESP_LOGI(TAG, "DIAG store start");
    if (!orbit_store::mountLittleFs()) {
        ESP_LOGE(TAG, "DIAG sd FAILED");
        return;
    }
    const std::vector<std::string> files = orbit_store::installedGames();
    ESP_LOGI(TAG, "DIAG files=%u", static_cast<unsigned>(files.size()));
    for (const std::string& file : files) {
        ESP_LOGI(TAG, "DIAG file %s", file.c_str());
    }
    const std::string probePath = std::string(orbit_store::kGamesDir) + "/2048.wren";
    FILE* probe = std::fopen(probePath.c_str(), "rb");
    ESP_LOGI(TAG, "DIAG fopen=%d errno=%d", probe != nullptr ? 1 : 0, errno);
    if (probe != nullptr) {
        std::fclose(probe);
    }
    std::string error;
    if (!orbit_store::ensureWifi(&error)) {
        ESP_LOGE(TAG, "DIAG wifi FAILED: %s", error.c_str());
        ESP_LOGI(TAG, "DIAG local game");
        const bool localOk = orbit_wren::runGame(std::string(orbit_store::kGamesDir) + "/2048.wren");
        ESP_LOGI(TAG, "DIAG local end ok=%d", localOk ? 1 : 0);
        return;
    }
    std::vector<orbit_store::GameInfo> catalog;
    if (!orbit_store::fetchCatalog(catalog, &error)) {
        ESP_LOGE(TAG, "DIAG catalog FAILED: %s", error.c_str());
        return;
    }
    ESP_LOGI(TAG, "DIAG catalog=%u", static_cast<unsigned>(catalog.size()));
    for (const auto& game : catalog) {
        ESP_LOGI(TAG, "DIAG game %s size=%u installed=%d", game.id.c_str(),
                 static_cast<unsigned>(game.size),
                 orbit_store::gameInstalled(game) ? 1 : 0);
    }
    if (catalog.empty()) {
        return;
    }
    const orbit_store::GameInfo& game = catalog.front();
    // Forza il download anche se il file e' gia' presente: testa HTTP, SHA-256
    // e scrittura atomica su SD.
    if (!orbit_store::downloadGame(game, &error)) {
        ESP_LOGE(TAG, "DIAG download FAILED: %s", error.c_str());
        return;
    }
    ESP_LOGI(TAG, "DIAG play %s", game.id.c_str());
    const bool ok = orbit_wren::runGame(orbit_store::gamePath(game.id));
    ESP_LOGI(TAG, "DIAG game end ok=%d", ok ? 1 : 0);
}
#endif

extern "C" void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // La NVS attuale e' quella di OrbitOS 2.x: non va cancellata.
        ESP_LOGW(TAG, "NVS non inizializzabile (%s): riapro senza erase",
                 esp_err_to_name(err));
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "OrbitOS 3.0 IDF boot");
    navigation::begin();
    orbit_settings::begin();
    mascot::begin();

    const uint8_t savedRotation = orbit_settings::rotation();
    if (!display::begin(savedRotation)) {
        ESP_LOGE(TAG, "framebuffer non disponibile: disegno diretto");
    }
    if (display::rotation() != savedRotation) {
        display::applyRotation(savedRotation);
        if (display::rotation() != savedRotation) {
            orbit_settings::setRotation(display::rotation());
        }
    }
    display::setTheme(orbit_settings::theme());
    orbit_hal::displayBrightness(orbit_settings::brightness());
    display::splash();
    orbit_debug::begin();
#ifdef ORBIT_DIAG_SELFTEST
    orbitSelfTest();
#endif

    ESP_LOGI(TAG, "Home");
#if defined(ORBIT_DIAG_CHAT)
    orbit_chat::runMenu();
#elif defined(ORBIT_DIAG_STORE)
    diagStore();
#else
    orbit_app::runHome();
#endif
}
