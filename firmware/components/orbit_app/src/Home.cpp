#include "Home.h"

#include "Display.h"
#include "MenuManager.h"
#include "SdUi.h"
#include "SettingsUi.h"
#include "StoreUi.h"
#include "Ui.h"
#include "orbit_chat/Chat.h"
#include "orbit_mascot/Mascot.h"

#if __has_include("orbit_audio/Audio.h")
#include "orbit_audio/Audio.h"
#define ORBIT_HAS_AUDIO 1
#endif
#if __has_include("orbit_ble/Ble.h")
#include "orbit_ble/Ble.h"
#define ORBIT_HAS_BLE 1
#endif
#if __has_include("orbit_wifi/Wifi.h")
#include "orbit_wifi/Wifi.h"
#define ORBIT_HAS_WIFI 1
#endif
#if __has_include("orbit_ir/Ir.h")
#include "orbit_ir/Ir.h"
#define ORBIT_HAS_IR 1
#endif
#if __has_include("orbit_tools/Tools.h")
#include "orbit_tools/Tools.h"
#define ORBIT_HAS_TOOLS 1
#endif

namespace orbit_app {
namespace {

void runBluetooth() {
    MenuManager menu("Bluetooth", "Home");
#ifdef ORBIT_HAS_BLE
    menu.addItem("Scanner e BLE spam", "ble", [] { orbit_ble::runMenu(); });
#else
    menu.addItem("Scanner BLE non disponibile", "ble", [] {}, false);
#endif
    menu.addItem("Chat BLE", "chat", [] { orbit_chat::runMenu(); });
    menu.setHint("ENTER=apri  `=indietro");
    menu.run(true);
}

void runNetwork() {
    MenuManager menu("Rete", "Home");
#ifdef ORBIT_HAS_WIFI
    menu.addItem("WiFi scanner e lab", "wifi", [] { orbit_wifi::runMenu(); });
#endif
    menu.addItem("Configura rete", "wifi", [] { runWifiSetup(); });
    menu.setHint("ENTER=apri  `=indietro");
    menu.run(true);
}

void runRf() {
#ifdef ORBIT_HAS_IR
    orbit_ir::runMenu();
#else
    ui::message("Analisi RF", {"Modulo IR non disponibile in questa build."}, "Home");
#endif
}

void runTools() {
    MenuManager menu("Strumenti", "Home");
#ifdef ORBIT_HAS_TOOLS
    menu.addItem("Utility", "tools", [] { orbit_tools::runMenu(); });
#endif
    menu.addItem("Scheda SD", "sd", [] { runSdMenu(); });
    menu.setHint("ENTER=apri  `=indietro");
    menu.run(true);
}

}  // namespace

void runHome() {
    MenuManager menu("ORBIT OS");
    menu.addItem("Store", "store", [] { runStore(); });
    menu.addItem("Giochi", "games", [] { runGames(); });
#ifdef ORBIT_HAS_AUDIO
    menu.addItem("Media", "music", [] { orbit_audio::runMenu(); });
#endif
    menu.addItem("Bluetooth", "ble", [] { runBluetooth(); });
#ifdef ORBIT_HAS_WIFI
    menu.addItem("Rete", "wifi", [] { runNetwork(); });
#endif
#ifdef ORBIT_HAS_IR
    menu.addItem("Analisi RF", "ir", [] { runRf(); });
#endif
#ifdef ORBIT_HAS_TOOLS
    menu.addItem("Strumenti", "tools", [] { runTools(); });
#endif
    menu.addItem("Zorbo", "alien", [] { mascot::runMenu(); });
    menu.addItem("Impostazioni", "settings", [] { runSettings(); });
    menu.setHint("ENTER=apri  `=indietro");
    menu.setCarousel();
    menu.run(false);
}

}  // namespace orbit_app
