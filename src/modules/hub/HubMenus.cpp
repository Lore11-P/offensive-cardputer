#include "modules/hub/HubMenus.h"

#include <functional>
#include <string>

#include "Config.h"
#include "MenuManager.h"
#include "ModuleFlags.h"
#include "Ui.h"

#if ENABLE_MODULE_BLE
#include "modules/ble/BleSpam.h"
#endif
#if ENABLE_MODULE_CHAT && ENABLE_MODULE_BLE
#include "modules/chat/Chat.h"
#endif
#if ENABLE_MODULE_WIFI
#include "modules/wifi/WifiMenu.h"
#endif
#if ENABLE_MODULE_IR
#include "modules/ir/IrMenu.h"
#endif
#if ENABLE_MODULE_GAMES
#include "modules/games/GamesMenu.h"
#endif
#if ENABLE_MODULE_SETTINGS
#include "modules/settings/SettingsMenu.h"
#endif
#if ENABLE_MODULE_SD
#include "modules/sd/SdMenu.h"
#endif
#if ENABLE_MODULE_RADAR
#include "modules/radar/RadarMenu.h"
#endif
#if ENABLE_MODULE_RADIO
#include "modules/radio/RadioMenu.h"
#endif
#if ENABLE_MODULE_MUSIC
#include "modules/music/MusicMenu.h"
#endif
#if ENABLE_MODULE_PCAPTOOLS
#include "modules/pcap/PcapMenu.h"
#endif
#if ENABLE_MODULE_BADUSB
#include "modules/badusb/BadUsbMenu.h"
#endif
#if ENABLE_MODULE_AUDIO
#include "modules/audio/AudioMenu.h"
#endif
#if ENABLE_MODULE_BLEHID
#include "modules/blehid/BleHidMenu.h"
#endif
#if ENABLE_MODULE_TOOLS
#include "modules/tools/ToolsMenu.h"
#endif
#if ENABLE_MODULE_WIFILAB
#include "modules/wifilab/WifiLabMenu.h"
#endif
#if ENABLE_MODULE_WPASEC
#include "modules/wpasec/WpaSecMenu.h"
#endif
#if ENABLE_MODULE_SENSORS
#include "modules/sensors/SensorsMenu.h"
#endif
#if ENABLE_MODULE_WIFIEXT
#include "modules/wifiext/WifiExtMenu.h"
#endif
#if ENABLE_MODULE_ESPNOW
#include "modules/espnow/EspNowMenu.h"
#endif

namespace hub {
namespace {

// I moduli con toggle runtime mantengono il messaggio esplicativo di prima.
std::function<void()> guarded(const char* name, const char* parent, config::Module module,
                              void (*entry)()) {
    return [name, parent, module, entry]() {
        if (!config::moduleEnabled(module)) {
            ui::message("Modulo disattivato",
                        {std::string(name) + " e' disattivato a runtime.",
                         "Attivalo da Impostazioni > Moduli attivi."},
                        parent);
            return;
        }
        entry();
    };
}

// Breadcrumb dei menu radice dei moduli aperti dal hub.
class RootScope {
public:
    explicit RootScope(const char* label) { MenuManager::setRootContext(label); }
    ~RootScope() { MenuManager::setRootContext(nullptr); }
};

constexpr const char* kHubHint = "ESC=indietro  ;.=nav  ENTER=apri";

}  // namespace

void networkMenu() {
    MenuManager menu("Rete", "Menu principale");
    RootScope scope("Rete");
#if ENABLE_MODULE_WIFI
    menu.addItem("WiFi", "wifi", guarded("WiFi", "Rete", config::Module::Wifi, wifi::runMenu));
#endif
#if ENABLE_MODULE_WIFILAB
    menu.addItem("WiFi Lab", "wifilab", [] { wifilab::runMenu(); });
#endif
#if ENABLE_MODULE_WIFIEXT
    menu.addItem("WiFi+", "wifiplus", [] { wifiext::runMenu(); });
#endif
#if ENABLE_MODULE_WPASEC
    menu.addItem("WPA-SEC", "wpasec", [] { wpasec::runMenu(); });
#endif
#if ENABLE_MODULE_ESPNOW
    menu.addItem("ESP-NOW", "espnow", [] { espnow::runMenu(); });
#endif
    menu.setHint(kHubHint);
    menu.run(true);
}

void bluetoothMenu() {
    MenuManager menu("Bluetooth", "Menu principale");
    RootScope scope("Bluetooth");
#if ENABLE_MODULE_BLE
    menu.addItem("BLE", "ble", guarded("BLE", "Bluetooth", config::Module::Ble, ble::runMenu));
#endif
#if ENABLE_MODULE_BLEHID
    menu.addItem("BLE HID", "hid", [] { blehid::runMenu(); });
#endif
#if ENABLE_MODULE_CHAT && ENABLE_MODULE_BLE
    menu.addItem("Chat", "chat",
                 guarded("Chat", "Bluetooth", config::Module::Chat, chat::runMenu));
#endif
    menu.setHint(kHubHint);
    menu.run(true);
}

void analysisMenu() {
    MenuManager menu("Analisi RF", "Menu principale");
    RootScope scope("Analisi RF");
#if ENABLE_MODULE_RADAR
    menu.addItem("Radar", "radar", guarded("Radar", "Analisi RF", config::Module::Radar,
                                           radar::runMenu));
#endif
#if ENABLE_MODULE_PCAPTOOLS
    menu.addItem("Capture", "capture", guarded("Capture", "Analisi RF",
                                               config::Module::PcapTools, pcaptools_ui::runMenu));
#endif
    menu.setHint(kHubHint);
    menu.run(true);
}

void mediaMenu() {
    MenuManager menu("Media", "Menu principale");
    RootScope scope("Media");
#if ENABLE_MODULE_RADIO
    menu.addItem("Radio", "tv",
                 guarded("Radio", "Media", config::Module::Radio, radio::runMenu));
#endif
#if ENABLE_MODULE_MUSIC
    menu.addItem("Musica", "music",
                 guarded("Musica", "Media", config::Module::Music, music::runMenu));
#endif
#if ENABLE_MODULE_AUDIO
    menu.addItem("Audio", "audio", [] { audio::runMenu(); });
#endif
    menu.setHint(kHubHint);
    menu.run(true);
}

void toolsMenu() {
    MenuManager menu("Strumenti", "Menu principale");
    RootScope scope("Strumenti");
#if ENABLE_MODULE_TOOLS
    menu.addItem("Utility", "tools", [] { tools::runMenu(); });
#endif
#if ENABLE_MODULE_SENSORS
    menu.addItem("Sensori", "sensors", [] { sensors::runMenu(); });
#endif
#if ENABLE_MODULE_IR
    menu.addItem("Infrarossi", "ir", guarded("IR", "Strumenti", config::Module::Ir, ir::runMenu));
#endif
#if ENABLE_MODULE_BADUSB
    menu.addItem("BadUSB", "usb",
                 guarded("BadUSB", "Strumenti", config::Module::BadUsb, badusb::runMenu));
#endif
    menu.setHint(kHubHint);
    menu.run(true);
}

void systemMenu() {
    MenuManager menu("Sistema", "Menu principale");
    RootScope scope("Sistema");
#if ENABLE_MODULE_SD
    menu.addItem("SD", "sd", guarded("SD", "Sistema", config::Module::Sd, sd_module::runMenu));
#endif
#if ENABLE_MODULE_SETTINGS
    menu.addItem("Impostazioni", "settings", settings_module::run);
#else
    menu.addItem("Impostazioni", "settings",
                 [] { ui::message("Impostazioni", {"Modulo non compilato."}, "Sistema"); });
#endif
    menu.setHint(kHubHint);
    menu.run(true);
}

void gamesEntry() {
#if ENABLE_MODULE_GAMES
    guarded("Giochi", "Menu principale", config::Module::Games, games::runMenu)();
#endif
}

}  // namespace hub
