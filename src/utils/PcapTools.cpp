#include "PcapTools.h"

#include <Arduino.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

#include "Storage.h"

namespace pcaptools {
namespace {

constexpr uint16_t kLinkIeee80211 = 105;
constexpr uint16_t kLinkRadiotap = 127;
constexpr size_t kPcapGlobalSize = 24;
constexpr size_t kPcapRecordSize = 16;
constexpr size_t kMaxSsids = 128;
constexpr uint16_t kMaxLiveSsids = 48;
constexpr size_t kMaxNonces = 64;
constexpr size_t kMaxPending = 32;

// ---------------------------------------------------------------------------
// Lettura/scrittura numeri
// ---------------------------------------------------------------------------

uint16_t readLe16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t readLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t readBe16(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t read32(const uint8_t* p, bool be) {
  if (!be) {
    return readLe32(p);
  }
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint64_t readBe64(const uint8_t* p) {
  return (static_cast<uint64_t>(read32(p, true)) << 32) | read32(p + 4, true);
}

void write32(uint8_t* p, uint32_t v, bool be) {
  if (be) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
  } else {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
  }
}

// ---------------------------------------------------------------------------
// Formattazione hashcat
// ---------------------------------------------------------------------------

void appendHex(std::string& out, const uint8_t* data, size_t len) {
  static const char kHex[] = "0123456789abcdef";
  if (data == nullptr || len == 0) {
    return;
  }
  out.reserve(out.size() + len * 2);
  for (size_t i = 0; i < len; ++i) {
    out.push_back(kHex[data[i] >> 4]);
    out.push_back(kHex[data[i] & 0x0F]);
  }
}

std::string formatPmkid(const uint8_t pmkid[16], const uint8_t bssid[6],
                        const uint8_t sta[6], const uint8_t* ssid, uint8_t ssidLen) {
  std::string out;
  out.reserve(96 + static_cast<size_t>(ssidLen) * 2);
  out += "WPA*01*";
  appendHex(out, pmkid, 16);
  out += '*';
  appendHex(out, bssid, 6);
  out += '*';
  appendHex(out, sta, 6);
  out += '*';
  appendHex(out, ssid, ssidLen);
  out += "***";
  return out;
}

std::string formatEapol(const uint8_t mic[16], const uint8_t bssid[6],
                        const uint8_t sta[6], const uint8_t* ssid, uint8_t ssidLen,
                        const uint8_t nonceAp[32], const uint8_t* eapol,
                        size_t eapolLen, uint8_t messagePair) {
  std::string out;
  out.reserve(128 + static_cast<size_t>(ssidLen) * 2 + eapolLen * 2);
  out += "WPA*02*";
  appendHex(out, mic, 16);
  out += '*';
  appendHex(out, bssid, 6);
  out += '*';
  appendHex(out, sta, 6);
  out += '*';
  appendHex(out, ssid, ssidLen);
  out += '*';
  appendHex(out, nonceAp, 32);
  out += '*';
  appendHex(out, eapol, eapolLen);
  out += '*';
  char pair[3];
  std::snprintf(pair, sizeof(pair), "%02x", static_cast<unsigned>(messagePair));
  out += pair;
  return out;
}

class LineWriter {
 public:
  enum class Result { Written, Duplicate, Full, Error };

  explicit LineWriter(File& file) : file_(file) {
    // 4 KB di dedup su heap: il chiamante gira su stack di task da 16 KB.
    hashes_.reset(new (std::nothrow) uint64_t[kMaxHcLines]);
    if (!hashes_) {
      failed_ = true;
    }
  }

  LineWriter(const LineWriter&) = delete;
  LineWriter& operator=(const LineWriter&) = delete;

  Result write(const std::string& line) {
    if (failed_) {
      return Result::Error;
    }
    const uint64_t hash = fnv1a(line);
    for (uint16_t i = 0; i < count_; ++i) {
      if (hashes_[i] == hash) {
        return Result::Duplicate;
      }
    }
    if (lines_ >= kMaxHcLines) {
      return Result::Full;
    }
    const size_t written =
        file_.write(reinterpret_cast<const uint8_t*>(line.data()), line.size());
    const size_t newline = file_.write(reinterpret_cast<const uint8_t*>("\n"), 1);
    if (written != line.size() || newline != 1) {
      failed_ = true;
      return Result::Error;
    }
    hashes_[count_++] = hash;
    ++lines_;
    return Result::Written;
  }

  bool failed() const { return failed_; }
  uint32_t lines() const { return lines_; }

 private:
  static uint64_t fnv1a(const std::string& text) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : text) {
      hash ^= c;
      hash *= 1099511628211ull;
    }
    return hash;
  }

  File& file_;
  std::unique_ptr<uint64_t[]> hashes_;
  uint16_t count_ = 0;
  uint32_t lines_ = 0;
  bool failed_ = false;
};

// ---------------------------------------------------------------------------
// Radiotap
// ---------------------------------------------------------------------------

struct RadiotapInfo {
  bool valid = false;
  bool fcs = false;
  bool dataPad = false;
  size_t len = 0;
  int8_t rssi = 0;
  uint8_t channel = 0;
};

bool radiotapField(int bit, size_t& align, size_t& size) {
  switch (bit) {
    case 0:  // TSFT
      align = 8;
      size = 8;
      return true;
    case 1:  // Flags
    case 2:  // Rate
    case 5:  // dBm antenna signal
    case 6:  // dBm antenna noise
    case 10:  // dBm TX power
    case 11:  // Antenna
    case 12:  // dB antenna signal
    case 13:  // dB antenna noise
    case 16:  // RTS retries
    case 17:  // Data retries
      align = 1;
      size = 1;
      return true;
    case 3:  // Channel
      align = 2;
      size = 4;
      return true;
    case 4:  // FHSS
      align = 2;
      size = 3;
      return true;
    case 7:  // Lock quality
    case 8:  // TX attenuation
    case 9:  // dB TX attenuation
    case 14:  // RX flags
    case 15:  // TX flags
      align = 2;
      size = 2;
      return true;
    case 18:  // XChannel
      align = 4;
      size = 8;
      return true;
    case 19:  // MCS
      align = 1;
      size = 3;
      return true;
    case 20:  // A-MPDU status
      align = 4;
      size = 8;
      return true;
    case 21:  // VHT
      align = 2;
      size = 12;
      return true;
    default:
      return false;
  }
}

uint8_t channelFromFreq(uint16_t freq) {
  if (freq == 2484) {
    return 14;
  }
  if (freq >= 2412 && freq <= 2472) {
    return static_cast<uint8_t>((freq - 2407) / 5);
  }
  if (freq >= 5000 && freq <= 5895) {
    return static_cast<uint8_t>((freq - 5000) / 5);
  }
  if (freq >= 5955 && freq <= 7115) {
    return static_cast<uint8_t>((freq - 5950) / 5);
  }
  return 0;
}

// Lunghezza dichiarata dal radiotap, senza interpretare i campi. Serve per
// saltare l'header anche quando contiene campi che non conosciamo.
size_t radiotapLength(const uint8_t* d, size_t n) {
  if (n < 8 || d[0] != 0 || d[1] != 0) {
    return 0;
  }
  const size_t len = readLe16(d + 2);
  if (len < 8 || len > n || len > 512) {
    return 0;
  }
  return len;
}

// Il radiotap e' sempre little-endian, indipendentemente dal PCAP.
bool parseRadiotap(const uint8_t* d, size_t n, RadiotapInfo& out) {
  out = RadiotapInfo{};
  if (n < 8 || d[0] != 0 || d[1] != 0) {
    return false;
  }
  const size_t len = readLe16(d + 2);
  if (len < 8 || len > n || len > 512) {
    return false;
  }

  uint32_t present[4] = {};
  size_t words = 0;
  size_t pos = 4;
  for (;;) {
    if (pos + 4 > len || words >= 4) {
      return false;
    }
    present[words] = readLe32(d + pos);
    ++words;
    pos += 4;
    if ((present[words - 1] & 0x80000000u) == 0) {
      break;
    }
  }

  size_t off = pos;
  for (size_t w = 0; w < words; ++w) {
    for (int bit = 0; bit < 31; ++bit) {
      if ((present[w] & (1u << bit)) == 0) {
        continue;
      }
      const int index = static_cast<int>(w) * 32 + bit;
      size_t align = 1;
      size_t size = 0;
      if (!radiotapField(index, align, size)) {
        return false;
      }
      off = (off + align - 1) & ~(align - 1);
      if (off + size > len) {
        return false;
      }
      if (index == 1) {
        out.fcs = (d[off] & 0x10) != 0;
        out.dataPad = (d[off] & 0x20) != 0;
      } else if (index == 3) {
        out.channel = channelFromFreq(readLe16(d + off));
      } else if (index == 5) {
        out.rssi = static_cast<int8_t>(d[off]);
      }
      off += size;
    }
  }
  if (off > len) {
    return false;
  }
  out.len = len;
  out.valid = true;
  return true;
}

// ---------------------------------------------------------------------------
// 802.11
// ---------------------------------------------------------------------------

struct IeeeInfo {
  bool valid = false;
  uint8_t type = 0xFF;
  uint8_t subtype = 0xFF;
  bool toDs = false;
  bool fromDs = false;
  bool qos = false;
  size_t qosOffset = 0;
  size_t headerLen = 0;
};

bool parseIeee(const uint8_t* d, size_t n, bool dataPad, IeeeInfo& out) {
  out = IeeeInfo{};
  if (n < 2 || (d[0] & 0x03) != 0) {
    return false;
  }
  out.type = static_cast<uint8_t>((d[0] >> 2) & 0x03);
  out.subtype = static_cast<uint8_t>((d[0] >> 4) & 0x0F);
  out.toDs = (d[1] & 0x01) != 0;
  out.fromDs = (d[1] & 0x02) != 0;
  const bool order = (d[1] & 0x80) != 0;

  if (out.type == 0) {
    // Management: 24 byte + HT Control se il bit Order e' impostato.
    out.headerLen = 24 + (order ? 4 : 0);
  } else if (out.type == 2) {
    out.headerLen = 24;
    if (out.toDs && out.fromDs) {
      out.headerLen += 6;
    }
    out.qosOffset = out.headerLen;
    out.qos = (out.subtype & 0x08) != 0;
    if (out.qos) {
      out.headerLen += 2;
      if (order) {
        out.headerLen += 4;
      }
    }
    if (dataPad) {
      out.headerLen = (out.headerLen + 3) & ~static_cast<size_t>(3);
    }
  } else if (out.type == 1) {
    // Controllo: nessun payload LLC, la lunghezza serve solo ai limiti.
    out.headerLen = 10;
  } else {
    out.headerLen = 24;
  }

  if (out.headerLen > n) {
    return false;
  }
  out.valid = true;
  return true;
}

struct EapolInfo {
  bool valid = false;
  bool pairwise = false;
  bool install = false;
  bool ack = false;
  bool micSet = false;
  bool secure = false;
  bool encrypted = false;
  uint8_t descriptor = 0;
  uint16_t keyInfo = 0;
  uint64_t replay = 0;
  const uint8_t* eapol = nullptr;
  size_t eapolLen = 0;
  const uint8_t* nonce = nullptr;
  const uint8_t* mic = nullptr;
  const uint8_t* keyData = nullptr;
  uint16_t keyDataLen = 0;
  uint8_t bssid[6] = {};
  uint8_t sta[6] = {};
};

// Bit standard del Key Information field (802.11 / wpa_supplicant):
// 0x0007 = versione, 0x0008 = pairwise, 0x0040 = install, 0x0080 = ACK,
// 0x0100 = MIC, 0x0200 = secure, 0x1000 = key data cifrata.
// Key Information, Key Data Length e Replay Counter sono big-endian
// (WPA_GET_BE16/BE64), non little-endian come i campi radiotap.
bool parseEapol(const uint8_t* d, size_t n, bool dataPad, EapolInfo& out) {
  out = EapolInfo{};
  IeeeInfo ie;
  if (!parseIeee(d, n, dataPad, ie) || ie.type != 2) {
    return false;
  }
  // Frame protetto: il payload LLC e' cifrato, non e' un EAPOL leggibile.
  if ((d[1] & 0x40) != 0) {
    return false;
  }
  // WDS: direzione AP/STA ambigua, meglio non produrre hash sbagliati.
  if (ie.toDs && ie.fromDs) {
    return false;
  }
  // A-MSDU: il payload non inizia con LLC/SNAP.
  if (ie.qos && ie.qosOffset + 2 <= n && (d[ie.qosOffset] & 0x80) != 0) {
    return false;
  }

  const size_t llc = ie.headerLen;
  if (llc + 8 + 4 > n) {
    return false;
  }
  static const uint8_t kSnap[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};
  if (std::memcmp(d + llc, kSnap, sizeof(kSnap)) != 0) {
    return false;
  }

  const size_t ep = llc + 8;
  if (d[ep + 1] != 3) {
    return false;
  }
  const uint16_t bodyLen = readBe16(d + ep + 2);
  if (bodyLen < 95 || bodyLen > n - ep - 4) {
    return false;
  }

  const uint8_t* key = d + ep + 4;
  const uint8_t descriptor = key[0];
  if (descriptor != 2 && descriptor != 254) {
    return false;
  }
  const uint16_t keyDataLen = readBe16(key + 93);
  if (keyDataLen > bodyLen - 95) {
    return false;
  }

  out.valid = true;
  out.descriptor = descriptor;
  out.keyInfo = readBe16(key + 1);
  out.replay = readBe64(key + 5);
  out.eapol = d + ep;
  out.eapolLen = static_cast<size_t>(bodyLen) + 4;
  out.nonce = key + 13;
  out.mic = key + 77;
  out.keyData = key + 95;
  out.keyDataLen = keyDataLen;
  out.pairwise = (out.keyInfo & 0x0008) != 0;
  out.install = (out.keyInfo & 0x0040) != 0;
  out.ack = (out.keyInfo & 0x0080) != 0;
  out.micSet = (out.keyInfo & 0x0100) != 0;
  out.secure = (out.keyInfo & 0x0200) != 0;
  out.encrypted = (out.keyInfo & 0x1000) != 0;

  if (ie.toDs && !ie.fromDs) {
    std::memcpy(out.bssid, d + 4, 6);   // addr1 = BSSID/AP
    std::memcpy(out.sta, d + 10, 6);    // addr2 = STA
  } else if (!ie.toDs && ie.fromDs) {
    std::memcpy(out.bssid, d + 10, 6);  // addr2 = BSSID/AP
    std::memcpy(out.sta, d + 4, 6);     // addr1 = STA
  } else {
    std::memcpy(out.bssid, d + 16, 6);  // addr3 = BSSID
    std::memcpy(out.sta, d + 10, 6);
  }
  return true;
}

bool parseBeaconSsid(const uint8_t* d, size_t n, uint8_t bssid[6], uint8_t ssid[32],
                     uint8_t& ssidLen) {
  ssidLen = 0;
  IeeeInfo ie;
  if (!parseIeee(d, n, false, ie) || ie.type != 0 ||
      (ie.subtype != 5 && ie.subtype != 8)) {
    return false;
  }
  if (ie.headerLen + 12 > n) {
    return false;
  }

  size_t pos = ie.headerLen + 12;
  while (pos + 2 <= n) {
    const uint8_t id = d[pos];
    const uint8_t len = d[pos + 1];
    if (pos + 2 + len > n) {
      break;
    }
    if (id == 0) {
      if (len > 32) {
        return false;
      }
      if (len == 0) {
        return false;  // SSID nascosto
      }
      std::memcpy(ssid, d + pos + 2, len);
      std::memcpy(bssid, d + 16, 6);
      ssidLen = len;
      return true;
    }
    pos += 2 + len;
  }
  return false;
}

bool findPmkid(const EapolInfo& info, uint8_t out[16]) {
  if (info.encrypted || info.keyData == nullptr) {
    return false;
  }
  const uint8_t* p = info.keyData;
  size_t left = info.keyDataLen;
  while (left >= 2) {
    const uint8_t id = p[0];
    const uint8_t len = p[1];
    if (len > left - 2) {
      return false;
    }
    if (id == 0xDD && len >= 20) {
      const uint8_t* vendor = p + 2;
      if (vendor[0] == 0x00 && vendor[1] == 0x0F && vendor[2] == 0xAC &&
          vendor[3] == 0x04) {
        std::memcpy(out, vendor + 4, 16);
        return true;
      }
    }
    p += 2 + len;
    left -= 2 + len;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Tabelle SSID / nonce / M2 in attesa
// ---------------------------------------------------------------------------

struct SsidEntry {
  uint8_t bssid[6] = {};
  uint8_t len = 0;
  uint8_t text[32] = {};
};

class SsidMap {
 public:
  SsidMap() { items_.reserve(16); }

  void add(const uint8_t bssid[6], const uint8_t* text, uint8_t len) {
    if (text == nullptr || len == 0 || len > 32) {
      return;
    }
    for (auto& item : items_) {
      if (std::memcmp(item.bssid, bssid, 6) == 0) {
        if (item.len == 0) {
          item.len = len;
          std::memcpy(item.text, text, len);
        }
        return;
      }
    }
    if (items_.size() >= kMaxSsids) {
      return;
    }
    SsidEntry item;
    std::memcpy(item.bssid, bssid, 6);
    item.len = len;
    std::memcpy(item.text, text, len);
    items_.push_back(item);
  }

  const SsidEntry* find(const uint8_t bssid[6]) const {
    for (const auto& item : items_) {
      if (std::memcmp(item.bssid, bssid, 6) == 0) {
        return &item;
      }
    }
    return nullptr;
  }

 private:
  std::vector<SsidEntry> items_;
};

struct NonceEntry {
  uint8_t bssid[6] = {};
  uint8_t sta[6] = {};
  uint64_t replay = 0;
  uint8_t nonce[32] = {};
  bool m3 = false;
};

class NonceCache {
 public:
  NonceCache() { items_.reserve(8); }

  void add(const EapolInfo& info, bool m3) {
    for (auto& item : items_) {
      if (item.m3 == m3 && item.replay == info.replay &&
          std::memcmp(item.bssid, info.bssid, 6) == 0 &&
          std::memcmp(item.sta, info.sta, 6) == 0) {
        // Retransmissione: si tiene il primo ANonce visto per quel replay.
        return;
      }
    }
    if (items_.size() >= kMaxNonces) {
      items_.erase(items_.begin());
    }
    NonceEntry item;
    std::memcpy(item.bssid, info.bssid, 6);
    std::memcpy(item.sta, info.sta, 6);
    item.replay = info.replay;
    std::memcpy(item.nonce, info.nonce, 32);
    item.m3 = m3;
    items_.push_back(item);
  }

  const uint8_t* find(bool m3, const uint8_t bssid[6], const uint8_t sta[6],
                      uint64_t replay) const {
    for (const auto& item : items_) {
      if (item.m3 == m3 && item.replay == replay &&
          std::memcmp(item.bssid, bssid, 6) == 0 &&
          std::memcmp(item.sta, sta, 6) == 0) {
        return item.nonce;
      }
    }
    return nullptr;
  }

 private:
  std::vector<NonceEntry> items_;
};

struct PendingM2 {
  uint8_t bssid[6] = {};
  uint8_t sta[6] = {};
  uint64_t replay = 0;
  uint16_t len = 0;
  uint8_t mic[16] = {};
  uint8_t eapol[kMaxEapolBytes] = {};
};

bool samePair(const uint8_t aBssid[6], const uint8_t aSta[6], const uint8_t bBssid[6],
              const uint8_t bSta[6]) {
  return std::memcmp(aBssid, bBssid, 6) == 0 && std::memcmp(aSta, bSta, 6) == 0;
}

void addPending(std::vector<PendingM2>& pending, const EapolInfo& info) {
  if (info.eapolLen > kMaxEapolBytes) {
    return;
  }
  for (const auto& item : pending) {
    if (item.replay == info.replay &&
        samePair(item.bssid, item.sta, info.bssid, info.sta)) {
      return;
    }
  }
  if (pending.size() >= kMaxPending) {
    pending.erase(pending.begin());
  }
  PendingM2 item;
  std::memcpy(item.bssid, info.bssid, 6);
  std::memcpy(item.sta, info.sta, 6);
  item.replay = info.replay;
  item.len = static_cast<uint16_t>(info.eapolLen);
  std::memcpy(item.mic, info.mic, 16);
  std::memcpy(item.eapol, info.eapol, info.eapolLen);
  pending.push_back(item);
}

// ---------------------------------------------------------------------------
// Utility percorsi
// ---------------------------------------------------------------------------

bool endsWithNoCase(const std::string& text, const char* suffix) {
  const size_t len = std::strlen(suffix);
  if (text.size() < len) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    const char a = static_cast<char>(text[text.size() - len + i] | 0x20);
    const char b = static_cast<char>(suffix[i] | 0x20);
    if (a != b) {
      return false;
    }
  }
  return true;
}

std::string stripExtension(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos ||
      (slash != std::string::npos && dot < slash)) {
    return path;
  }
  return path.substr(0, dot);
}

}  // namespace

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

Reader::~Reader() {
  close();
}

bool Reader::open(const std::string& path, bool preferSD) {
  close();
  file_ = storage::openRead(path, preferSD);
  if (!file_ || file_.isDirectory()) {
    close();
    return false;
  }
  if (file_.read(global_, kPcapGlobalSize) != kPcapGlobalSize) {
    close();
    return false;
  }

  const uint32_t magicLe = readLe32(global_);
  if (magicLe == 0xA1B2C3D4) {
    bigEndian_ = false;  // microsecondi, little endian
  } else if (magicLe == 0xA1B23C4D) {
    bigEndian_ = false;  // nanosecondi, little endian
  } else if (magicLe == 0xD4C3B2A1) {
    bigEndian_ = true;   // microsecondi, big endian
  } else if (magicLe == 0x4D3CB2A1) {
    bigEndian_ = true;   // nanosecondi, big endian
  } else {
    close();
    return false;
  }

  linkType_ = read32(global_ + 20, bigEndian_);
  fileSize_ = file_.size();
  buffer_.resize(kMaxFrameBytes);
  index_ = 0;
  recordOffset_ = 0;
  truncated_ = false;
  open_ = true;
  return true;
}

void Reader::close() {
  if (file_) {
    file_.close();
  }
  open_ = false;
  fileSize_ = 0;
  index_ = 0;
  recordOffset_ = 0;
  rawLen_ = 0;
  frameOff_ = 0;
  frameLen_ = 0;
  dataPad_ = false;
  eapol_ = false;
  truncated_ = false;
}

size_t Reader::position() const {
  if (!file_) {
    return 0;
  }
  return file_.position();
}

const uint8_t* Reader::frame() const {
  if (buffer_.empty() || frameOff_ > buffer_.size()) {
    return nullptr;
  }
  return buffer_.data() + frameOff_;
}

const uint8_t* Reader::raw() const {
  if (buffer_.empty()) {
    return nullptr;
  }
  return buffer_.data();
}

bool Reader::readRecord(uint32_t offset, bool& oversized) {
  oversized = false;
  if (!open_) {
    return false;
  }
  if (static_cast<uint64_t>(offset) + kPcapRecordSize > fileSize_) {
    truncated_ = offset < fileSize_;
    return false;
  }
  if (!file_.seek(offset)) {
    truncated_ = true;
    return false;
  }

  uint8_t header[kPcapRecordSize];
  const size_t got = file_.read(header, sizeof(header));
  if (got != sizeof(header)) {
    truncated_ = got > 0;
    return false;
  }

  const uint32_t timestamp = read32(header, bigEndian_);
  const uint32_t usec = read32(header + 4, bigEndian_);
  const uint32_t inclLen = read32(header + 8, bigEndian_);
  const uint32_t origLen = read32(header + 12, bigEndian_);

  if (inclLen > kMaxFrameBytes) {
    // Record troppo grande: lo saltiamo senza caricarlo.
    oversized = true;
    const uint64_t next = static_cast<uint64_t>(offset) + kPcapRecordSize + inclLen;
    if (next > fileSize_) {
      truncated_ = true;
      file_.seek(static_cast<uint32_t>(fileSize_));
      return false;
    }
    if (!file_.seek(static_cast<uint32_t>(next))) {
      truncated_ = true;
      return false;
    }
    return false;
  }

  const uint64_t end = static_cast<uint64_t>(offset) + kPcapRecordSize + inclLen;
  if (end > fileSize_) {
    truncated_ = true;
    return false;
  }
  if (buffer_.size() < kMaxFrameBytes) {
    buffer_.resize(kMaxFrameBytes);
  }
  if (inclLen > 0 && file_.read(buffer_.data(), inclLen) != inclLen) {
    truncated_ = true;
    return false;
  }

  recordOffset_ = offset;
  tsSec_ = timestamp;
  tsUsec_ = usec;
  origLen_ = origLen < inclLen ? inclLen : origLen;
  rawLen_ = inclLen;
  parsePayload();
  return true;
}

void Reader::parsePayload() {
  type_ = 0xFF;
  subtype_ = 0xFF;
  rssi_ = 0;
  channel_ = 0;
  frameOff_ = 0;
  frameLen_ = rawLen_;
  dataPad_ = false;
  eapol_ = false;
  bool dataPad = false;

  if (rawLen_ >= 8) {
    const size_t rtLen = radiotapLength(buffer_.data(), rawLen_);
    if (rtLen != 0) {
      bool trust = false;
      if (linkType_ == kLinkRadiotap) {
        trust = true;
      } else if (linkType_ == kLinkIeee80211) {
        // Alcune catture (comprese quelle del firmware) dichiarano 105 ma
        // includono il radiotap: lo si riconosce da versione/pad/lunghezza e
        // dal fatto che subito dopo deve iniziare un frame 802.11 valido.
        trust = rtLen + 2 <= rawLen_ && (buffer_[rtLen] & 0x03) == 0;
      }
      if (trust) {
        RadiotapInfo rt;
        const bool parsed = parseRadiotap(buffer_.data(), rawLen_, rt);
        frameOff_ = rtLen;
        frameLen_ = rawLen_ - rtLen;
        // Solo per il linktype radiotap puro l'FCS dichiarato e' affidabile:
        // il firmware scrive 105 e rimuove gia' l'FCS prima del salvataggio.
        if (linkType_ == kLinkRadiotap && parsed && rt.fcs && frameLen_ >= 4) {
          frameLen_ -= 4;
        }
        if (parsed) {
          channel_ = rt.channel;
          rssi_ = rt.rssi;
          dataPad = rt.dataPad;
          dataPad_ = rt.dataPad;
        }
      }
    }
  }

  if (frameOff_ + 2 > rawLen_) {
    frameLen_ = 0;
    return;
  }

  IeeeInfo ie;
  if (!parseIeee(buffer_.data() + frameOff_, frameLen_, dataPad, ie)) {
    return;
  }
  type_ = ie.type;
  subtype_ = ie.subtype;

  EapolInfo ep;
  eapol_ = parseEapol(buffer_.data() + frameOff_, frameLen_, dataPad, ep);
}

bool Reader::next(PcapFrame& out) {
  if (!open_) {
    return false;
  }
  for (;;) {
    const uint32_t offset = static_cast<uint32_t>(file_.position());
    bool oversized = false;
    if (!readRecord(offset, oversized)) {
      if (oversized) {
        ++index_;
        if (truncated_) {
          return false;
        }
        delay(0);
        continue;
      }
      return false;
    }
    out = PcapFrame{};
    out.index = ++index_;
    out.tsSec = tsSec_;
    out.len = static_cast<uint32_t>(frameLen_);
    out.type = type_;
    out.subtype = subtype_;
    out.rssi = rssi_;
    out.channel = channel_;
    return true;
  }
}

bool Reader::readAt(uint32_t offset, PcapFrame& out) {
  bool oversized = false;
  if (!readRecord(offset, oversized) || oversized) {
    return false;
  }
  out = PcapFrame{};
  out.tsSec = tsSec_;
  out.len = static_cast<uint32_t>(frameLen_);
  out.type = type_;
  out.subtype = subtype_;
  out.rssi = rssi_;
  out.channel = channel_;
  return true;
}

// ---------------------------------------------------------------------------
// Conversione hc22000
// ---------------------------------------------------------------------------

bool convertToHc22000(const std::string& pcapPath, const std::string& outPath,
                      HcStats& stats, bool preferSD, const ProgressFn& progress) {
  stats = HcStats{};

  Reader reader;
  if (!reader.open(pcapPath, preferSD)) {
    return false;
  }
  if (reader.linkType() != kLinkIeee80211 && reader.linkType() != kLinkRadiotap) {
    reader.close();
    return false;
  }
  const size_t pcapSize = reader.fileSize();

  // Prima passata: SSID dei beacon e delle probe response.
  SsidMap ssids;
  PcapFrame frame;
  uint32_t totalFrames = 0;
  uint32_t counter = 0;
  while (reader.next(frame)) {
    ++totalFrames;
    uint8_t bssid[6] = {};
    uint8_t ssid[32] = {};
    uint8_t ssidLen = 0;
    if (parseBeaconSsid(reader.frame(), reader.frameLen(), bssid, ssid, ssidLen)) {
      ssids.add(bssid, ssid, ssidLen);
    }
    if (progress && (++counter & 0x3F) == 0) {
      if (!progress(0, reader.position(), pcapSize)) {
        reader.close();
        return false;
      }
    }
  }
  reader.close();
  if (progress) {
    if (!progress(0, pcapSize, pcapSize)) {
      return false;
    }
  }

  // Seconda passata: PMKID e handshake.
  if (!reader.open(pcapPath, preferSD)) {
    return false;
  }
  File out = storage::openWrite(outPath, preferSD);
  if (!out) {
    reader.close();
    return false;
  }

  LineWriter writer(out);
  NonceCache nonces;
  std::vector<PendingM2> pending;
  pending.reserve(8);
  HcStats result;
  result.frames = totalFrames;
  counter = 0;
  bool aborted = false;

  auto emitM2 = [&](const uint8_t* nonceAp, const uint8_t bssid[6], const uint8_t sta[6],
                    const uint8_t mic[16], const uint8_t* eapol, size_t eapolLen,
                    uint8_t messagePair) -> bool {
    const SsidEntry* ssid = ssids.find(bssid);
    if (ssid == nullptr) {
      ++result.skippedNoEssid;
      return true;
    }
    const std::string line = formatEapol(mic, bssid, sta, ssid->text, ssid->len,
                                         nonceAp, eapol, eapolLen, messagePair);
    const LineWriter::Result res = writer.write(line);
    if (res == LineWriter::Result::Written) {
      ++result.handshakes;
    }
    return res != LineWriter::Result::Error;
  };

  while (reader.next(frame)) {
    // parsePayload() ha gia' classificato il frame: evitare una seconda
    // parseEapol() su tutti i frame non EAPOL dimezza il lavoro della passata.
    if (!reader.isEapol()) {
      if (progress && (++counter & 0x3F) == 0) {
        if (!progress(1, reader.position(), pcapSize)) {
          aborted = true;
          break;
        }
      }
      continue;
    }
    EapolInfo info;
    if (parseEapol(reader.frame(), reader.frameLen(), reader.dataPad(), info)) {
      ++result.eapol;

      if (info.pairwise && info.ack && !info.install) {
        // Messaggio 1: possibile PMKID nel key data (non cifrato).
        uint8_t pmkid[16];
        if (findPmkid(info, pmkid)) {
          const SsidEntry* ssid = ssids.find(info.bssid);
          if (ssid == nullptr || ssid->len == 0) {
            // Senza ESSID la riga non e' verificabile da hashcat: non
            // scriverla, come nel collector live.
            ++result.skippedNoEssid;
          } else {
            const std::string line =
                formatPmkid(pmkid, info.bssid, info.sta, ssid->text, ssid->len);
            const LineWriter::Result res = writer.write(line);
            if (res == LineWriter::Result::Written) {
              ++result.pmkid;
            } else if (res == LineWriter::Result::Error) {
              aborted = true;
              break;
            }
          }
        }

        nonces.add(info, false);
        // M1 puo' arrivare dopo M2 per riordino: prova ad accoppiare subito.
        for (size_t i = 0; i < pending.size();) {
          const PendingM2& m2 = pending[i];
          if (m2.replay == info.replay &&
              samePair(m2.bssid, m2.sta, info.bssid, info.sta)) {
            const uint8_t* nonce = nonces.find(false, m2.bssid, m2.sta, m2.replay);
            if (nonce == nullptr) {
              ++i;
              continue;
            }
            const bool ok = emitM2(nonce, m2.bssid, m2.sta, m2.mic, m2.eapol, m2.len,
                                   0x00);
            if (!ok) {
              aborted = true;
              break;
            }
            pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(i));
          } else {
            ++i;
          }
        }
        if (aborted) {
          break;
        }
      } else if (info.pairwise && info.ack && info.install) {
        // Messaggio 3: fornisce l'ANonce per un M2 gia' visto.
        nonces.add(info, true);
        for (size_t i = 0; i < pending.size();) {
          const PendingM2& m2 = pending[i];
          if (info.replay == m2.replay + 1 &&
              samePair(m2.bssid, m2.sta, info.bssid, info.sta)) {
            const uint8_t* nonce = nonces.find(true, m2.bssid, m2.sta, info.replay);
            if (nonce == nullptr) {
              ++i;
              continue;
            }
            const bool ok = emitM2(nonce, m2.bssid, m2.sta, m2.mic, m2.eapol, m2.len,
                                   0x02);
            if (!ok) {
              aborted = true;
              break;
            }
            pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(i));
          } else {
            ++i;
          }
        }
        if (aborted) {
          break;
        }
      } else if (info.pairwise && !info.ack && info.micSet && !info.secure &&
                 info.keyDataLen > 0) {
        // Messaggio 2: EAPOL del client (key data = RSN/WPA IE), ANonce da M1/M3.
        // Il key data non vuoto distingue M2 da M4; il bit Secure aggiunge una
        // seconda barriera contro i falsi M4.
        uint8_t messagePair = 0x00;
        const uint8_t* nonceAp =
            nonces.find(false, info.bssid, info.sta, info.replay);
        if (nonceAp == nullptr) {
          nonceAp = nonces.find(true, info.bssid, info.sta, info.replay + 1);
          messagePair = 0x02;
        }
        if (nonceAp != nullptr) {
          if (!emitM2(nonceAp, info.bssid, info.sta, info.mic, info.eapol,
                      info.eapolLen, messagePair)) {
            aborted = true;
            break;
          }
        } else {
          addPending(pending, info);
        }
      }
    }

    if (progress && (++counter & 0x3F) == 0) {
      if (!progress(1, reader.position(), pcapSize)) {
        aborted = true;
        break;
      }
    }
  }
  reader.close();

  if (!aborted && progress) {
    aborted = !progress(1, pcapSize, pcapSize);
  }

  out.flush();
  out.close();
  if (aborted || writer.failed()) {
    storage::removeFile(outPath, preferSD);
    stats = result;
    return false;
  }
  // Anche senza hash il file resta: e' l'output richiesto dall'operazione
  // (0 byte se non e' stato trovato nulla).
  stats = result;
  return true;
}

// ---------------------------------------------------------------------------
// Estrazione EAPOL
// ---------------------------------------------------------------------------

bool extractEapol(const std::string& inPath, const std::string& outPath,
                  HcStats& stats, bool preferSD, const ProgressFn& progress) {
  stats = HcStats{};

  Reader reader;
  if (!reader.open(inPath, preferSD)) {
    return false;
  }
  if (reader.linkType() != kLinkIeee80211 && reader.linkType() != kLinkRadiotap) {
    reader.close();
    return false;
  }
  const size_t pcapSize = reader.fileSize();

  File out = storage::openWrite(outPath, preferSD);
  if (!out) {
    reader.close();
    return false;
  }
  const uint8_t* global = reader.globalHeader();
  if (out.write(global, kPcapGlobalSize) != kPcapGlobalSize) {
    out.close();
    reader.close();
    storage::removeFile(outPath, preferSD);
    return false;
  }

  PcapFrame frame;
  uint32_t counter = 0;
  bool failed = false;
  while (reader.next(frame)) {
    ++stats.frames;
    if (reader.isEapol()) {
      ++stats.eapol;
      uint8_t header[kPcapRecordSize];
      write32(header, frame.tsSec, reader.bigEndian());
      write32(header + 4, reader.tsUsec(), reader.bigEndian());
      write32(header + 8, static_cast<uint32_t>(reader.rawLen()), reader.bigEndian());
      const uint32_t orig = reader.origLen() < reader.rawLen()
                                ? static_cast<uint32_t>(reader.rawLen())
                                : reader.origLen();
      write32(header + 12, orig, reader.bigEndian());
      if (out.write(header, kPcapRecordSize) != kPcapRecordSize ||
          out.write(reader.raw(), reader.rawLen()) != reader.rawLen()) {
        failed = true;
        break;
      }
    }
    if (progress && (++counter & 0x3F) == 0) {
      if (!progress(0, reader.position(), pcapSize)) {
        failed = true;
        break;
      }
    }
  }
  reader.close();

  if (!failed && progress) {
    failed = !progress(0, pcapSize, pcapSize);
  }

  out.flush();
  out.close();
  if (failed) {
    storage::removeFile(outPath, preferSD);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Cattura live
// ---------------------------------------------------------------------------

struct LiveCollector::Impl {
  struct Event {
    enum Kind : uint8_t { Pmkid = 0, M1 = 1, M2 = 2, M3 = 3 };
    uint8_t kind = Pmkid;
    uint8_t bssid[6] = {};
    uint8_t sta[6] = {};
    uint64_t replay = 0;
    uint8_t nonce[32] = {};
    uint8_t pmkid[16] = {};
    uint8_t mic[16] = {};
    uint16_t len = 0;
    uint8_t eapol[kMaxEapolBytes] = {};
  };

  struct Ssid {
    uint8_t bssid[6] = {};
    uint8_t len = 0;
    uint8_t text[32] = {};
    uint32_t seen = 0;  // ultimo frame in cui il BSSID e' stato visto (LRU)
  };

  Event events[kMaxLiveEntries];
  uint16_t count = 0;
  Ssid ssids[kMaxLiveSsids];
  uint16_t ssidCount = 0;
  uint32_t ssidTick = 0;
  uint32_t frames = 0;
  uint32_t eapol = 0;

  void reset() {
    count = 0;
    ssidCount = 0;
    ssidTick = 0;
    frames = 0;
    eapol = 0;
  }

  void addSsid(const uint8_t bssid[6], const uint8_t* text, uint8_t len) {
    if (text == nullptr || len == 0 || len > 32) {
      return;
    }
    ++ssidTick;
    for (uint16_t i = 0; i < ssidCount; ++i) {
      if (std::memcmp(ssids[i].bssid, bssid, 6) == 0) {
        if (ssids[i].len == 0) {
          ssids[i].len = len;
          std::memcpy(ssids[i].text, text, len);
        }
        ssids[i].seen = ssidTick;
        return;
      }
    }
    uint16_t slot = 0;
    if (ssidCount < kMaxLiveSsids) {
      slot = ssidCount++;
    } else {
      // Tabella piena: si sostituisce il BSSID visto meno di recente, cosi'
      // l'AP dell'handshake non viene espulso dai beacon di reti di passaggio.
      for (uint16_t i = 1; i < kMaxLiveSsids; ++i) {
        if (ssids[i].seen < ssids[slot].seen) {
          slot = i;
        }
      }
    }
    Ssid& item = ssids[slot];
    std::memcpy(item.bssid, bssid, 6);
    item.len = len;
    std::memcpy(item.text, text, len);
    item.seen = ssidTick;
  }

  const Ssid* findSsid(const uint8_t bssid[6]) const {
    for (uint16_t i = 0; i < ssidCount; ++i) {
      if (std::memcmp(ssids[i].bssid, bssid, 6) == 0) {
        return &ssids[i];
      }
    }
    return nullptr;
  }

  bool addEvent(const Event& event) {
    for (uint16_t i = 0; i < count; ++i) {
      Event& item = events[i];
      if (item.kind != event.kind ||
          !samePair(item.bssid, item.sta, event.bssid, event.sta)) {
        continue;
      }
      if (event.kind == Event::Pmkid) {
        if (std::memcmp(item.pmkid, event.pmkid, 16) == 0) {
          return false;
        }
      } else if (event.kind == Event::M2) {
        if (item.replay == event.replay) {
          return false;
        }
      } else if (item.replay == event.replay) {
        // M1/M3 ripetuto: si conserva il primo nonce.
        return true;
      }
    }
    if (count >= kMaxLiveEntries) {
      return false;
    }
    events[count++] = event;
    return true;
  }

  bool feed(const uint8_t* payload, size_t len) {
    if (payload == nullptr || len < 2) {
      return false;
    }
    ++frames;

    const uint8_t* d = payload;
    size_t n = len;
    bool dataPad = false;
    const size_t rtLen = radiotapLength(payload, len);
    if (rtLen != 0) {
      d += rtLen;
      n -= rtLen;
      RadiotapInfo rt;
      if (parseRadiotap(payload, len, rt)) {
        dataPad = rt.dataPad;
      }
      // L'FCS e' gia' stato tolto da snifferCb prima della copia nel ring.
    }
    if (n < 2) {
      return false;
    }

    uint8_t bssid[6] = {};
    uint8_t ssid[32] = {};
    uint8_t ssidLen = 0;
    if (parseBeaconSsid(d, n, bssid, ssid, ssidLen)) {
      addSsid(bssid, ssid, ssidLen);
      return true;
    }

    EapolInfo info;
    if (!parseEapol(d, n, dataPad, info)) {
      return false;
    }
    ++eapol;
    if (!info.pairwise) {
      return true;
    }

    if (info.ack && !info.install) {
      uint8_t pmkid[16];
      if (findPmkid(info, pmkid)) {
        Event event;
        event.kind = Event::Pmkid;
        std::memcpy(event.bssid, info.bssid, 6);
        std::memcpy(event.sta, info.sta, 6);
        std::memcpy(event.pmkid, pmkid, 16);
        addEvent(event);
      }
      Event event;
      event.kind = Event::M1;
      std::memcpy(event.bssid, info.bssid, 6);
      std::memcpy(event.sta, info.sta, 6);
      event.replay = info.replay;
      std::memcpy(event.nonce, info.nonce, 32);
      addEvent(event);
    } else if (info.ack && info.install) {
      Event event;
      event.kind = Event::M3;
      std::memcpy(event.bssid, info.bssid, 6);
      std::memcpy(event.sta, info.sta, 6);
      event.replay = info.replay;
      std::memcpy(event.nonce, info.nonce, 32);
      addEvent(event);
    } else if (!info.ack && info.micSet && !info.secure && info.keyDataLen > 0) {
      if (info.eapolLen <= kMaxEapolBytes) {
        Event event;
        event.kind = Event::M2;
        std::memcpy(event.bssid, info.bssid, 6);
        std::memcpy(event.sta, info.sta, 6);
        event.replay = info.replay;
        event.len = static_cast<uint16_t>(info.eapolLen);
        std::memcpy(event.mic, info.mic, 16);
        std::memcpy(event.eapol, info.eapol, info.eapolLen);
        addEvent(event);
      }
    }
    return true;
  }

  bool writeFile(const std::string& path, bool preferSD, HcStats* stats) {
    HcStats result;
    result.frames = frames;
    result.eapol = eapol;

    File out = storage::openWrite(path, preferSD);
    if (!out) {
      if (stats != nullptr) {
        *stats = result;
      }
      return false;
    }
    LineWriter writer(out);

    for (uint16_t i = 0; i < count; ++i) {
      const Event& event = events[i];
      if (event.kind != Event::Pmkid) {
        continue;
      }
      const Ssid* ssid = findSsid(event.bssid);
      if (ssid == nullptr || ssid->len == 0) {
        // Senza ESSID hashcat non puo' verificare la riga: non scriverla.
        ++result.skippedNoEssid;
        continue;
      }
      const std::string line =
          formatPmkid(event.pmkid, event.bssid, event.sta, ssid->text, ssid->len);
      const LineWriter::Result res = writer.write(line);
      if (res == LineWriter::Result::Written) {
        ++result.pmkid;
      } else if (res == LineWriter::Result::Error) {
        break;
      }
    }

    if (!writer.failed()) {
      for (uint16_t i = 0; i < count; ++i) {
        const Event& event = events[i];
        if (event.kind != Event::M2) {
          continue;
        }
        const Ssid* ssid = findSsid(event.bssid);
        if (ssid == nullptr) {
          ++result.skippedNoEssid;
          continue;
        }
        const uint8_t* nonceAp = nullptr;
        uint8_t messagePair = 0x00;
        for (uint16_t j = 0; j < count; ++j) {
          const Event& other = events[j];
          if (other.kind == Event::M1 && other.replay == event.replay &&
              samePair(other.bssid, other.sta, event.bssid, event.sta)) {
            nonceAp = other.nonce;
            break;
          }
        }
        if (nonceAp == nullptr) {
          for (uint16_t j = 0; j < count; ++j) {
            const Event& other = events[j];
            if (other.kind == Event::M3 && other.replay == event.replay + 1 &&
                samePair(other.bssid, other.sta, event.bssid, event.sta)) {
              nonceAp = other.nonce;
              messagePair = 0x02;
              break;
            }
          }
        }
        if (nonceAp == nullptr) {
          continue;
        }
        const std::string line =
            formatEapol(event.mic, event.bssid, event.sta, ssid->text, ssid->len,
                        nonceAp, event.eapol, event.len, messagePair);
        const LineWriter::Result res = writer.write(line);
        if (res == LineWriter::Result::Written) {
          ++result.handshakes;
        } else if (res == LineWriter::Result::Error) {
          break;
        }
      }
    }

    out.flush();
    out.close();
    const bool ok = !writer.failed();
    if (!ok) {
      storage::removeFile(path, preferSD);
    }
    if (stats != nullptr) {
      *stats = result;
    }
    return ok;
  }
};

LiveCollector::LiveCollector() {
  impl_.reset(new (std::nothrow) Impl());
}

LiveCollector::~LiveCollector() = default;

void LiveCollector::reset() {
  if (impl_) {
    impl_->reset();
  }
}

bool LiveCollector::feed(const uint8_t* payload, size_t len) {
  return impl_ ? impl_->feed(payload, len) : false;
}

bool LiveCollector::writeFile(const std::string& path, bool preferSD, HcStats* stats) {
  if (!impl_) {
    if (stats != nullptr) {
      *stats = HcStats{};
    }
    return false;
  }
  return impl_->writeFile(path, preferSD, stats);
}

size_t LiveCollector::entries() const {
  return impl_ ? impl_->count : 0;
}

// ---------------------------------------------------------------------------
// Utility pubbliche
// ---------------------------------------------------------------------------

bool isPcapName(const std::string& name) {
  return endsWithNoCase(name, ".pcap");
}

bool isHc22000Name(const std::string& name) {
  return endsWithNoCase(name, ".hc22000");
}

std::string joinPath(const std::string& dir, const std::string& name) {
  if (dir.empty()) {
    return name;
  }
  if (dir.back() == '/') {
    return dir + name;
  }
  return dir + "/" + name;
}

std::string hc22000Path(const std::string& pcapPath) {
  return stripExtension(pcapPath) + ".hc22000";
}

std::string eapolPcapPath(const std::string& pcapPath) {
  return stripExtension(pcapPath) + "_eapol.pcap";
}

}  // namespace pcaptools
