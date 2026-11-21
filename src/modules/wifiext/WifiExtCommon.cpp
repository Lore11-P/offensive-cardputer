#include "WifiExtCommon.h"

#include <Arduino.h>
#include <SD.h>

#include <cstdio>
#include <cstring>

#include "ModuleFlags.h"
#include "Storage.h"

#if ENABLE_MODULE_WIFIEXT

namespace wifiext {

std::string shorten(const std::string& text, size_t maxLen) {
    if (text.size() <= maxLen || maxLen < 2) {
        return text;
    }
    return text.substr(0, maxLen - 1) + "~";
}

std::string trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' ||
                           text[begin] == '\r' || text[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\r' || text[end - 1] == '\n')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string hexBytes(const uint8_t* data, size_t len) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

std::string sdFreeText() {
    if (!storage::hasSD()) {
        return "SD assente";
    }
    const uint64_t total = SD.totalBytes();
    const uint64_t used = SD.usedBytes();
    if (total == 0) {
        return "SD non pronta";
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%llu MB liberi",
                  static_cast<unsigned long long>((total - used) / (1024ULL * 1024ULL)));
    return text;
}

bool ensureDir() {
    if (!storage::hasSD()) {
        return false;
    }
    storage::mkdir("/wifiext", true);
    return true;
}

bool appendSd(const char* path, const std::string& text) {
    if (!ensureDir()) {
        return false;
    }
    File file;
    if (SD.exists(path)) {
        file = SD.open(path, FILE_APPEND);
    } else {
        file = SD.open(path, FILE_WRITE);
    }
    if (!file) {
        return false;
    }
    const size_t written =
        file.write(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    file.flush();
    const bool ok = written == text.size() && file.getWriteError() == 0;
    file.close();
    return ok;
}

}  // namespace wifiext

#endif  // ENABLE_MODULE_WIFIEXT
