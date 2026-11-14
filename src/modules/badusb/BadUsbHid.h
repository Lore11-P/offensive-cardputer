#pragma once

#include <cstdint>

#include "ModuleFlags.h"

#if ENABLE_MODULE_BADUSB

// Interfaccia minima verso la tastiera USB HID (TinyUSB).
namespace badusb {
namespace hid {

// Tasti speciali. I valori sono interni al modulo: non sono i codici KEY_*
// dell'header USB, che collidono con quelli definiti da M5Cardputer.
enum Special : uint8_t {
    kEnter = 0x80,
    kTab,
    kEsc,
    kBackspace,
    kDelete,
    kCapsLock,
    kInsert,
    kHome,
    kEnd,
    kPageUp,
    kPageDown,
    kUp,
    kDown,
    kLeft,
    kRight,
    kF1,
    kF2,
    kF3,
    kF4,
    kF5,
    kF6,
    kF7,
    kF8,
    kF9,
    kF10,
    kF11,
    kF12,
    kLeftCtrl,
    kLeftShift,
    kLeftAlt,
    kLeftGui
};

// Prepara USB e tastiera alla prima chiamata; idempotente.
bool begin();

// True se un host ha enumerato il dispositivo.
bool connected();

// Rilascia modificatori e tasti premuti.
void releaseAll();

// `code`: carattere ASCII 0x20..0x7E oppure uno dei valori Special.
bool press(uint8_t code);
bool release(uint8_t code);

// Battuta breve: press, attesa, release.
bool tap(uint8_t code, uint32_t holdMs);

}  // namespace hid
}  // namespace badusb

#endif  // ENABLE_MODULE_BADUSB
