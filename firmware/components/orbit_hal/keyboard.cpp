#include "orbit_hal/keyboard.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_log.h>

#include <cstdio>

namespace orbit_hal {
namespace {

constexpr const char* kTag = "kbd";
constexpr uint8_t kAddress = 0x34;
constexpr int kSda = 8;
constexpr int kScl = 9;

// Registri TCA8418.
constexpr uint8_t kRegCfg = 0x01;
constexpr uint8_t kRegIntStat = 0x02;
constexpr uint8_t kRegKeyLckEc = 0x03;
constexpr uint8_t kRegKeyEventA = 0x04;
constexpr uint8_t kRegGpioIntStat1 = 0x11;
constexpr uint8_t kRegGpioIntEn1 = 0x1A;
constexpr uint8_t kRegKpGpio1 = 0x1D;
constexpr uint8_t kRegGpiEm1 = 0x20;
constexpr uint8_t kRegGpioDir1 = 0x23;
constexpr uint8_t kRegGpioIntLvl1 = 0x26;

i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_dev = nullptr;
bool s_shift = false;

// Mappa logica 4x14 (la stessa di M5Cardputer).
constexpr char kMap[4][14][2] = {
    {{'`', '~'}, {'1', '!'}, {'2', '@'}, {'3', '#'}, {'4', '$'}, {'5', '%'},
     {'6', '^'}, {'7', '&'}, {'8', '*'}, {'9', '('}, {'0', ')'}, {'-', '_'},
     {'=', '+'}, {'\b', '\b'}},
    {{'\t', '\t'}, {'q', 'Q'}, {'w', 'W'}, {'e', 'E'}, {'r', 'R'}, {'t', 'T'},
     {'y', 'Y'}, {'u', 'U'}, {'i', 'I'}, {'o', 'O'}, {'p', 'P'}, {'[', '{'},
     {']', '}'}, {'\\', '|'}},
    {{0, 0}, {0, 0}, {'a', 'A'}, {'s', 'S'}, {'d', 'D'}, {'f', 'F'}, {'g', 'G'},
     {'h', 'H'}, {'j', 'J'}, {'k', 'K'}, {'l', 'L'}, {';', ':'}, {'\'', '"'},
     {'\n', '\n'}},
    {{0, 0}, {0, 0}, {0, 0}, {'z', 'Z'}, {'x', 'X'}, {'c', 'C'}, {'v', 'V'},
     {'b', 'B'}, {'n', 'N'}, {'m', 'M'}, {',', '<'}, {'.', '>'}, {'/', '?'},
     {' ', ' '}},
};

bool writeReg(uint8_t reg, uint8_t value) {
    const uint8_t data[2] = {reg, value};
    return i2c_master_transmit(s_dev, data, sizeof(data), 50) == ESP_OK;
}

bool readReg(uint8_t reg, uint8_t* value) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, value, 1, 50) == ESP_OK;
}

bool initBus() {
    i2c_master_bus_config_t busConfig = {};
    busConfig.i2c_port = I2C_NUM_1;
    busConfig.sda_io_num = static_cast<gpio_num_t>(kSda);
    busConfig.scl_io_num = static_cast<gpio_num_t>(kScl);
    busConfig.clk_source = I2C_CLK_SRC_DEFAULT;
    busConfig.glitch_ignore_cnt = 7;
    busConfig.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&busConfig, &s_bus) != ESP_OK) {
        return false;
    }
    i2c_device_config_t devConfig = {};
    devConfig.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    devConfig.device_address = kAddress;
    devConfig.scl_speed_hz = 100000;
    return i2c_master_bus_add_device(s_bus, &devConfig, &s_dev) == ESP_OK;
}

void flush() {
    uint8_t value = 0;
    for (int i = 0; i < 16 && readReg(kRegKeyEventA, &value) && value != 0; ++i) {
    }
    readReg(kRegGpioIntStat1, &value);
    readReg(kRegGpioIntStat1 + 1, &value);
    readReg(kRegGpioIntStat1 + 2, &value);
    writeReg(kRegIntStat, 0x03);
}

// Handle per indirizzi I2C aggiuntivi sullo stesso bus (codec, IMU, ...).
i2c_master_dev_handle_t deviceFor(uint8_t address) {
    struct Entry {
        uint8_t address;
        i2c_master_dev_handle_t dev;
    };
    static Entry entries[6] = {};
    static int count = 0;
    for (int i = 0; i < count; ++i) {
        if (entries[i].address == address) {
            return entries[i].dev;
        }
    }
    if (s_bus == nullptr || count >= 6) {
        return nullptr;
    }
    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = address;
    config.scl_speed_hz = 100000;
    i2c_master_dev_handle_t dev = nullptr;
    if (i2c_master_bus_add_device(s_bus, &config, &dev) != ESP_OK) {
        return nullptr;
    }
    entries[count++] = {address, dev};
    return dev;
}

}  // namespace

bool i2cPresent(uint8_t address) {
    return s_bus != nullptr && i2c_master_probe(s_bus, address, 50) == ESP_OK;
}

bool i2cWrite(uint8_t address, const uint8_t* data, size_t len) {
    i2c_master_dev_handle_t dev = deviceFor(address);
    return dev != nullptr && i2c_master_transmit(dev, data, len, 100) == ESP_OK;
}

bool i2cWriteReg(uint8_t address, uint8_t reg, uint8_t value) {
    const uint8_t data[2] = {reg, value};
    return i2cWrite(address, data, sizeof(data));
}

bool i2cReadReg(uint8_t address, uint8_t reg, uint8_t* value) {
    i2c_master_dev_handle_t dev = deviceFor(address);
    return dev != nullptr && value != nullptr &&
           i2c_master_transmit_receive(dev, &reg, 1, value, 1, 100) == ESP_OK;
}

bool keyboardBegin() {
    if (!initBus()) {
        ESP_LOGE(kTag, "I2C non inizializzata");
        return false;
    }
    if (i2c_master_probe(s_bus, kAddress, 50) != ESP_OK) {
        ESP_LOGE(kTag, "TCA8418 non risponde a 0x%02x", kAddress);
        return false;
    }

    // INT del TCA8418 (GPIO11, open-drain attivo basso): solo diagnostica.
    gpio_config_t intCfg = {};
    intCfg.pin_bit_mask = 1ULL << 11;
    intCfg.mode = GPIO_MODE_INPUT;
    intCfg.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&intCfg);

    // Tutti i pin come input, eventi su tutti, interrupt su fronte di discesa.
    writeReg(kRegGpioDir1, 0x00);
    writeReg(kRegGpioDir1 + 1, 0x00);
    writeReg(kRegGpioDir1 + 2, 0x00);
    writeReg(kRegGpiEm1, 0xFF);
    writeReg(kRegGpiEm1 + 1, 0xFF);
    writeReg(kRegGpiEm1 + 2, 0xFF);
    writeReg(kRegGpioIntLvl1, 0x00);
    writeReg(kRegGpioIntLvl1 + 1, 0x00);
    writeReg(kRegGpioIntLvl1 + 2, 0x00);
    writeReg(kRegGpioIntEn1, 0xFF);
    writeReg(kRegGpioIntEn1 + 1, 0xFF);
    writeReg(kRegGpioIntEn1 + 2, 0xFF);

    // Matrice 7 righe x 8 colonne.
    writeReg(kRegKpGpio1, 0x7F);
    writeReg(kRegKpGpio1 + 1, 0xFF);
    writeReg(kRegKpGpio1 + 2, 0x00);
    flush();

    // Abilita key event e GPIO event come il driver M5Cardputer.
    uint8_t cfg = 0;
    if (readReg(kRegCfg, &cfg)) {
        if (!writeReg(kRegCfg, cfg | 0x03)) {
            ESP_LOGW(kTag, "scrittura CFG fallita");
        }
    }
    ESP_LOGI(kTag, "TCA8418 pronto");
    return true;
}

void keyboardDebug(char* out, size_t len) {
    if (out == nullptr || len == 0) {
        return;
    }
    uint8_t cfg = 0;
    uint8_t lck = 0;
    uint8_t kp1 = 0;
    uint8_t kp2 = 0;
    uint8_t kp3 = 0;
    uint8_t intStat = 0;
    readReg(kRegCfg, &cfg);
    readReg(kRegKeyLckEc, &lck);
    readReg(kRegKpGpio1, &kp1);
    readReg(kRegKpGpio1 + 1, &kp2);
    readReg(kRegKpGpio1 + 2, &kp3);
    readReg(kRegIntStat, &intStat);
    snprintf(out, len, "cfg=%02x lck=%02x kp=%02x/%02x/%02x int=%02x pin=%d", cfg, lck,
             kp1, kp2, kp3, intStat, gpio_get_level(GPIO_NUM_11));
}

bool keyboardPoll(KeyEvent* out) {
    if (out == nullptr || s_dev == nullptr) {
        return false;
    }
    uint8_t count = 0;
    if (!readReg(kRegKeyLckEc, &count)) {
        return false;
    }
    count &= 0x0F;
    if (count == 0) {
        return false;
    }
    uint8_t event = 0;
    if (!readReg(kRegKeyEventA, &event) || event == 0) {
        return false;
    }

    const bool pressed = (event & 0x80) != 0;
    uint8_t code = event & 0x7F;
    if (code == 0) {
        return false;
    }
    --code;
    const uint8_t rawRow = code / 10;
    const uint8_t rawCol = code % 10;

    // Remap alla mappa logica 4x14.
    const uint8_t row = static_cast<uint8_t>((rawCol + 4) % 4);
    const uint8_t col = static_cast<uint8_t>(rawRow * 2 + (rawCol > 3 ? 1 : 0));
    if (row >= 4 || col >= 14) {
        return false;
    }
    out->pressed = pressed;
    out->row = row;
    out->col = col;
    // Shift fisico (riga 2, colonna 1): cambia il carattere delle lettere.
    if (row == 2 && col == 1) {
        s_shift = pressed;
        out->key = 0;
        return true;
    }
    out->key = kMap[row][col][s_shift ? 1 : 0];
    return true;
}

}  // namespace orbit_hal
