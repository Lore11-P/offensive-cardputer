#pragma once

// Ricezione file da PC via USB CDC (porta seriale). Il device deve essere in
// questa schermata; i file vengono scritti sulla microSD.
namespace usb {

void runTransfer();

}  // namespace usb
