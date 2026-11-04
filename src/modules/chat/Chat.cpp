#include "Chat.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "Config.h"
#include "Display.h"
#include "Logger.h"
#include "MenuManager.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "TextInput.h"
#include "Ui.h"

#if ENABLE_MODULE_CHAT && ENABLE_MODULE_BLE

namespace chat {
namespace {

constexpr uint8_t kMagic = 0xC7;
constexpr uint16_t kCompanyId = 0xFFFF;
constexpr size_t kNickLen = config::kChatNicknameMax;
constexpr size_t kTextLen = 19;
constexpr size_t kHeaderLen = 2;  // magic + contatore
constexpr size_t kAppLen = kHeaderLen + kNickLen + kTextLen;
constexpr size_t kHistoryMax = 8;
constexpr uint32_t kPeerTimeoutMs = 15000;
constexpr uint16_t kAdvInterval = 160;  // 100 ms (unita' da 0,625 ms)

struct ChatMessage {
    std::string sender;
    std::string text;
    bool mine = false;
};

std::vector<ChatMessage> g_messages;
std::map<std::string, uint8_t> g_lastCounter;
std::map<std::string, uint32_t> g_peers;
portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
volatile bool g_dirty = true;
uint8_t g_counter = 0;
std::string g_selfAddress;

void ensureBle() {
    if (!NimBLEDevice::getInitialized()) {
        NimBLEDevice::init("CardputerFW");
    }
}

std::string trimRight(std::string value) {
    while (!value.empty() && (value.back() == ' ' || value.back() == '\0')) {
        value.pop_back();
    }
    return value;
}

std::string printable(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c >= 0x20 && c < 0x7F) {
            out.push_back(c);
        }
    }
    return out;
}

bool parsePacket(const std::string& data, uint8_t& counter, std::string& sender,
                 std::string& text) {
    // company ID (2) + magic + contatore + nickname (6)
    if (data.size() < 2 + kHeaderLen + kNickLen) {
        return false;
    }
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(data.data());
    const uint16_t company = static_cast<uint16_t>(raw[0]) |
                             (static_cast<uint16_t>(raw[1]) << 8);
    if (company != kCompanyId) {
        return false;
    }

    const uint8_t* payload = raw + 2;
    const size_t len = data.size() - 2;
    if (payload[0] != kMagic) {
        return false;
    }
    counter = payload[1];
    sender.assign(reinterpret_cast<const char*>(payload + kHeaderLen), kNickLen);
    sender = printable(trimRight(sender));
    if (sender.empty()) {
        sender = "???";
    }

    const size_t textLen =
        std::min(len - kHeaderLen - kNickLen, static_cast<size_t>(kTextLen));
    text.assign(reinterpret_cast<const char*>(payload + kHeaderLen + kNickLen),
                textLen);
    text = printable(trimRight(text));
    return !text.empty();
}

std::vector<uint8_t> buildPacket(const std::string& text, uint8_t counter) {
    std::vector<uint8_t> data;
    data.reserve(2 + kAppLen);

    data.push_back(static_cast<uint8_t>(kCompanyId & 0xFF));
    data.push_back(static_cast<uint8_t>((kCompanyId >> 8) & 0xFF));
    data.push_back(kMagic);
    data.push_back(counter);

    std::string nickname = config::chatNickname();
    if (nickname.size() > kNickLen) {
        nickname.resize(kNickLen);
    }
    data.insert(data.end(), nickname.begin(), nickname.end());
    data.insert(data.end(), kNickLen - nickname.size(), ' ');

    std::string body = text;
    if (body.size() > kTextLen) {
        body.resize(kTextLen);
    }
    data.insert(data.end(), body.begin(), body.end());
    data.insert(data.end(), kTextLen - body.size(), ' ');
    return data;
}

class ChatScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
public:
    void onResult(NimBLEAdvertisedDevice* device) override {
        if (device == nullptr || !device->haveManufacturerData()) {
            return;
        }

        const std::string data = device->getManufacturerData(0);
        uint8_t counter = 0;
        std::string sender;
        std::string text;
        if (!parsePacket(data, counter, sender, text)) {
            return;
        }

        const std::string address = device->getAddress().toString();
        if (address == g_selfAddress) {
            return;
        }

        const uint32_t now = millis();
        bool duplicate = false;
        portENTER_CRITICAL(&g_lock);
        g_peers[address] = now;
        auto it = g_lastCounter.find(address);
        if (it != g_lastCounter.end() && it->second == counter) {
            duplicate = true;
        } else {
            g_lastCounter[address] = counter;
            ChatMessage message;
            message.sender = sender;
            message.text = text;
            g_messages.push_back(std::move(message));
            if (g_messages.size() > kHistoryMax) {
                g_messages.erase(g_messages.begin());
            }
            g_dirty = true;
        }
        portEXIT_CRITICAL(&g_lock);

        if (!duplicate) {
            logger::info("Chat BLE: %s: %s", sender.c_str(), text.c_str());
        }
    }
};

void addLocalMessage(const std::string& text) {
    ChatMessage message;
    message.sender = config::chatNickname();
    message.text = text;
    message.mine = true;

    portENTER_CRITICAL(&g_lock);
    g_messages.push_back(std::move(message));
    if (g_messages.size() > kHistoryMax) {
        g_messages.erase(g_messages.begin());
    }
    g_dirty = true;
    portEXIT_CRITICAL(&g_lock);
}

void publishMessage(const std::string& text) {
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    if (advertising == nullptr) {
        return;
    }

    if (advertising->isAdvertising()) {
        advertising->stop();
        delay(5);
    }

    const uint8_t counter = g_counter++;
    const std::vector<uint8_t> payload = buildPacket(text, counter);
    NimBLEAdvertisementData advertisement;
    advertisement.setManufacturerData(payload);

    advertising->setAdvertisementType(BLE_GAP_CONN_MODE_NON);
    advertising->setScanResponse(false);
    advertising->setMinInterval(kAdvInterval);
    advertising->setMaxInterval(kAdvInterval);
    advertising->setAdvertisementData(advertisement);
    if (!advertising->start(0, nullptr, nullptr)) {
        logger::warn("Chat BLE: avvio advertising fallito");
    }
}

void drawChat() {
    g_dirty = false;

    std::vector<ChatMessage> messages;
    size_t peers = 0;

    portENTER_CRITICAL(&g_lock);
    messages = g_messages;
    const uint32_t now = millis();
    for (auto it = g_peers.begin(); it != g_peers.end();) {
        if (now - it->second > kPeerTimeoutMs) {
            it = g_peers.erase(it);
        } else {
            ++peers;
            ++it;
        }
    }
    portEXIT_CRITICAL(&g_lock);

    display::clear();
    display::header("Chat BLE", "Menu principale");

    std::string status = "Tu: " + config::chatNickname();
    if (peers > 0) {
        status += "  vicini: " + std::to_string(peers);
    } else {
        status += "  in ascolto";
    }
    display::text(4, 24, status, display::kTextDim, 0);

    if (messages.empty()) {
        display::textCentered(52, "Nessun messaggio", display::kText, 2);
        display::textCentered(70, "ENTER = scrivi", display::kTextDim, 0);
    } else {
        constexpr int kLineHeight = 10;
        constexpr int kFirstLineY = 38;
        const int maxLines = (display::height() - 12 - kFirstLineY) / kLineHeight;
        const size_t start = messages.size() > static_cast<size_t>(maxLines)
                                 ? messages.size() - static_cast<size_t>(maxLines)
                                 : 0;
        int y = kFirstLineY;
        for (size_t i = start; i < messages.size(); ++i) {
            const ChatMessage& message = messages[i];
            std::string line =
                (message.mine ? std::string("Tu") : message.sender) + ": " +
                message.text;
            if (line.size() > 39) {
                line.resize(38);
                line += "~";
            }
            display::text(4, y, line,
                          message.mine ? display::kAccent : display::kText, 0);
            y += kLineHeight;
        }
    }

    display::footer("ENTER=scrivi  `=esci");
    display::push();
}

void composeMessage() {
    std::vector<textinput::Field> fields;
    fields.push_back({"Messaggio", "", kTextLen, false, false});
    if (!textinput::edit("Nuovo messaggio", "Chat BLE", fields)) {
        return;
    }
    if (fields[0].value.empty()) {
        return;
    }
    addLocalMessage(fields[0].value);
    publishMessage(fields[0].value);
}

void editNickname() {
    std::vector<textinput::Field> fields;
    fields.push_back({"Nickname", config::chatNickname(),
                      config::kChatNicknameMax, false, false});
    if (!textinput::edit("Nickname chat", "Chat", fields)) {
        return;
    }
    config::setChatNickname(fields[0].value);
    ui::message("Nickname chat",
                {">Nickname: " + config::chatNickname()}, "Chat");
}

void runChat() {
    ensureBle();
    // Usa sempre l'indirizzo pubblico: evita che un modulo precedente
    // (es. BLE spam) lasci attivo un MAC random non coerente con getAddress().
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);

    NimBLEScan* scan = NimBLEDevice::getScan();
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    if (scan == nullptr || advertising == nullptr) {
        ui::message("Chat BLE",
                    {"!Stack BLE non disponibile.", "Riavvia il dispositivo."},
                    "Chat");
        return;
    }

    g_selfAddress = NimBLEDevice::getAddress().toString();
    g_counter = static_cast<uint8_t>(esp_random());
    g_peers.clear();
    g_dirty = true;

    if (scan->isScanning()) {
        scan->stop();
    }
    if (advertising->isAdvertising()) {
        advertising->stop();
    }

    ChatScanCallbacks callbacks;
    scan->setAdvertisedDeviceCallbacks(&callbacks, true);
    scan->setActiveScan(false);
    scan->setInterval(50);
    scan->setWindow(50);
    scan->setMaxResults(0);
    scan->clearResults();

    if (!scan->start(0, nullptr, false)) {
        scan->setAdvertisedDeviceCallbacks(nullptr, false);
        ui::message("Chat BLE",
                    {"!Scansione BLE non avviata.",
                     "Riprova tra qualche secondo."},
                    "Chat");
        return;
    }

    logger::info("Chat BLE: in ascolto come %s", config::chatNickname().c_str());

    navigation::waitRelease();
    bool running = true;
    uint32_t lastDraw = 0;
    while (running) {
        navigation::update();
        const navigation::EventData event = navigation::poll();

        if (event.type == navigation::Event::Select) {
            navigation::waitRelease();
            composeMessage();
            g_dirty = true;
            navigation::waitRelease();
        } else if (event.type == navigation::Event::Back) {
            running = false;
        }

        const uint32_t now = millis();
        if (g_dirty || now - lastDraw > 1000) {
            drawChat();
            lastDraw = now;
        }
        delay(25);
    }

    if (scan->isScanning()) {
        scan->stop();
    }
    scan->setAdvertisedDeviceCallbacks(nullptr, false);
    scan->clearResults();
    if (advertising->isAdvertising()) {
        advertising->stop();
    }
    navigation::waitRelease();
    logger::info("Chat BLE: chiusa");
}

}  // namespace

void runMenu() {
    MenuManager menu("Chat", "Menu principale");
    menu.addItem("Apri chat", "chat", runChat);
    menu.addItem("Nickname", "settings", editNickname);
    menu.addItem("Info", "info", [] {
        ui::message("Chat BLE",
                    {">Messaggi broadcast via BLE.",
                     "Apri Chat su piu' Cardputer vicini.",
                     "Max 19 caratteri per messaggio.",
                     "Nessun pairing: non usare per dati sensibili."},
                    "Chat");
    });
    menu.setHint("`=indietro  ;.=nav  ENTER=apri");
    menu.run(true);
}

}  // namespace chat

#endif  // ENABLE_MODULE_CHAT && ENABLE_MODULE_BLE
