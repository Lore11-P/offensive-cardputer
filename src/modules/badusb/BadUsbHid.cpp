#include "BadUsbHid.h"

#include "ModuleFlags.h"

#if ENABLE_MODULE_BADUSB

#if defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE
#error "ENABLE_MODULE_BADUSB richiede ARDUINO_USB_MODE=0 (env cardputer-adv-hid)"
#endif

#include <Arduino.h>
#include <USB.h>
#include <USBHIDKeyboard.h>

#include "Logger.h"

namespace badusb {
namespace hid {
namespace {

// Unica istanza della tastiera. Il costruttore registra l'interfaccia HID
// presso TinyUSB e con ARDUINO_USB_CDC_ON_BOOT=1 il core chiama USB.begin()
// prima di setup(): la registrazione deve quindi avvenire durante
// l'inizializzazione statica. Un oggetto locale arriverebbe troppo tardi e
// l'interfaccia HID non comparirebbe nel descriptor USB.
USBHIDKeyboard g_keyboard;

bool g_begun = false;

// Converte i codici del modulo nei codici attesi da USBHIDKeyboard. ASCII
// invariato; i tasti speciali diventano KEY_* letti da USBHIDKeyboard.h.
uint8_t usbCode(uint8_t code) {
    if (code < 0x80) {
        return code;
    }
    switch (code) {
        case kEnter:
            return KEY_RETURN;
        case kTab:
            return KEY_TAB;
        case kEsc:
            return KEY_ESC;
        case kBackspace:
            return KEY_BACKSPACE;
        case kDelete:
            return KEY_DELETE;
        case kCapsLock:
            return KEY_CAPS_LOCK;
        case kInsert:
            return KEY_INSERT;
        case kHome:
            return KEY_HOME;
        case kEnd:
            return KEY_END;
        case kPageUp:
            return KEY_PAGE_UP;
        case kPageDown:
            return KEY_PAGE_DOWN;
        case kUp:
            return KEY_UP_ARROW;
        case kDown:
            return KEY_DOWN_ARROW;
        case kLeft:
            return KEY_LEFT_ARROW;
        case kRight:
            return KEY_RIGHT_ARROW;
        case kF1:
            return KEY_F1;
        case kF2:
            return KEY_F2;
        case kF3:
            return KEY_F3;
        case kF4:
            return KEY_F4;
        case kF5:
            return KEY_F5;
        case kF6:
            return KEY_F6;
        case kF7:
            return KEY_F7;
        case kF8:
            return KEY_F8;
        case kF9:
            return KEY_F9;
        case kF10:
            return KEY_F10;
        case kF11:
            return KEY_F11;
        case kF12:
            return KEY_F12;
        case kLeftCtrl:
            return KEY_LEFT_CTRL;
        case kLeftShift:
            return KEY_LEFT_SHIFT;
        case kLeftAlt:
            return KEY_LEFT_ALT;
        case kLeftGui:
            return KEY_LEFT_GUI;
        default:
            return 0;
    }
}

}  // namespace

bool begin() {
    if (g_begun) {
        return true;
    }
    const bool started = USB.begin();
    g_keyboard.begin();
    // Segna l'inizializzazione solo se e' riuscita: cosi' un tentativo fallito
    // non blocca tutti quelli successivi.
    g_begun = started;
    logger::info("BadUSB: USB HID inizializzato (%s)", started ? "ok" : "errore");
    return started;
}

bool connected() {
    return static_cast<bool>(USB);
}

void releaseAll() {
    g_keyboard.releaseAll();
}

bool press(uint8_t code) {
    const uint8_t key = usbCode(code);
    return key != 0 && g_keyboard.press(key) != 0;
}

bool release(uint8_t code) {
    const uint8_t key = usbCode(code);
    return key != 0 && g_keyboard.release(key) != 0;
}

bool tap(uint8_t code, uint32_t holdMs) {
    const uint8_t key = usbCode(code);
    if (key == 0) {
        return false;
    }
    const bool down = g_keyboard.press(key) != 0;
    delay(holdMs);
    const bool up = g_keyboard.release(key) != 0;
    return down && up;
}

}  // namespace hid
}  // namespace badusb

#endif  // ENABLE_MODULE_BADUSB
