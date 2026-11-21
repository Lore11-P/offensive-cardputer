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
#include "Orbit.h"
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

void runBrightness() {
    navigation::waitRelease();
    uint32_t nextRepeat = 0;
    bool leftHeld = false;
    bool rightHeld = false;
    // Il valore si applica subito al pannello ma si salva in NVS una volta
    // sola all'uscita: con l'auto-repeat si scriverebbe ~11 volte al secondo.
    uint8_t local = config::brightness();

    uint32_t lastDraw = 0;
    for (;;) {
        bool changed = false;
        const int step = 26;
        if (navigation::pressed(',') && millis() >= nextRepeat) {
            const int v = static_cast<int>(local) - step;
            local = static_cast<uint8_t>(v < 10 ? 10 : v);
            M5Cardputer.Display.setBrightness(local);
            nextRepeat = millis() + (leftHeld ? 90 : 320);
            leftHeld = true;
            changed = true;
        } else if (!navigation::pressed(',')) {
            leftHeld = false;
        }
        if (navigation::pressed('/') && millis() >= nextRepeat) {
            const int v = static_cast<int>(local) + step;
            local = static_cast<uint8_t>(v > 255 ? 255 : v);
            M5Cardputer.Display.setBrightness(local);
            nextRepeat = millis() + (rightHeld ? 90 : 320);
            rightHeld = true;
            changed = true;
        } else if (!navigation::pressed('/')) {
            rightHeld = false;
        }

        const uint32_t now = millis();
        if (changed || lastDraw == 0 || now - lastDraw >= 500) {
            display::clear();
            display::header("Luminosita", "Impostazioni");
            display::textCentered(35,
                                  std::to_string((static_cast<int>(local) * 100 + 127) / 255) + "%",
                                  display::kAccent, 4);
            display::progressBar(28, 66, display::width() - 56, 10,
                                 static_cast<float>(local) / 255.0f);
            display::textCentered(84,
                                  "valore " + std::to_string(local) + " / 255",
                                  display::kTextDim, 0);
            display::textCentered(102, ", = -10%    / = +10%", display::kText, 0);
            display::footer("ENTER/ESC = salva e torna");
            display::push();
            lastDraw = now;
        }

        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back || ev.type == navigation::Event::Select) {
            break;
        }
        delay(changed ? 25 : 30);
    }
    config::setBrightness(local);
    M5Cardputer.Display.setBrightness(config::brightness());
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
    uint32_t lastDraw = 0;
    // Anteprima su schermo e salvataggio solo all'uscita: cosi' non si
    // riscrivono tutte le chiavi NVS a ogni passo.
    uint8_t local = config::rotation();
    const uint8_t initial = local;
    bool commit = false;
    for (;;) {
        bool changed = false;
        if (navigation::pressed(',') && millis() >= nextRepeat) {
            local = static_cast<uint8_t>((local + 3) & 3);
            display::applyRotation(local);
            nextRepeat = millis() + 250;
            changed = true;
        }
        if (navigation::pressed('/') && millis() >= nextRepeat) {
            local = static_cast<uint8_t>((local + 1) & 3);
            display::applyRotation(local);
            nextRepeat = millis() + 250;
            changed = true;
        }

        const uint32_t now = millis();
        if (changed || lastDraw == 0 || now - lastDraw >= 500) {
            display::clear();
            display::header("Rotazione", "Impostazioni");
            display::textCentered(36, rotationLabel(local), display::kAccent, 2);
            display::textCentered(60, "indice display: " + std::to_string(local),
                                  display::kTextDim, 0);
            display::textCentered(88, ", = precedente    / = successiva", display::kText, 0);
            display::footer("ENTER = salva    ESC = annulla");
            display::push();
            lastDraw = now;
        }

        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Select) {
            commit = true;
            break;
        }
        if (ev.type == navigation::Event::Back) {
            break;
        }
        delay(changed ? 40 : 30);
    }
    if (commit) {
        config::setRotation(local);
    } else {
        display::applyRotation(initial);
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
    uint32_t lastDraw = 0;
    uint16_t lastValue = 0xFFFF;
    for (;;) {
        const uint16_t current = config::sleepTimeoutSec();
        const uint32_t now = millis();
        if (current != lastValue || lastDraw == 0 || now - lastDraw >= 500) {
            display::clear();
            display::header("Spegnimento auto", "Impostazioni");
            display::textCentered(32, sleepLabel(current), display::kAccent, 2);
            display::textCentered(56, "Spegne la Cardputer dopo", display::kTextDim, 0);
            display::textCentered(68, "questo tempo senza tasti.", display::kTextDim, 0);
            display::textCentered(88, "Un tasto durante il preavviso", display::kMuted, 0);
            display::textCentered(98, "annulla lo spegnimento.", display::kMuted, 0);
            display::textCentered(110, ", / = cambia", display::kText, 0);
            display::footer("ENTER/ESC = salva e torna");
            display::push();
            lastDraw = now;
            lastValue = current;
        }

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
            const bool charging = display::chargingLikely();

            std::vector<std::string> lines;
            lines.push_back(std::string(">") + fw::kTitle);
            lines.push_back("Build: " + std::string(fw::kBuildDate));
            lines.push_back("Chip: " + std::string(ESP.getChipModel()) + " rev " +
                            std::to_string(ESP.getChipRevision()));
            lines.push_back("Flash: " + std::to_string(ESP.getFlashChipSize() / (1024 * 1024)) + " MB");
            lines.push_back("MAC: " + macString());
            lines.push_back("Heap libero: " + std::to_string(ESP.getFreeHeap() / 1024) + " KB");
            lines.push_back(std::string("Framebuffer: ") +
                            (display::framebufferActive()
                                 ? std::to_string(display::framebufferBytes() / 1024) + " KB"
                                 : std::string("diretto (flicker)")));
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
            display::footer("ESC = torna");
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

void runTheme() {
    navigation::waitRelease();
    uint8_t local = config::theme();
    const uint8_t initial = local;
    display::setTheme(local);
    uint32_t nextRepeat = 0;
    uint32_t lastDraw = 0;
    uint8_t lastShown = 0xFF;
    bool commit = false;
    for (;;) {
        bool changed = false;
        if (navigation::pressed(',') && millis() >= nextRepeat) {
            local = static_cast<uint8_t>((local + display::kThemeCount - 1) % display::kThemeCount);
            display::setTheme(local);
            nextRepeat = millis() + 260;
            changed = true;
        }
        if (navigation::pressed('/') && millis() >= nextRepeat) {
            local = static_cast<uint8_t>((local + 1) % display::kThemeCount);
            display::setTheme(local);
            nextRepeat = millis() + 260;
            changed = true;
        }

        const uint32_t now = millis();
        if (changed || lastShown != local || lastDraw == 0 || now - lastDraw >= 900) {
            display::clear(display::C(display::T_bg));
            orbit::sky(0, 0, display::width(), display::height(), 0x7E7A);
            display::header("Tema colore", "Impostazioni");

            // Anteprima: pianeta + razzo in orbita, come nella home.
            const int cx = 196;
            const int cy = 56;
            orbit::orbitPath(cx, cy, 34, 12, display::C(display::T_line2), display::C(display::T_line), 0);
            orbit::planet(cx, cy, 16, -0.6f, -0.5f, true);
            orbit::rocket(cx + 26, cy - 8, 9, 0.9f, true, static_cast<uint8_t>(now / 90));

            display::text(10, 34, "PALETTE", display::C(display::T_dim), 0);
            display::text(10, 44, display::themeName(local), display::C(display::T_hi), 4);
            display::text(10, 64, display::themeNote(local), display::C(display::T_text), 0);

            // Barra dei sette temi: quattro bande per palette.
            auto& d = display::g();
            for (int i = 0; i < display::kThemeCount; ++i) {
                const int bx = 10 + i * 30;
                const bool sel = (i == local);
                d.fillRect(bx, 80, 26, 2, display::themeColor(i, display::T_acc));
                d.fillRect(bx, 82, 26, 2, display::themeColor(i, display::T_bg));
                d.fillRect(bx, 84, 26, 2, display::themeColor(i, display::T_sec));
                d.fillRect(bx, 86, 26, 2, display::themeColor(i, display::T_text));
                d.drawRect(bx, 80, 26, 8,
                           sel ? display::C(display::T_hi) : display::C(display::T_line));
                if (sel) {
                    d.drawRect(bx - 1, 79, 28, 10, display::C(display::T_acc));
                }
            }

            display::text(10, 96, "indice " + std::to_string(local + 1) + " / " +
                                      std::to_string(display::kThemeCount),
                          display::C(display::T_dim), 0);
            display::text(10, 106, ", / = cambia palette", display::C(display::T_text), 0);
            display::footer("ENTER = salva    ESC = annulla");
            display::push();
            lastDraw = now;
            lastShown = local;
        }

        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Select) {
            commit = true;
            break;
        }
        if (ev.type == navigation::Event::Back) {
            break;
        }
        delay(changed ? 30 : 40);
    }
    if (commit) {
        config::setTheme(local);
    } else {
        display::setTheme(initial);
    }
    navigation::waitRelease();
}

void runModules() {
    const ToggleRow rows[] = {
        {"BLE", "ble", config::Module::Ble},
        {"Chat", "chat", config::Module::Chat},
        {"WiFi", "wifi", config::Module::Wifi},
        {"Radar", "radar", config::Module::Radar},
            {"Radio", "tv", config::Module::Radio},
            {"Musica", "music", config::Module::Music},
        {"Infrarossi", "ir", config::Module::Ir},
        {"Giochi", "games", config::Module::Games},
            {"SD", "sd", config::Module::Sd},
#if ENABLE_MODULE_BADUSB
            {"BadUSB", "usb", config::Module::BadUsb},
#endif
            {"Capture", "capture", config::Module::PcapTools},
            {"BLE+", "ble", config::Module::BleExt},
    };
    constexpr int kCount = sizeof(rows) / sizeof(rows[0]);
    constexpr int kRowH = 20;
    const int visible = std::max(1, (display::height() - 22 - 12) / kRowH);

    navigation::waitRelease();
    int selected = 0;
    int top = 0;
    uint32_t lastDraw = 0;
    bool dirty = true;
    for (;;) {
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Up) {
            selected = (selected + kCount - 1) % kCount;
            dirty = true;
        } else if (ev.type == navigation::Event::Down) {
            selected = (selected + 1) % kCount;
            dirty = true;
        } else if (ev.type == navigation::Event::Select) {
            const config::Module m = rows[selected].module;
            if (config::moduleCompiled(m)) {
                config::setModuleEnabled(m, !config::moduleEnabled(m));
            }
            navigation::waitRelease();
            dirty = true;
        } else if (ev.type == navigation::Event::Back) {
            break;
        }

        if (selected < top) {
            top = selected;
        } else if (selected >= top + visible) {
            top = selected - visible + 1;
        }

        const uint32_t now = millis();
        if (dirty || lastDraw == 0 || now - lastDraw >= 500) {
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
                const bool compiled = config::moduleCompiled(rows[i].module);
                const bool on = config::moduleEnabled(rows[i].module);
                const char* state = !compiled ? "[n/d]" : (on ? "[ON]" : "[OFF]");
                display::text(w - 42, y + 3, state,
                              (compiled && on) ? display::kAccent : display::kTextDim, 2);
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
            display::footer("; . = seleziona  ENTER = toggle  ESC = torna");
            display::push();
            lastDraw = now;
            dirty = false;
        }
        delay(25);
    }
    navigation::waitRelease();
}

void exportConfig() {
    if (!ui::confirm("Salva config su FS",
                     {"Sovrascrivere /config.json con", "le impostazioni attuali?"},
                     "Impostazioni")) {
        return;
    }
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
    if (!ui::confirm("Importa config",
                     {"Importare /config.json?", "Le impostazioni attuali",
                      "verranno sovrascritte."},
                     "Impostazioni")) {
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
    menu.addItem("Rotazione", "rotate", runRotation);
    menu.addItem("Tema colore", "planet", runTheme);
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
