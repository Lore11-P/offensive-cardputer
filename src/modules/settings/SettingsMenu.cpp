#include "SettingsMenu.h"

#include <Arduino.h>
#include <esp_mac.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "Config.h"
#include "Display.h"
#include "MenuManager.h"
#include "Navigation.h"
#include "OtaUpdate.h"
#include "Storage.h"
#include "Ui.h"
#include "WifiSetup.h"

#if ENABLE_MODULE_SETTINGS

namespace settings_module {
namespace {

void applyDisplay() {
    M5Cardputer.Display.setBrightness(config::brightness());
    display::applyRotation(config::rotation());
}

int brightnessPercent() {
    return (static_cast<int>(config::brightness()) * 100 + 127) / 255;
}

void runBrightness() {
    navigation::waitRelease();
    uint32_t nextRepeat = 0;
    bool leftHeld = false;
    bool rightHeld = false;

    for (;;) {
        display::clear();
        display::header("Luminosita", "Impostazioni");
        display::textCentered(35, std::to_string(brightnessPercent()) + "%", display::kAccent, 4);
        display::progressBar(28, 66, display::width() - 56, 10,
                             static_cast<float>(config::brightness()) / 255.0f);
        display::textCentered(84, "valore " + std::to_string(config::brightness()) + " / 255",
                              display::kTextDim, 0);
        display::textCentered(102, ", = -10%    / = +10%", display::kText, 0);
        display::footer("ENTER/` = salva e torna");
        display::push();

        bool changed = false;
        const int step = 26;
        if (navigation::pressed(',') && millis() >= nextRepeat) {
            const int v = static_cast<int>(config::brightness()) - step;
            config::setBrightness(static_cast<uint8_t>(v < 10 ? 10 : v));
            M5Cardputer.Display.setBrightness(config::brightness());
            nextRepeat = millis() + (leftHeld ? 90 : 320);
            leftHeld = true;
            changed = true;
        } else if (!navigation::pressed(',')) {
            leftHeld = false;
        }
        if (navigation::pressed('/') && millis() >= nextRepeat) {
            const int v = static_cast<int>(config::brightness()) + step;
            config::setBrightness(static_cast<uint8_t>(v > 255 ? 255 : v));
            M5Cardputer.Display.setBrightness(config::brightness());
            nextRepeat = millis() + (rightHeld ? 90 : 320);
            rightHeld = true;
            changed = true;
        } else if (!navigation::pressed('/')) {
            rightHeld = false;
        }

        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back || ev.type == navigation::Event::Select) {
            break;
        }
        delay(changed ? 25 : 30);
    }
    navigation::waitRelease();
}

const char* rotationLabel(uint8_t index) {
    switch (index & 3) {
        case 0:
            return "0 gradi (portrait)";
        case 1:
            return "90 gradi (landscape)";
        case 2:
            return "180 gradi (portrait)";
        case 3:
            return "270 gradi (landscape)";
        default:
            return "?";
    }
}

void runRotation() {
    navigation::waitRelease();
    uint32_t nextRepeat = 0;
    for (;;) {
        display::clear();
        display::header("Rotazione", "Impostazioni");
        display::textCentered(36, rotationLabel(config::rotation()), display::kAccent, 2);
        display::textCentered(60, "indice display: " + std::to_string(config::rotation()),
                              display::kTextDim, 0);
        display::textCentered(88, ", = precedente    / = successiva", display::kText, 0);
        display::footer("ENTER/` = salva e torna");
        display::push();

        bool changed = false;
        if (navigation::pressed(',') && millis() >= nextRepeat) {
            config::setRotation(static_cast<uint8_t>((config::rotation() + 3) & 3));
            applyDisplay();
            nextRepeat = millis() + 250;
            changed = true;
        }
        if (navigation::pressed('/') && millis() >= nextRepeat) {
            config::setRotation(static_cast<uint8_t>((config::rotation() + 1) & 3));
            applyDisplay();
            nextRepeat = millis() + 250;
            changed = true;
        }

        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back || ev.type == navigation::Event::Select) {
            break;
        }
        delay(changed ? 40 : 30);
    }
    navigation::waitRelease();
}

const char* sleepLabel(uint16_t seconds) {
    switch (seconds) {
        case 0:
            return "disattivato";
        case 30:
            return "30 secondi";
        case 60:
            return "1 minuto";
        case 120:
            return "2 minuti";
        case 300:
            return "5 minuti";
        case 600:
            return "10 minuti";
        case 900:
            return "15 minuti";
        case 1800:
            return "30 minuti";
        default:
            return "?";
    }
}

void runSleepTimer() {
    static const uint16_t kOptions[] = {0, 30, 60, 120, 300, 600, 900, 1800};
    constexpr int kCount = sizeof(kOptions) / sizeof(kOptions[0]);

    navigation::waitRelease();
    for (;;) {
        const uint16_t current = config::sleepTimeoutSec();
        display::clear();
        display::header("Spegnimento auto", "Impostazioni");
        display::textCentered(32, sleepLabel(current), display::kAccent, 2);
        display::textCentered(56, "Spegne la Cardputer dopo", display::kTextDim, 0);
        display::textCentered(68, "questo tempo senza tasti.", display::kTextDim, 0);
        display::textCentered(88, "Un tasto durante il preavviso", display::kMuted, 0);
        display::textCentered(98, "annulla lo spegnimento.", display::kMuted, 0);
        display::textCentered(110, ", / = cambia", display::kText, 0);
        display::footer("ENTER/` = salva e torna");
        display::push();

        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back || ev.type == navigation::Event::Select) {
            break;
        }
        if (ev.type == navigation::Event::Left || ev.type == navigation::Event::Right) {
            int index = 0;
            for (int i = 0; i < kCount; ++i) {
                if (kOptions[i] == current) {
                    index = i;
                    break;
                }
            }
            index += (ev.type == navigation::Event::Right) ? 1 : -1;
            if (index < 0) {
                index = kCount - 1;
            } else if (index >= kCount) {
                index = 0;
            }
            config::setSleepTimeoutSec(kOptions[index]);
        }
        delay(25);
    }
    navigation::waitRelease();
}

std::string macString() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
                  mac[4], mac[5]);
    return std::string(buf);
}

void showInfo() {
    navigation::waitRelease();
    uint32_t lastRefresh = 0;
    for (;;) {
        if (millis() - lastRefresh > 500) {
            lastRefresh = millis();
            M5Cardputer.update();
            const int level = static_cast<int>(M5Cardputer.Power.getBatteryLevel());
            const bool charging =
                M5Cardputer.Power.isCharging() == m5::Power_Class::is_charging;

            std::vector<std::string> lines;
            lines.push_back(std::string(">") + fw::kTitle);
            lines.push_back("Build: " + std::string(fw::kBuildDate));
            lines.push_back("Chip: " + std::string(ESP.getChipModel()) + " rev " +
                            std::to_string(ESP.getChipRevision()));
            lines.push_back("Flash: " + std::to_string(ESP.getFlashChipSize() / (1024 * 1024)) + " MB");
            lines.push_back("MAC: " + macString());
            lines.push_back("Heap libero: " + std::to_string(ESP.getFreeHeap() / 1024) + " KB");
            // Stack residuo minimo del task dei menu: se scende sotto ~2 KB il
            // firmware e' a rischio di stack overflow (menu con icone fallback).
            lines.push_back("Stack min: " +
                            std::to_string(static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr))) +
                            " B");
            std::string batt = "Batteria: ";
            if (level >= 0) {
                batt += std::to_string(level) + "%" + (charging ? " (in carica)" : "");
            } else {
                batt += "non disponibile";
            }
            lines.push_back(batt);
            lines.push_back(std::string("Funzioni offensive: ") + (ENABLE_OFFENSIVE ? "ON" : "OFF"));
            lines.push_back("Moduli: BLE " + std::string(config::moduleEnabled(config::Module::Ble) ? "ON" : "OFF") +
                            " | WiFi " + (config::moduleEnabled(config::Module::Wifi) ? "ON" : "OFF") +
                            " | IR " + (config::moduleEnabled(config::Module::Ir) ? "ON" : "OFF"));

            display::clear();
            display::header("Info dispositivo", "Impostazioni");
            int y = 26;
            for (const auto& line : lines) {
                display::text(4, y, line, line.rfind(">", 0) == 0 ? display::kAccent : display::kText,
                              0);
                y += 11;
            }
            display::footer("` = torna");
            display::push();
        }

        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back || ev.type == navigation::Event::Select) {
            break;
        }
        delay(25);
    }
    navigation::waitRelease();
}

struct ToggleRow {
    const char* label;
    const char* icon;
    config::Module module;
};

void runModules() {
    const ToggleRow rows[] = {
        {"BLE", "ble", config::Module::Ble},
        {"Chat", "chat", config::Module::Chat},
        {"WiFi", "wifi", config::Module::Wifi},
        {"Radar", "radar", config::Module::Radar},
        {"Radio", "tv", config::Module::Radio},
        {"Infrarossi", "ir", config::Module::Ir},
        {"Giochi", "games", config::Module::Games},
        {"SD", "sd", config::Module::Sd},
    };
    constexpr int kCount = sizeof(rows) / sizeof(rows[0]);
    constexpr int kRowH = 20;
    const int visible = std::max(1, (display::height() - 22 - 12) / kRowH);

    navigation::waitRelease();
    int selected = 0;
    int top = 0;
    for (;;) {
        if (selected < top) {
            top = selected;
        } else if (selected >= top + visible) {
            top = selected - visible + 1;
        }

        display::clear();
        display::header("Moduli attivi", "Impostazioni");
        const int w = display::width();
        for (int row = 0; row < visible; ++row) {
            const int i = top + row;
            if (i >= kCount) {
                break;
            }
            const int y = 23 + row * kRowH;
            const bool sel = (i == selected);
            if (sel) {
                display::g().fillRoundRect(2, y, w - 6, 19, 3, display::kAccentDark);
                display::g().fillRect(2, y + 5, 2, 10, display::kAccent);
            }
            display::drawIcon(rows[i].icon, 6, y + 2, 16);
            display::text(28, y + 3, rows[i].label, display::kText, 2);
            const bool on = config::moduleEnabled(rows[i].module);
            const std::string state = on ? "[ON]" : "[OFF]";
            display::text(w - 42, y + 3, state, on ? display::kAccent : display::kTextDim, 2);
        }
        if (kCount > visible) {
            const int trackX = w - 3;
            const int trackY = 22;
            const int trackH = display::height() - 34;
            display::g().fillRect(trackX, trackY, 2, trackH, display::kPanel);
            const int thumbH = std::max(8, trackH * visible / kCount);
            const int thumbY =
                trackY + (trackH - thumbH) * top / std::max(1, kCount - visible);
            display::g().fillRect(trackX, thumbY, 2, thumbH, display::kAccent);
        }
        display::footer("; . = seleziona  ENTER = toggle  ` = torna");
        display::push();

        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Up) {
            selected = (selected + kCount - 1) % kCount;
        } else if (ev.type == navigation::Event::Down) {
            selected = (selected + 1) % kCount;
        } else if (ev.type == navigation::Event::Select) {
            const config::Module m = rows[selected].module;
            config::setModuleEnabled(m, !config::moduleEnabled(m));
            navigation::waitRelease();
        } else if (ev.type == navigation::Event::Back) {
            break;
        }
        delay(25);
    }
    navigation::waitRelease();
}

void exportConfig() {
    const bool ok = config::exportToFile("/config.json");
    ui::message("Backup config", {ok ? ">Configurazione salvata in" : "!Salvataggio fallito",
                                  ok ? "/config.json (LittleFS)" : "Verifica LittleFS (/data)."},
                "Impostazioni");
}

void importConfig() {
    if (!storage::exists("/config.json")) {
        ui::message("Importa config", {"!File /config.json non trovato.",
                                       "Carica il filesystem con:",
                                       "pio run -t uploadfs"},
                    "Impostazioni");
        return;
    }
    const bool ok = config::importFromFile("/config.json");
    if (ok) {
        applyDisplay();
        ui::message("Importa config", {">Configurazione importata."}, "Impostazioni");
    } else {
        ui::message("Importa config", {"!JSON non valido."}, "Impostazioni");
    }
}

void resetDefaults() {
    const bool ok = ui::confirm("Ripristina default",
                                {"Ripristinare luminosita, rotazione",
                                 "e moduli attivi ai valori iniziali?",
                                 "Le impostazioni NVS verranno azzerate."},
                                "Impostazioni");
    if (!ok) {
        return;
    }
    config::reset();
    applyDisplay();
    ui::message("Ripristino", {">Impostazioni ripristinate."}, "Impostazioni");
}

}  // namespace

void run() {
    MenuManager menu("Impostazioni", "Menu principale");
    menu.addItem("Luminosita", "settings", runBrightness);
    menu.addItem("Rotazione", "settings", runRotation);
    menu.addItem("Spegnimento auto", "settings", runSleepTimer);
    menu.addItem("Info dispositivo", "scan", showInfo);
    menu.addItem("Rete WiFi", "wifi", [] { wifisetup::runSettings("Impostazioni"); });
    menu.addItem("Aggiorna firmware", "scan", [] { ota::run("Impostazioni"); });
    menu.addItem("Moduli attivi", "lock", runModules);
    menu.addItem("Salva config su FS", "lock", exportConfig);
    menu.addItem("Importa config", "lock", importConfig);
    menu.addItem("Ripristina default", "lock", resetDefaults);
    menu.run(true);
}

}  // namespace settings_module

#endif  // ENABLE_MODULE_SETTINGS
