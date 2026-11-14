#pragma once

#include <FS.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Analisi in streaming delle catture 802.11 in formato PCAP.
// I file non vengono mai caricati interamente in RAM: il Reader legge un
// record alla volta e riusa sempre lo stesso buffer.
namespace pcaptools {

constexpr const char* kCaptureDir = "/captures";

// Limite del buffer di un singolo record (radiotap + frame 802.11 + FCS).
// Le catture scritte dal firmware arrivano a 1600 byte; 4096 copre anche
// frame esterni piu' grandi senza sprecare troppa RAM.
constexpr size_t kMaxFrameBytes = 4096;

// Copia massima del frame EAPOL tenuta in RAM durante la cattura live.
constexpr size_t kMaxEapolBytes = 256;

// Righe massime scritte in un file hc22000 (deduplicate).
constexpr uint16_t kMaxHcLines = 512;

// Eventi EAPOL/PMKID massimi tenuti in RAM dalla cattura live.
constexpr uint16_t kMaxLiveEntries = 8;

// Frame massimi indicizzati dal browser (solo offset, non i dati).
constexpr uint32_t kMaxIndexFrames = 4000;

struct PcapFrame {
  uint32_t index = 0;   // numero progressivo del record (1 = primo)
  uint32_t tsSec = 0;   // timestamp del record PCAP
  uint32_t len = 0;     // lunghezza del frame 802.11 (senza radiotap/FCS)
  uint8_t type = 0xFF;  // tipo 802.11: 0 mgmt, 1 ctrl, 2 data, 3 ext
  uint8_t subtype = 0xFF;
  int8_t rssi = 0;      // valido solo se il radiotap e' presente
  uint8_t channel = 0;  // 0 = canale sconosciuto
};


struct HcStats {
  uint32_t frames = 0;
  uint32_t eapol = 0;
  uint32_t pmkid = 0;
  uint32_t handshakes = 0;
  uint32_t skippedNoEssid = 0;
};

// Callback di avanzamento (pass, offset, dimensione). Ritornare false
// interrompe l'operazione e fa rimuovere l'output parziale.
using ProgressFn = std::function<bool(uint8_t pass, size_t offset, size_t size)>;

// Lettore PCAP sequenziale. Supporta little/big endian, timestamp in
// microsecondi o nanosecondi, linktype 105 (802.11) e 127 (radiotap).
// Il payload resta valido fino alla chiamata successiva a next()/readAt().
class Reader {
 public:
  Reader() = default;
  ~Reader();

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  bool open(const std::string& path, bool preferSD = true);
  void close();

  bool next(PcapFrame& out);
  bool readAt(uint32_t offset, PcapFrame& out);

  const uint8_t* frame() const;
  size_t frameLen() const { return frameLen_; }
  const uint8_t* raw() const;
  size_t rawLen() const { return rawLen_; }
  bool isEapol() const { return eapol_; }
  bool dataPad() const { return dataPad_; }
  uint32_t linkType() const { return linkType_; }
  bool supportedLinkType() const { return linkType_ == 105 || linkType_ == 127; }
  size_t fileSize() const { return fileSize_; }
  size_t position() const;
  uint32_t frameOffset() const { return recordOffset_; }
  uint32_t tsUsec() const { return tsUsec_; }
  uint32_t origLen() const { return origLen_; }
  bool truncated() const { return truncated_; }
  const uint8_t* globalHeader() const { return global_; }
  bool bigEndian() const { return bigEndian_; }

 private:
  bool readRecord(uint32_t offset, bool& oversized);
  void parsePayload();

  File file_;
  std::vector<uint8_t> buffer_;
  uint8_t global_[24] = {0};
  bool bigEndian_ = false;
  bool open_ = false;
  bool truncated_ = false;
  uint32_t linkType_ = 0;
  size_t fileSize_ = 0;
  uint32_t index_ = 0;
  uint32_t recordOffset_ = 0;
  uint32_t tsSec_ = 0;
  uint32_t tsUsec_ = 0;
  uint32_t origLen_ = 0;
  size_t rawLen_ = 0;
  size_t frameOff_ = 0;
  size_t frameLen_ = 0;
  uint8_t type_ = 0xFF;
  uint8_t subtype_ = 0xFF;
  int8_t rssi_ = 0;
  uint8_t channel_ = 0;
  bool dataPad_ = false;
  bool eapol_ = false;
};

// Converte una cattura in un file hashcat 22000. L'analisi avviene in due
// passate: la prima indicizza gli SSID dei beacon/probe response, la seconda
// estrae PMKID e handshake. Il file di output viene rimosso se resta vuoto.
bool convertToHc22000(const std::string& pcapPath, const std::string& outPath,
                      HcStats& stats, bool preferSD = true,
                      const ProgressFn& progress = nullptr);

// Scrive un PCAP con lo stesso global header e i soli frame EAPOL-Key.
bool extractEapol(const std::string& inPath, const std::string& outPath,
                  HcStats& stats, bool preferSD = true,
                  const ProgressFn& progress = nullptr);

// Collettore per la cattura live: riceve i payload grezzi del ring buffer
// (radiotap compreso), li analizza sul task principale e tiene in RAM al
// massimo kMaxLiveEntries eventi. Non esegue I/O durante feed().
class LiveCollector {
 public:
  LiveCollector();
  ~LiveCollector();

  LiveCollector(const LiveCollector&) = delete;
  LiveCollector& operator=(const LiveCollector&) = delete;

  void reset();
  bool feed(const uint8_t* payload, size_t len);
  bool writeFile(const std::string& path, bool preferSD, HcStats* stats = nullptr);
  size_t entries() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Utility sui percorsi usate dalla UI.
bool isPcapName(const std::string& name);
bool isHc22000Name(const std::string& name);
std::string joinPath(const std::string& dir, const std::string& name);
std::string hc22000Path(const std::string& pcapPath);
std::string eapolPcapPath(const std::string& pcapPath);

}  // namespace pcaptools
