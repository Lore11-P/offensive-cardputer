#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include <mbedtls/md.h>

#include "Base32.h"
#include "Display.h"
#include "MenuManager.h"
#include "Navigation.h"
#include "ToolsInternal.h"
#include "Ui.h"
#include "orbit_compat.h"

namespace orbit_tools {
namespace {

constexpr time_t kEpoch2020 = 1577836800;
constexpr int kPeriod = 30;
constexpr size_t kSecretMax = 64;
constexpr size_t kKeyMax = 64;
constexpr const char* kTotpKey = "totp";

bool hotpCode(const uint8_t* key, size_t keyLen, uint64_t counter, uint32_t& code) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (info == nullptr) {
        return false;
    }
    uint8_t message[8];
    for (int i = 7; i >= 0; --i) {
        message[i] = static_cast<uint8_t>(counter & 0xFF);
        counter >>= 8;
    }
    uint8_t digest[20];
    if (mbedtls_md_hmac(info, key, keyLen, message, sizeof(message), digest) != 0) {
        return false;
    }
    const int offset = digest[19] & 0x0F;
    const uint32_t binary =
        (static_cast<uint32_t>(digest[offset] & 0x7F) << 24) |
        (static_cast<uint32_t>(digest[offset + 1]) << 16) |
        (static_cast<uint32_t>(digest[offset + 2]) << 8) |
        static_cast<uint32_t>(digest[offset + 3]);
    code = binary % 1000000u;
    return true;
}

void drawTotp(bool timeValid, bool secretValid, bool haveCode, uint32_t code, int remaining) {
    display::clear(display::C(display::T_bg));
    display::header("TOTP", "Strumenti");

    if (!timeValid) {
        display::textCentered(40, "OROLOGIO NON", display::C(display::T_warn), 2);
        display::textCentered(53, "SINCRONIZZATO", display::C(display::T_warn), 2);
        display::textCentered(74, "NTP non disponibile", display::C(display::T_dim), 0);
    } else if (!secretValid) {
        display::textCentered(46, "SEGRETO NON VALIDO", display::C(display::T_err), 2);
        display::textCentered(66, "Riscrivilo con M", display::C(display::T_dim), 0);
    } else if (!haveCode) {
        display::textCentered(46, "ERRORE HMAC", display::C(display::T_err), 2);
        display::textCentered(66, "Riprova o cambia segreto", display::C(display::T_dim), 0);
    } else {
        char codeText[16];
        std::snprintf(codeText, sizeof(codeText), "%06u", static_cast<unsigned>(code));
        display::textCentered(29, codeText, display::C(display::T_acc), 4);

        char remainingText[32];
        std::snprintf(remainingText, sizeof(remainingText), "%d secondi rimanenti", remaining);
        display::textCentered(54, remainingText, display::C(display::T_text), 0);
        display::progressBar(20, 72, display::width() - 40, 6,
                             static_cast<float>(remaining) / static_cast<float>(kPeriod),
                             display::C(display::T_acc));
        display::textCentered(88, "Codice a 6 cifre, 30 s", display::C(display::T_dim), 0);
        display::textCentered(102, "M = cambia segreto", display::C(display::T_dim), 0);
    }

    display::footer("ESC=esci  M=cambia");
    display::push();
}

enum class TotpResult { Back, Change };

TotpResult totpDisplay(const std::string& secret) {
    uint8_t key[kKeyMax] = {};
    size_t keyLen = 0;
    const bool decoded = base32Decode(secret, key, sizeof(key), keyLen);
    bool haveCode = false;
    uint32_t code = 0;
    int64_t counter = -1;
    uint32_t lastDraw = 0;
    int lastRemaining = -1;

    navigation::waitRelease();
    for (;;) {
        const time_t now = time(nullptr);
        const bool valid = now >= kEpoch2020;
        int remaining = 0;
        if (valid) {
            remaining = kPeriod - static_cast<int>(now % kPeriod);
            const int64_t step = static_cast<int64_t>(now) / kPeriod;
            if (step != counter) {
                counter = step;
                haveCode = decoded && hotpCode(key, keyLen, static_cast<uint64_t>(step), code);
            }
        } else {
            counter = -1;
            haveCode = false;
        }

        const uint32_t nowMs = millis();
        if (lastDraw == 0 || nowMs - lastDraw >= 250 || remaining != lastRemaining) {
            lastDraw = nowMs;
            lastRemaining = remaining;
            drawTotp(valid, decoded, haveCode, code, remaining);
        }

        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
            break;
        }
        if (ev.type == navigation::Event::Text && (ev.ch == 'm' || ev.ch == 'M')) {
            std::memset(key, 0, sizeof(key));
            navigation::waitRelease();
            return TotpResult::Change;
        }
        delay(20);
    }

    std::memset(key, 0, sizeof(key));
    navigation::waitRelease();
    return TotpResult::Back;
}

bool totpSecretInput(std::string& secret) {
    if (!textInput("Segreto TOTP", "TOTP", "Base32: A-Z e 2-7", secret, kSecretMax)) {
        return false;
    }
    if (secret.empty()) {
        return false;
    }
    if (!base32Valid(secret)) {
        ui::message("TOTP", {"!Segreto Base32 non valido.", "Usa solo A-Z, 2-7 e padding =."},
                    "TOTP");
        return false;
    }
    return true;
}

void totpShow() {
    std::string secret;
    if (!nvsReadString(kTotpKey, secret) || secret.empty()) {
        if (!totpSecretInput(secret)) {
            return;
        }
        if (!nvsWriteString(kTotpKey, secret)) {
            ui::message("TOTP", {"!Salvataggio non riuscito.", "NVS non disponibile."}, "TOTP");
            return;
        }
    }

    for (;;) {
        const TotpResult result = totpDisplay(secret);
        if (result != TotpResult::Change) {
            break;
        }
        std::string next = secret;
        if (!totpSecretInput(next)) {
            break;
        }
        if (!nvsWriteString(kTotpKey, next)) {
            ui::message("TOTP", {"!Salvataggio non riuscito.", "NVS non disponibile."}, "TOTP");
            break;
        }
        secret = next;
    }
}

void totpSetSecret() {
    std::string secret;
    nvsReadString(kTotpKey, secret);
    if (!totpSecretInput(secret)) {
        return;
    }
    if (!nvsWriteString(kTotpKey, secret)) {
        ui::message("TOTP", {"!Salvataggio non riuscito.", "NVS non disponibile."}, "TOTP");
        return;
    }
    ui::message("TOTP", {"Segreto salvato in NVS.", "Conservato in chiaro."}, "TOTP");
}

void totpRemoveSecret() {
    std::string secret;
    if (!nvsReadString(kTotpKey, secret) || secret.empty()) {
        ui::message("TOTP", {"Nessun segreto salvato."}, "TOTP");
        return;
    }
    if (!ui::confirm("TOTP", {"Rimuovere il segreto salvato?"}, "TOTP")) {
        return;
    }
    if (!nvsEraseKey(kTotpKey)) {
        ui::message("TOTP", {"!Rimozione non riuscita.", "NVS non disponibile."}, "TOTP");
        return;
    }
    ui::message("TOTP", {"Segreto rimosso."}, "TOTP");
}

}  // namespace

void totpMenu() {
    MenuManager menu("TOTP", "Strumenti");
    std::function<void()> refresh;
    refresh = [&menu, &refresh]() {
        std::string secret;
        const bool stored = nvsReadString(kTotpKey, secret) && !secret.empty();
        std::vector<MenuItem> items;
        items.emplace_back("Mostra codice", "key", [&refresh] {
            totpShow();
            refresh();
        });
        items.emplace_back(stored ? "Modifica segreto" : "Imposta segreto", "key",
                           [&refresh] {
                               totpSetSecret();
                               refresh();
                           });
        if (stored) {
            items.emplace_back("Rimuovi segreto", "key", [&refresh] {
                totpRemoveSecret();
                refresh();
            });
        }
        menu.setItems(std::move(items));
    };
    refresh();
    menu.setHint("ENTER=apri  `=indietro");
    menu.run(true);
}

}  // namespace orbit_tools
