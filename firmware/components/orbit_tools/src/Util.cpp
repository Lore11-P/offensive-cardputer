#include "ToolsInternal.h"

#include <cstdio>
#include <cstring>

#include <esp_err.h>
#include <nvs.h>

#include "Display.h"
#include "orbit_compat.h"

namespace orbit_tools {
namespace {

constexpr int kCharsPerLine = 32;

std::string printable(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        out.push_back(u >= 0x20 && u < 0x7F ? c : ' ');
    }
    return out;
}

}  // namespace

bool nvsReadString(const char* key, std::string& out) {
    out.clear();
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    char buffer[128] = {};
    size_t length = sizeof(buffer);
    const esp_err_t err = nvs_get_str(handle, key, buffer, &length);
    nvs_close(handle);
    if (err != ESP_OK) {
        return false;
    }
    out.assign(buffer);
    return true;
}

bool nvsWriteString(const char* key, const std::string& value) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_str(handle, key, value.c_str());
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

bool nvsEraseKey(const char* key) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_erase_key(handle, key);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
}

char eventChar(const navigation::EventData& ev) {
    switch (ev.type) {
        case navigation::Event::Text:
            return ev.ch;
        case navigation::Event::Up:
            return ';';
        case navigation::Event::Down:
            return '.';
        case navigation::Event::Left:
            return ',';
        case navigation::Event::Right:
            return '/';
        default:
            return 0;
    }
}

void refreshHeader(const std::string& title, const std::string& parent, uint16_t& lastBattery) {
    const uint16_t battery = display::batterySignature();
    if (battery == lastBattery) {
        return;
    }
    lastBattery = battery;
    display::header(title, parent);
    display::push(0, 0, display::width(), 22);
}

std::string asciiSnippet(const std::string& text, size_t maxChars) {
    std::string out = printable(text);
    if (maxChars < 3 || out.size() <= maxChars) {
        return out;
    }
    out.resize(maxChars - 2);
    out += "..";
    return out;
}

bool textInput(const std::string& title, const std::string& parent, const std::string& prompt,
               std::string& value, size_t maxLen) {
    if (value.size() > maxLen) {
        value.resize(maxLen);
    }
    navigation::waitRelease();

    bool dirty = true;
    uint16_t lastBattery = 0;
    uint32_t lastStatus = millis();
    for (;;) {
        if (dirty) {
            dirty = false;
            display::clear(display::C(display::T_bg));
            display::header(title, parent);
            display::text(8, 28, prompt, display::C(display::T_dim), 0);
            char counter[16];
            std::snprintf(counter, sizeof(counter), "%u/%u", static_cast<unsigned>(value.size()),
                          static_cast<unsigned>(maxLen));
            display::text(display::width() - 8 - display::textWidth(counter, 0), 28, counter,
                          display::C(display::T_dim), 0);
            display::g().drawFastHLine(8, 39, display::width() - 16,
                                       display::C(display::T_line));

            // Cursore in coda; il valore e' mandato a capo ogni 32 caratteri.
            std::string shown = printable(value);
            shown.push_back('_');
            for (int line = 0; line < 3; ++line) {
                const size_t pos = static_cast<size_t>(line) * kCharsPerLine;
                if (pos >= shown.size()) {
                    break;
                }
                display::text(8, 46 + line * 12, shown.substr(pos, kCharsPerLine),
                              display::C(display::T_text), 2);
            }

            display::text(8, 90, "INVIO=conferma", display::C(display::T_dim), 0);
            display::text(8, 102, "BKSP=cancella   `=annulla", display::C(display::T_dim), 0);
            display::footer("INVIO=ok  `=annulla");
            display::push();
            lastBattery = display::batterySignature();
        }

        const uint32_t now = millis();
        if (now - lastStatus >= 500) {
            lastStatus = now;
            refreshHeader(title, parent, lastBattery);
        }

        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Select) {
            navigation::waitRelease();
            return true;
        }
        if (ev.type == navigation::Event::Back) {
            if (navigation::backtickPressed()) {
                navigation::waitRelease();
                return false;
            }
            if (!value.empty()) {
                value.pop_back();
                dirty = true;
            }
            continue;
        }
        const char c = eventChar(ev);
        if (c >= 0x20 && c < 0x7F && value.size() < maxLen) {
            value.push_back(c);
            dirty = true;
        }
        delay(15);
    }
}

}  // namespace orbit_tools
