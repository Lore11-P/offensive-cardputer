#include "BleHidMenu.h"

#include "ModuleFlags.h"

#if ENABLE_MODULE_BLEHID

#include <M5Cardputer.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include "BleHidKeyboard.h"
#include "BleHidScript.h"
#include "Config.h"
#include "Display.h"
#include "MenuManager.h"
#include "Navigation.h"
#include "PowerTimer.h"
#include "Ui.h"

namespace blehid {
namespace {

constexpr uint8_t kModShift = 0x02;
constexpr uint8_t kModAltGr = 0x40;

std::string deviceName() {
    const std::string raw = config::chatNickname();
    std::string nick;
    for (char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
            nick.push_back(c);
        }
        if (nick.size() >= 8) {
            break;
        }
    }
    if (nick.empty()) {
        nick = "CP";
    }
    return "ORBIT-" + nick;
}

const char* layoutName() {
    return hid::layout() == Layout::Us ? "US" : "Italiano";
}

void drawWaiting(const std::string& title, const std::string& name,
                 bool script) {
    display::clear(display::kBackground);
    display::header(title, "BLE HID");
    display::textCentered(30, script ? "Attendo l'host" : "Tastiera Bluetooth",
                          display::kAccent, 4);
    display::text(8, 50, "Nome: " + name, display::kText, 0);
    display::text(8, 60, std::string("Layout: ") + layoutName(),
                  display::kTextDim, 0);

    const bool up = hid::linkUp();
    const bool secure = hid::linkSecure();
    const bool authorized = hid::authorized();
    const bool ready = hid::ready();
    if (up) {
        display::text(8, 72, "Host: " + hid::peerAddress(), display::kInfo, 0);
    }

    const char* status = ready         ? "Collegato"
                         : !up         ? "In attesa di connessione"
                         : !secure     ? "Connesso: attendo pairing"
                         : !authorized ? "Host da autorizzare"
                                       : "Attendo i report";
    display::text(8, 88, status,
                  ready ? display::C(display::T_ok) : display::kWarning, 2);

    display::text(8, 104, "Associa il dispositivo dalle", display::kTextDim, 0);
    display::text(8, 114, "impostazioni Bluetooth.", display::kTextDim, 0);
    display::footer("ESC = annulla");
    display::push();
}

// Attende connessione, cifratura e autorizzazione esplicita del peer.
bool waitHost(const std::string& title, const std::string& name, bool script) {
    powertimer::Hold hold;
    navigation::waitRelease();
    uint32_t lastDraw = 0;
    for (;;) {
        const uint32_t now = millis();
        if (lastDraw == 0 || now - lastDraw >= 250) {
            drawWaiting(title, name, script);
            lastDraw = now;
        }

        // La UI non dice mai "Collegato" prima della cifratura. Dopo il
        // pairing serve la conferma sulla Cardputer: Just Works non
        // autentica il central, quindi l'utente verifica l'indirizzo.
        if (hid::linkUp() && hid::linkSecure() && !hid::authorized()) {
            if (!ui::confirm("Autorizza host",
                             {"Host: " + hid::peerAddress(),
                              "Link Bluetooth cifrato (Just Works).",
                              "Autorizza solo un PC/telefono tuo.",
                              "ENTER = autorizza   ESC = rifiuta"},
                             title)) {
                return false;
            }
            if (!hid::authorizePeer()) {
                return false;
            }
            lastDraw = 0;
            continue;
        }

        if (hid::ready()) {
            // Lascia completare discovery e sottoscrizione prima dei tasti.
            delay(300);
            navigation::waitRelease();
            return hid::ready();
        }

        if (navigation::poll().type == navigation::Event::Back) {
            return false;
        }
        delay(40);
    }
}

bool wordHas(const std::vector<char>& word, char c) {
    return std::find(word.begin(), word.end(), c) != word.end();
}

bool hasKey(const std::vector<uint8_t>& keys, uint8_t code) {
    return std::find(keys.begin(), keys.end(), code) != keys.end();
}

std::string ledText(uint8_t leds) {
    std::string out;
    if ((leds & 0x02) != 0) {
        out += "CAPS ";
    }
    if ((leds & 0x01) != 0) {
        out += "NUM ";
    }
    if ((leds & 0x04) != 0) {
        out += "SCROLL";
    }
    if (out.empty()) {
        out = "-";
    }
    return out;
}

void drawLive(const std::string& name, uint32_t typed,
              const std::string& preview) {
    display::clear(display::kBackground);
    display::header("BLE HID", "Tastiera");
    const bool ready = hid::ready();
    display::text(8, 26, ready ? "Collegato" : "Report non pronti",
                  ready ? display::C(display::T_ok) : display::kWarning, 2);
    display::text(8, 41, "Host: " + hid::peerAddress(), display::kInfo, 0);
    display::text(8, 51, "Nome: " + name + "  Layout: " + layoutName(),
                  display::kTextDim, 0);
    display::text(8, 61, "LED: " + ledText(hid::leds()), display::kTextDim, 0);
    display::text(8, 75, "Tasti inviati: " + std::to_string(typed),
                  display::kText, 2);
    display::text(8, 91, "> " + preview, display::kAccent, 0);
    display::footer("ESC = esci");
    display::push();
}

// Tastiera dal vivo: ogni tasto della Cardputer diventa un report HID.
// ESC (') esce senza essere inviato all'host; DEL invia backspace.
void runLive(const std::string& name) {
    M5Cardputer.update();
    auto& initial = M5Cardputer.Keyboard.keysState();
    std::vector<char> lastWord = initial.word;
    std::vector<uint8_t> lastHid = initial.hid_keys;
    bool lastEnter = initial.enter;
    bool lastTab = initial.tab;
    bool lastDel = initial.del;

    uint32_t typed = 0;
    uint32_t nextBackspace = 0;
    uint32_t lastDraw = 0;
    std::string preview;

    for (;;) {
        M5Cardputer.update();
        powertimer::tick();
        auto& state = M5Cardputer.Keyboard.keysState();
        const uint32_t now = millis();

        if (wordHas(state.word, '`') && !wordHas(lastWord, '`')) {
            break;
        }
        if (!hid::linkUp()) {
            // Chiudi solo quando il server non ha piu' peer: la
            // disconnessione di un central estraneo non deve terminare la
            // sessione dell'host legittimo. ESC resta sempre disponibile.
            if (hid::peerCount() == 0) {
                break;
            }
            delay(20);
            continue;
        }
        if (!hid::authorized()) {
            break;  // cifratura o autorizzazione persa: nessun invio
        }

        bool sent = false;
        if (state.ctrl || state.alt || state.opt) {
            // Combinazioni CTRL/ALT/ALTGR: si usa il tasto fisico, non il
            // carattere maiuscolo che la libreria riporta in word.
            const uint8_t mods =
                static_cast<uint8_t>(state.modifiers | (state.opt ? kModAltGr : 0));
            for (uint8_t code : state.hid_keys) {
                if (hasKey(lastHid, code)) {
                    continue;
                }
                const uint8_t usage = static_cast<uint8_t>(code & 0x7F);
                const uint8_t extra = (code & 0x80) != 0 ? kModShift : 0;
                if (hid::tapUsage(usage, static_cast<uint8_t>(mods | extra))) {
                    ++typed;
                    sent = true;
                }
            }
        } else {
            for (char c : state.word) {
                if (c == '`' || wordHas(lastWord, c)) {
                    continue;
                }
                if (hid::tapChar(c)) {
                    ++typed;
                    sent = true;
                    preview.push_back(c);
                    if (preview.size() > 18) {
                        preview.erase(0, preview.size() - 18);
                    }
                }
            }
            if (state.enter && !lastEnter) {
                if (hid::tapSpecial(kEnter)) {
                    ++typed;
                    sent = true;
                    preview.push_back(' ');
                    if (preview.size() > 18) {
                        preview.erase(0, preview.size() - 18);
                    }
                }
            }
            if (state.tab && !lastTab) {
                if (hid::tapSpecial(kTab)) {
                    ++typed;
                    sent = true;
                }
            }
            // Nella libreria M5Cardputer 1.1.1 "del" e' il tasto backspace.
            if (state.del) {
                if (!lastDel) {
                    if (hid::tapSpecial(kBackspace)) {
                        ++typed;
                        sent = true;
                    }
                    nextBackspace = now + 400;
                } else if (now >= nextBackspace) {
                    if (hid::tapSpecial(kBackspace)) {
                        ++typed;
                        sent = true;
                    }
                    nextBackspace = now + 60;
                }
            }
        }

        lastWord = state.word;
        lastHid = state.hid_keys;
        lastEnter = state.enter;
        lastTab = state.tab;
        lastDel = state.del;

        if (sent || now - lastDraw >= 500) {
            drawLive(name, typed, preview);
            lastDraw = now;
        }
        delay(8);
    }

    hid::releaseAll();
    // La lettura diretta non aggiorna Navigation: riallinea prima del menu.
    navigation::begin();
}

struct HidSession {
    bool ok = false;
    explicit HidSession(const std::string& name) {
        ok = hid::begin(name);
    }
    ~HidSession() {
        hid::end();
    }
    HidSession(const HidSession&) = delete;
    HidSession& operator=(const HidSession&) = delete;
};

void showStartError(hid::StartError error) {
    switch (error) {
        case hid::StartError::LowHeap:
            ui::message("BLE HID",
                        {"!Memoria insufficiente.", "Servono almeno 32 KB liberi.",
                         "Chiudi le altre funzioni e riprova."},
                        "BLE HID");
            break;
        case hid::StartError::PeerConnected:
            ui::message("BLE HID",
                        {"!BLE gia' in uso.", "Un host e' connesso a un altro modulo.",
                         "Attendi la disconnessione e riprova."},
                        "BLE HID");
            break;
        default:
            ui::message("BLE HID",
                        {"!Stack BLE non disponibile.", "Riprova o riavvia il dispositivo."},
                        "BLE HID");
            break;
    }
}

void runKeyboard() {
    if (!ui::legalWarning("Tastiera BLE HID",
                          {"Serve autorizzazione esplicita dell'host."},
                          "BLE HID")) {
        return;
    }

    const std::string name = deviceName();
    {
        HidSession session(name);
        if (!session.ok) {
            showStartError(hid::lastError());
            return;
        }
        if (!waitHost("Tastiera BLE", name, false)) {
            return;
        }
        runLive(name);
    }
    ui::message("BLE HID", {"Sessione terminata.", "Stack BLE rilasciato."}, "BLE HID");
}

void runBadScript(const std::string& name, bool preferSd) {
#if ENABLE_OFFENSIVE
    if (!ui::legalWarning("Bad BLE - " + name,
                          {"Host Bluetooth associato e autorizzato."},
                          "Bad BLE")) {
        return;
    }

    const std::string path = "/badusb/" + name;
    const std::string device = deviceName();
    ScriptResult result;
    {
        HidSession session(device);
        if (!session.ok) {
            showStartError(hid::lastError());
            return;
        }
        if (!waitHost("Bad BLE", device, true)) {
            return;
        }
        if (!ui::confirm("Bad BLE",
                         {"Script: " + name,
                          "Host: " + hid::peerAddress(),
                          "Avvio esecuzione..."},
                         "Bad BLE")) {
            return;
        }
        result = runScript(path, name, preferSd);
    }

    if (result.disconnected) {
        ui::message("Bad BLE",
                    {"!Host disconnesso.", "Script interrotto.",
                     "Riga " + std::to_string(result.lines) + " / " +
                         std::to_string(result.total)},
                    "Bad BLE");
        return;
    }
    if (result.aborted) {
        ui::message("Bad BLE",
                    {"Script annullato.",
                     "Riga " + std::to_string(result.lines) + " / " +
                         std::to_string(result.total)},
                    "Bad BLE");
        return;
    }
    if (result.completed) {
        ui::message("Bad BLE",
                    {"Script completato.", name,
                     "Righe: " + std::to_string(result.lines),
                     "Tasti: " + std::to_string(hid::sentKeys())},
                    "Bad BLE");
        return;
    }
    ui::message("Bad BLE",
                {"!Esecuzione non riuscita.",
                 "Riga " + std::to_string(result.lines) + " / " +
                     std::to_string(result.total)},
                "Bad BLE");
#else
    (void)name;
    (void)preferSd;
    ui::message("Bad BLE",
                {"Funzione non compilata.", "Compila con ENABLE_OFFENSIVE=1."},
                "Bad BLE");
#endif
}

void runBadBle() {
#if ENABLE_OFFENSIVE
    bool preferSd = false;
    const std::vector<std::string> scripts = listScripts(preferSd);
    if (scripts.empty()) {
        ui::message("Bad BLE",
                    {"!Nessuno script trovato.", "Copia i file in /badusb sulla SD",
                     "oppure su LittleFS."},
                    "Bad BLE");
        return;
    }

    MenuManager menu("Script /badusb", "Bad BLE");
    menu.addSeparator(preferSd ? "Sorgente: SD" : "Sorgente: LittleFS");
    for (const auto& script : scripts) {
        menu.addItem(script, "usb", [script, preferSd] { runBadScript(script, preferSd); });
    }
    menu.setHint("ENTER = esegui  ESC = indietro");
    menu.run(true);
#else
    ui::message("Bad BLE",
                {"Funzione non compilata.", "Compila con ENABLE_OFFENSIVE=1."},
                "Bad BLE");
#endif
}

void chooseLayout() {
    MenuManager menu("Layout host", "BLE HID");
    const bool it = hid::layout() == Layout::It;
    menu.addItem(std::string(it ? "> " : "  ") + "Italiano", "settings",
                 [] { hid::setLayout(Layout::It); });
    menu.addItem(std::string(it ? "  " : "> ") + "US / International", "settings",
                 [] { hid::setLayout(Layout::Us); });
    menu.setHint("ENTER = imposta  ESC = indietro");
    menu.run(true);
}

void showInfo() {
    ui::message("Info BLE HID",
                {"Nome: " + deviceName(),
                 std::string("Layout host: ") + layoutName(),
                 "Un solo host, link cifrato e autorizzato",
                 "Report 0x2A4D; 0x2A22 solo in Boot",
                 "Script: /badusb (SD, poi LittleFS)",
                 "Solo tastiera. NimBLE rilasciato all'uscita."},
                "BLE HID");
}

}  // namespace

void runMenu() {
    MenuManager menu("BLE HID", "Menu principale");
    menu.addItem("Tastiera BLE", "ble", [] { runKeyboard(); });
#if ENABLE_OFFENSIVE
    menu.addItem("Bad BLE", "usb", [] { runBadBle(); });
#endif
    menu.addSeparator("Impostazioni");
    menu.addItem("Layout", "settings", [] { chooseLayout(); });
    menu.addItem("Info", "info", [] { showInfo(); });
    menu.setHint("ESC = indietro  ;. = nav  ENTER = apri");
    menu.run(true);
}

}  // namespace blehid

#endif  // ENABLE_MODULE_BLEHID
