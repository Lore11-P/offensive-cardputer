#pragma once

// BadUSB / HID injection: esegue script Ducky/Flipper da /badusb sulla SD.
// Compilato solo nell'env con TinyUSB (ENABLE_MODULE_BADUSB=1).
namespace badusb {

void runMenu();

}  // namespace badusb
