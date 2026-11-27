#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "Display.h"
#include "Navigation.h"
#include "ToolsInternal.h"
#include "Ui.h"
#include "lgfx_qrcode.h"
#include "orbit_compat.h"

namespace orbit_tools {
namespace {

constexpr size_t kQrTextMax = 78;
constexpr size_t kQrMaxVersion = 4;

// Capacita' in byte con ECC_LOW per le versioni 1..4. La libreria non
// rifiuta un testo troppo lungo: la versione va scelta qui.
constexpr size_t kByteCapacity[kQrMaxVersion] = {17, 32, 53, 78};

int chooseVersion(size_t length) {
    for (size_t v = 0; v < kQrMaxVersion; ++v) {
        if (length <= kByteCapacity[v]) {
            return static_cast<int>(v) + 1;
        }
    }
    return 0;
}

void drawQr(QRCode& qr, int version) {
    display::clear(display::C(display::T_bg));
    display::header("QR code", "Strumenti");

    const int modules = qr.size;
    const int total = modules + 8;  // quiet zone di 4 moduli per lato
    const int top = 23;
    const int bottom = display::height() - 12;
    const int availableW = display::width() - 8;
    const int availableH = bottom - top;
    int scale = std::min(availableW / total, availableH / total);
    if (scale < 1) {
        scale = 1;
    }
    const int pixels = total * scale;
    const int ox = (display::width() - pixels) / 2;
    const int oy = top + (availableH - pixels) / 2;

    auto& g = display::g();
    g.fillRect(ox, oy, pixels, pixels, 0xFFFF);
    for (int y = 0; y < modules; ++y) {
        for (int x = 0; x < modules; ++x) {
            if (lgfx_qrcode_getModule(&qr, x, y)) {
                g.fillRect(ox + (x + 4) * scale, oy + (y + 4) * scale, scale, scale, 0x0000);
            }
        }
    }

    char info[16];
    std::snprintf(info, sizeof(info), "V%d", version);
    display::footer("ESC=esci", info);
    display::push();
}

}  // namespace

void qrScreen() {
    std::string text;
    if (!textInput("QR code", "Strumenti", "Testo o URL (max 78)", text, kQrTextMax) ||
        text.empty()) {
        return;
    }

    const int version = chooseVersion(text.size());
    if (version == 0) {
        ui::message("QR code", {"!Testo troppo lungo.", "Massimo 78 byte."}, "Strumenti");
        return;
    }

    // Il buffer vive solo durante la generazione e il disegno: viene liberato
    // prima di entrare nel loop di attesa.
    {
        std::vector<uint8_t> modules(lgfx_qrcode_getBufferSize(static_cast<uint8_t>(version)));
        QRCode qr = {};
        if (lgfx_qrcode_initText(&qr, modules.data(), static_cast<uint8_t>(version), ECC_LOW,
                                 text.c_str()) != 0) {
            ui::message("QR code", {"!Generazione non riuscita.", "Testo non codificabile."},
                        "Strumenti");
            return;
        }
        drawQr(qr, version);
    }

    navigation::waitRelease();
    uint16_t lastBattery = display::batterySignature();
    uint32_t lastStatus = millis();
    for (;;) {
        const uint32_t now = millis();
        if (now - lastStatus >= 500) {
            lastStatus = now;
            refreshHeader("QR code", "Strumenti", lastBattery);
        }
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        }
        delay(20);
    }
    navigation::waitRelease();
}

}  // namespace orbit_tools
