#include "PcapMenu.h"

#include "ModuleFlags.h"

#if ENABLE_MODULE_PCAPTOOLS

#include <Arduino.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "Display.h"
#include "Logger.h"
#include "MenuManager.h"
#include "Navigation.h"
#include "PcapTools.h"
#include "PowerTimer.h"
#include "Storage.h"
#include "Ui.h"

namespace pcaptools_ui {
namespace {

namespace pc = pcaptools;

struct CaptureItem {
  std::string name;
  size_t size = 0;
  bool hc = false;
};

std::string shorten(const std::string& text, size_t maxChars) {
  if (maxChars == 0) {
    return "";
  }
  if (text.size() <= maxChars) {
    return text;
  }
  if (maxChars == 1) {
    return "~";
  }
  return text.substr(0, maxChars - 1) + "~";
}

std::string fileName(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string formatSize(size_t size) {
  char buffer[20];
  if (size < 1024) {
    std::snprintf(buffer, sizeof(buffer), "%uB", static_cast<unsigned>(size));
  } else if (size < 1024UL * 1024UL) {
    std::snprintf(buffer, sizeof(buffer), "%uK",
                  static_cast<unsigned>((size + 1023) / 1024));
  } else {
    const unsigned whole = static_cast<unsigned>(size / (1024UL * 1024UL));
    const unsigned tenth = static_cast<unsigned>(((size / 1024UL) % 1024UL) * 10 / 1024);
    std::snprintf(buffer, sizeof(buffer), "%u.%uM", whole, tenth);
  }
  return buffer;
}

// Elenca /captures. Se la SD e' montata ha priorita'; se non contiene file
// utili si ripiega su LittleFS.
bool collectCaptures(std::vector<CaptureItem>& items, bool includePcap, bool includeHc,
                     bool& preferSD) {
  items.clear();
  if (!storage::hasSD() && !storage::hasLittleFS()) {
    return false;
  }
  preferSD = storage::hasSD();

  auto gather = [&](bool sd) {
    const std::vector<storage::Entry> entries =
        storage::listEntries(pc::kCaptureDir, sd);
    for (const auto& entry : entries) {
      if (entry.isDir) {
        continue;
      }
      const bool hc = pc::isHc22000Name(entry.name);
      if (hc) {
        if (!includeHc) {
          continue;
        }
      } else if (pc::isPcapName(entry.name)) {
        if (!includePcap) {
          continue;
        }
      } else {
        continue;
      }
      CaptureItem item;
      item.name = entry.name;
      item.size = entry.size;
      item.hc = hc;
      items.push_back(std::move(item));
    }
  };

  if (preferSD) {
    gather(true);
  }
  if (items.empty() && storage::hasLittleFS()) {
    preferSD = false;
    gather(false);
  }

  std::sort(items.begin(), items.end(), [](const CaptureItem& a, const CaptureItem& b) {
    if (a.hc != b.hc) {
      return !a.hc;
    }
    return a.name < b.name;
  });
  return true;
}

void drawPicker(const std::string& title, const std::vector<CaptureItem>& items,
                int selected, int top, int rows) {
  display::clear(display::kPaper);
  display::header(title, "Capture");
  for (int i = 0; i < rows; ++i) {
    const int index = top + i;
    if (index >= static_cast<int>(items.size())) {
      break;
    }
    const int y = 23 + i * 10;
    const bool isSelected = index == selected;
    if (isSelected) {
      display::g().fillRect(1, y, display::width() - 2, 10, display::kPaperLight);
      display::g().fillRect(1, y, 2, 10, display::kAccent);
    }

    const std::string size = formatSize(items[index].size);
    const int sizeWidth = static_cast<int>(size.size()) * 6;
    const size_t maxChars = static_cast<size_t>(
        std::max(8, (display::width() - 12 - sizeWidth) / 6));
    std::string label = items[index].name;
    if (items[index].hc) {
      label += " [out]";
    }
    label = shorten(label, maxChars);

    const uint16_t color = isSelected
                               ? display::kInk
                               : (items[index].hc ? display::kMuted : display::kInk);
    display::text(4, y + 1, label, color, 0);
    display::text(display::width() - sizeWidth - 3, y + 1, size, display::kMuted, 0);
  }
  display::footer("ESC=indietro  ;.=nav  ENTER=apri");
  display::push();
}

int pickCapture(const std::string& title, const std::vector<CaptureItem>& items) {
  if (items.empty()) {
    return -1;
  }
  constexpr int kRowH = 10;
  const int rows = std::max(1, (display::height() - 21 - 12) / kRowH);
  const int maxTop = std::max(0, static_cast<int>(items.size()) - rows);
  int selected = 0;
  int top = 0;
  bool dirty = true;
  uint32_t lastDraw = 0;

  navigation::waitRelease();
  for (;;) {
    const uint32_t now = millis();
    if (dirty || now - lastDraw > 300) {
      drawPicker(title, items, selected, top, rows);
      dirty = false;
      lastDraw = now;
    }

    const navigation::EventData ev = navigation::poll();
    if (ev.type == navigation::Event::Back) {
      navigation::waitRelease();
      return -1;
    }
    if (ev.type == navigation::Event::Select) {
      navigation::waitRelease();
      return selected;
    }
    if (ev.type == navigation::Event::Up) {
      selected = std::max(0, selected - 1);
      dirty = true;
    } else if (ev.type == navigation::Event::Down) {
      selected = std::min(static_cast<int>(items.size()) - 1, selected + 1);
      dirty = true;
    } else if (ev.type == navigation::Event::Left) {
      selected = std::max(0, selected - rows);
      dirty = true;
    } else if (ev.type == navigation::Event::Right) {
      selected = std::min(static_cast<int>(items.size()) - 1, selected + rows);
      dirty = true;
    }

    if (selected < top) {
      top = selected;
    }
    if (selected >= top + rows) {
      top = selected - rows + 1;
    }
    top = std::max(0, std::min(top, maxTop));
    delay(20);
  }
}

const char* labelFor(uint8_t type, uint8_t subtype, bool eapol) {
  if (eapol) {
    return "EAPOL";
  }
  if (type == 0) {
    switch (subtype) {
      case 0:
        return "ASSOCREQ";
      case 1:
        return "ASSOCRESP";
      case 2:
        return "REASSOC";
      case 3:
        return "REASSOCRS";
      case 4:
        return "PROBE";
      case 5:
        return "PROBERESP";
      case 8:
        return "BEACON";
      case 9:
        return "ATIM";
      case 10:
        return "DISASSOC";
      case 11:
        return "AUTH";
      case 12:
        return "DEAUTH";
      case 13:
        return "ACTION";
      case 14:
        return "ACTNOACK";
      default:
        return "MGMT";
    }
  }
  if (type == 1) {
    switch (subtype) {
      case 8:
        return "BAR";
      case 9:
        return "BA";
      case 10:
        return "PSPOLL";
      case 11:
        return "RTS";
      case 12:
        return "CTS";
      case 13:
        return "ACK";
      case 14:
        return "CFEND";
      default:
        return "CTRL";
    }
  }
  if (type == 2) {
    switch (subtype) {
      case 4:
        return "NULL";
      case 12:
        return "QOSNULL";
      case 8:
        return "QOSDATA";
      default:
        return "DATA";
    }
  }
  if (type == 3) {
    return "EXT";
  }
  return "?";
}

void drawScanProgress(const std::string& file, size_t count, size_t offset,
                      size_t total) {
  display::clear(display::kPaper);
  display::header("Indice frame", "Capture");
  display::text(4, 26, shorten(file, 38), display::kInk, 0);
  display::text(4, 40, "Frame: " + std::to_string(count), display::kMuted, 0);
  const float fraction =
      total > 0 ? std::min(1.0f, static_cast<float>(offset) / static_cast<float>(total))
                : 0.0f;
  display::progressBar(12, 60, display::width() - 24, 8, fraction, display::kAccent);
  char percent[8];
  std::snprintf(percent, sizeof(percent), "%u%%",
                static_cast<unsigned>(fraction * 100.0f + 0.5f));
  display::textCentered(78, percent, display::kInk, 2);
  display::textCentered(94, formatSize(offset) + " / " + formatSize(total),
                        display::kMuted, 0);
  display::footer("ESC = annulla");
  display::push();
}

void drawBrowser(pc::Reader& reader, const std::vector<uint32_t>& offsets,
                 const std::string& file, int selected, int top, int rows,
                 bool truncatedIndex, bool damaged) {
  display::clear(display::kPaper);
  display::header(file, "Capture");

  for (int i = 0; i < rows; ++i) {
    const int index = top + i;
    if (index >= static_cast<int>(offsets.size())) {
      break;
    }
    const int y = 22 + i * 10;
    const bool isSelected = index == selected;
    if (isSelected) {
      display::g().fillRect(1, y, display::width() - 2, 10, display::kPaperLight);
      display::g().fillRect(1, y, 2, 10, display::kAccent);
    }

    char line[64];
    pc::PcapFrame frame;
    bool eapol = false;
    bool ok = reader.readAt(offsets[index], frame);
    if (ok) {
      eapol = reader.isEapol();
      const char* label = labelFor(frame.type, frame.subtype, eapol);
      if (frame.channel != 0) {
        std::snprintf(line, sizeof(line), "%5u %-10s %4u ch%02u %4d",
                      static_cast<unsigned>(index + 1), label,
                      static_cast<unsigned>(frame.len),
                      static_cast<unsigned>(frame.channel),
                      static_cast<int>(frame.rssi));
      } else {
        std::snprintf(line, sizeof(line), "%5u %-10s %4u",
                      static_cast<unsigned>(index + 1), label,
                      static_cast<unsigned>(frame.len));
      }
    } else {
      std::snprintf(line, sizeof(line), "%5u %-10s", static_cast<unsigned>(index + 1),
                    "?");
    }

    uint16_t color = display::kInk;
    if (!isSelected && eapol) {
      color = display::kDanger;
    }
    display::text(4, y + 1, line, color, 0);
  }

  if (truncatedIndex) {
    display::footer("ESC=torna  ;.=nav  ,/=pag  max 20000");
  } else if (damaged) {
    display::footer("ESC=torna  ;.=nav  file troncato");
  } else {
    display::footer("ESC=torna  ;.=nav  ,/=pag  ENTER=hex");
  }
  display::push();
}

void showHex(pc::Reader& reader, uint32_t offset, const std::string& file,
             uint32_t number) {
  pc::PcapFrame frame;
  if (!reader.readAt(offset, frame)) {
    ui::message("Hex", {"!Frame non leggibile."}, "Capture");
    return;
  }
  const uint8_t* data = reader.frame();
  const size_t length = data == nullptr ? 0 : reader.frameLen();
  constexpr size_t kMaxBytes = 128;

  navigation::waitRelease();
  uint32_t lastDraw = 0;
  for (;;) {
    const uint32_t now = millis();
    if (lastDraw == 0 || now - lastDraw > 300) {
      display::clear(display::kPaper);
      display::header("Hex #" + std::to_string(number), file);

      if (length == 0) {
        display::text(4, 26, "Frame vuoto", display::kMuted, 0);
      } else {
        const size_t rows = std::min<size_t>(8, (length + 15) / 16);
        for (size_t row = 0; row < rows; ++row) {
          const size_t base = row * 16;
          char line[64];
          int pos = std::snprintf(line, sizeof(line), "%04X ",
                                  static_cast<unsigned>(base));
          for (size_t i = 0; i < 16 && base + i < length && base + i < kMaxBytes;
               ++i) {
            std::snprintf(line + pos, sizeof(line) - static_cast<size_t>(pos), "%02x",
                          static_cast<unsigned>(data[base + i]));
            pos += 2;
          }
          display::text(3, 23 + static_cast<int>(row) * 10, line, display::kInk, 0);
        }
      }
      display::footer(length > kMaxBytes ? "ESC=torna  primi 128B" : "ESC=torna");
      display::push();
      lastDraw = now;
    }

    const navigation::EventData ev = navigation::poll();
    if (ev.type == navigation::Event::Back || ev.type == navigation::Event::Select) {
      break;
    }
    delay(20);
  }
  navigation::waitRelease();
}

void browseFile(const std::string& path, const std::string& name, bool preferSD) {
  powertimer::Hold hold;

  pc::Reader reader;
  if (!reader.open(path, preferSD)) {
    ui::message("Sfoglia", {"!File non leggibile o non PCAP.", name}, "Capture");
    return;
  }
  if (!reader.supportedLinkType()) {
    reader.close();
    ui::message("Sfoglia", {"!Linktype non 802.11.", "Attesi 105 oppure 127."}, "Capture");
    return;
  }

  // Solo gli offset: i frame vengono riletti quando servono al disegno.
  // La riserva e' proporzionale al file (minimo 16 byte per record) e non
  // supera il limite dell'indice, per non occupare 80 KB sui file piccoli.
  std::vector<uint32_t> offsets;
  size_t estimate = reader.fileSize() / 16 + 1;
  if (estimate > pc::kMaxIndexFrames) {
    estimate = pc::kMaxIndexFrames;
  }
  offsets.reserve(std::min<size_t>(estimate, 2048));
  pc::PcapFrame frame;
  bool truncatedIndex = false;
  bool aborted = false;
  uint32_t lastDraw = 0;
  uint32_t tick = 0;

  navigation::waitRelease();
  while (reader.next(frame)) {
    offsets.push_back(reader.frameOffset());
    if (offsets.size() >= pc::kMaxIndexFrames) {
      truncatedIndex = true;
      break;
    }
    if ((++tick & 0x3F) == 0) {
      const navigation::EventData ev = navigation::poll();
      if (ev.type == navigation::Event::Back) {
        aborted = true;
        break;
      }
      const uint32_t now = millis();
      if (now - lastDraw > 150) {
        drawScanProgress(name, offsets.size(), reader.position(), reader.fileSize());
        lastDraw = now;
      }
      delay(1);
    }
  }
  const bool damaged = reader.truncated();

  if (aborted) {
    reader.close();
    return;
  }
  if (offsets.empty()) {
    reader.close();
    ui::message("Sfoglia", {"Nessun frame leggibile.", name}, "Capture");
    return;
  }

  constexpr int kRowH = 10;
  const int rows = std::max(1, (display::height() - 21 - 12) / kRowH);
  const int maxTop = std::max(0, static_cast<int>(offsets.size()) - rows);
  int selected = 0;
  int top = 0;
  bool dirty = true;
  lastDraw = 0;

  for (;;) {
    const uint32_t now = millis();
    if (dirty || now - lastDraw > 400) {
      drawBrowser(reader, offsets, name, selected, top, rows, truncatedIndex, damaged);
      dirty = false;
      lastDraw = now;
    }

    const navigation::EventData ev = navigation::poll();
    if (ev.type == navigation::Event::Back) {
      break;
    }
    if (ev.type == navigation::Event::Select) {
      showHex(reader, offsets[selected], name, static_cast<uint32_t>(selected + 1));
      dirty = true;
      continue;
    }
    if (ev.type == navigation::Event::Up) {
      selected = std::max(0, selected - 1);
      dirty = true;
    } else if (ev.type == navigation::Event::Down) {
      selected = std::min(static_cast<int>(offsets.size()) - 1, selected + 1);
      dirty = true;
    } else if (ev.type == navigation::Event::Left) {
      selected = std::max(0, selected - rows);
      dirty = true;
    } else if (ev.type == navigation::Event::Right) {
      selected = std::min(static_cast<int>(offsets.size()) - 1, selected + rows);
      dirty = true;
    }

    if (selected < top) {
      top = selected;
    }
    if (selected >= top + rows) {
      top = selected - rows + 1;
    }
    top = std::max(0, std::min(top, maxTop));
    delay(20);
  }

  reader.close();
  navigation::waitRelease();
}

void browseCapture() {
  std::vector<CaptureItem> items;
  bool preferSD = false;
  if (!collectCaptures(items, true, true, preferSD)) {
    ui::message("Sfoglia", {"!Nessun filesystem disponibile."}, "Capture");
    return;
  }
  if (items.empty()) {
    ui::message("Sfoglia", {"Nessuna cattura in /captures.", "Registra con WiFi > Sniffer 802.11."},
                "Capture");
    return;
  }

  const int choice = pickCapture("Sfoglia catture", items);
  if (choice < 0) {
    return;
  }
  if (items[choice].hc) {
    ui::message("Sfoglia",
                {"File di output hc22000.", "Non e' una cattura PCAP.",
                 "Non puo' essere aperto in questo modulo."},
                "Capture");
    return;
  }

  const std::string path = pc::joinPath(pc::kCaptureDir, items[choice].name);
  browseFile(path, items[choice].name, preferSD);
}

void drawWork(const std::string& title, const std::string& file,
              const std::string& phase, float fraction, size_t offset,
              size_t total) {
  display::clear(display::kPaper);
  display::header(title, "Capture");
  display::text(4, 26, shorten(file, 38), display::kInk, 0);
  display::text(4, 42, phase, display::kMuted, 0);
  display::progressBar(12, 62, display::width() - 24, 8, fraction, display::kAccent);
  char percent[8];
  std::snprintf(percent, sizeof(percent), "%u%%",
                static_cast<unsigned>(fraction * 100.0f + 0.5f));
  display::textCentered(80, percent, display::kInk, 2);
  display::textCentered(94, formatSize(offset) + " / " + formatSize(total),
                        display::kMuted, 0);
  display::footer("ESC = annulla");
  display::push();
}

bool runConvert(const std::string& in, const std::string& out, bool preferSD,
                pc::HcStats& stats, bool& aborted, const std::string& name) {
  aborted = false;
  uint32_t lastDraw = 0;
  powertimer::Hold hold;

  return pc::convertToHc22000(
      in, out, stats, preferSD,
      [&](uint8_t pass, size_t offset, size_t total) -> bool {
        navigation::update();
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
          aborted = true;
          return false;
        }
        const uint32_t now = millis();
        if (now - lastDraw > 150) {
          float part = total > 0 ? static_cast<float>(offset) / static_cast<float>(total)
                                 : 0.0f;
          part = std::max(0.0f, std::min(1.0f, part));
          const float fraction = pass == 0 ? part * 0.5f : 0.5f + part * 0.5f;
          drawWork("hc22000", name,
                   pass == 0 ? "Fase 1/2: SSID" : "Fase 2/2: EAPOL", fraction,
                   offset, total);
          lastDraw = now;
        }
        delay(1);
        return true;
      });
}

void convertCapture() {
  std::vector<CaptureItem> items;
  bool preferSD = false;
  if (!collectCaptures(items, true, false, preferSD)) {
    ui::message("hc22000", {"!Nessun filesystem disponibile."}, "Capture");
    return;
  }
  if (items.empty()) {
    ui::message("hc22000", {"Nessuna cattura .pcap in /captures."}, "Capture");
    return;
  }

  const int choice = pickCapture("Converti in hc22000", items);
  if (choice < 0) {
    return;
  }

  const std::string in = pc::joinPath(pc::kCaptureDir, items[choice].name);
  const std::string out = pc::hc22000Path(in);
  storage::mkdir(pc::kCaptureDir, preferSD);

  pc::HcStats stats;
  bool aborted = false;
  const bool ok = runConvert(in, out, preferSD, stats, aborted, items[choice].name);

  if (!ok) {
    if (aborted) {
      ui::message("hc22000", {"Operazione annullata."}, "Capture");
    } else {
      ui::message("hc22000", {"!Conversione non riuscita.", "Verifica spazio e file di input."},
                  "Capture");
    }
    return;
  }

  std::vector<std::string> lines;
  lines.push_back(">Frame: " + std::to_string(stats.frames));
  lines.push_back("EAPOL: " + std::to_string(stats.eapol));
  lines.push_back("PMKID: " + std::to_string(stats.pmkid));
  lines.push_back("Handshake: " + std::to_string(stats.handshakes));
  lines.push_back("Senza SSID: " + std::to_string(stats.skippedNoEssid));
  lines.push_back("File: " + fileName(out));
  if (stats.pmkid + stats.handshakes == 0) {
    lines.push_back("!Nessun hash trovato (file vuoto).");
  }
  ui::message("hc22000", lines, "Capture");
}

void extractCapture() {
  std::vector<CaptureItem> items;
  bool preferSD = false;
  if (!collectCaptures(items, true, false, preferSD)) {
    ui::message("Estrai EAPOL", {"!Nessun filesystem disponibile."}, "Capture");
    return;
  }
  if (items.empty()) {
    ui::message("Estrai EAPOL", {"Nessuna cattura .pcap in /captures."}, "Capture");
    return;
  }

  const int choice = pickCapture("Estrai solo EAPOL", items);
  if (choice < 0) {
    return;
  }

  const std::string in = pc::joinPath(pc::kCaptureDir, items[choice].name);
  const std::string out = pc::eapolPcapPath(in);
  storage::mkdir(pc::kCaptureDir, preferSD);

  pc::HcStats stats;
  bool aborted = false;
  uint32_t lastDraw = 0;
  powertimer::Hold hold;
  const bool ok = pc::extractEapol(
      in, out, stats, preferSD,
      [&](uint8_t, size_t offset, size_t total) -> bool {
        navigation::update();
        const navigation::EventData ev = navigation::poll();
        if (ev.type == navigation::Event::Back) {
          aborted = true;
          return false;
        }
        const uint32_t now = millis();
        if (now - lastDraw > 150) {
          const float fraction =
              total > 0
                  ? std::max(0.0f, std::min(1.0f, static_cast<float>(offset) /
                                                       static_cast<float>(total)))
                  : 0.0f;
          drawWork("Estrai EAPOL", items[choice].name, "Filtro frame EAPOL",
                   fraction, offset, total);
          lastDraw = now;
        }
        delay(1);
        return true;
      });

  if (!ok) {
    if (aborted) {
      ui::message("Estrai EAPOL", {"Operazione annullata."}, "Capture");
    } else {
      ui::message("Estrai EAPOL", {"!Estrazione non riuscita."}, "Capture");
    }
    return;
  }

  std::vector<std::string> lines;
  lines.push_back(">Frame: " + std::to_string(stats.frames));
  lines.push_back("EAPOL scritti: " + std::to_string(stats.eapol));
  lines.push_back("File: " + fileName(out));
  if (stats.eapol == 0) {
    lines.push_back("Nessun EAPOL nella cattura.");
  }
  ui::message("Estrai EAPOL", lines, "Capture");
}

void deleteHc() {
  std::vector<CaptureItem> items;
  bool preferSD = false;
  if (!collectCaptures(items, false, true, preferSD)) {
    ui::message("Elimina hc22000", {"!Nessun filesystem disponibile."}, "Capture");
    return;
  }
  if (items.empty()) {
    ui::message("Elimina hc22000", {"Nessun file .hc22000 in /captures."}, "Capture");
    return;
  }

  const int choice = pickCapture("Elimina .hc22000", items);
  if (choice < 0) {
    return;
  }

  const std::string path = pc::joinPath(pc::kCaptureDir, items[choice].name);
  if (!ui::confirm("Elimina file",
                   {items[choice].name, "Vuoi cancellare questo file?", "Azione irreversibile."},
                   "Capture")) {
    return;
  }

  const bool ok = storage::removeFile(path, preferSD);
  if (!ok) {
    logger::warn("Capture: eliminazione fallita per %s", path.c_str());
  }
  ui::message("Elimina file",
              {ok ? ">File eliminato." : "!Eliminazione non riuscita."}, "Capture");
}

}  // namespace

void runMenu() {
  if (!storage::hasSD() && !storage::hasLittleFS()) {
    ui::message("Capture", {"!Nessun filesystem disponibile."}, "Menu principale");
    return;
  }

  MenuManager menu("Capture", "Menu principale");
  menu.addItem("Sfoglia", "scan", [] { browseCapture(); });
  menu.addItem("Converti in hc22000", "lock", [] { convertCapture(); });
  menu.addItem("Estrai solo EAPOL", "wifi", [] { extractCapture(); });
  menu.addItem("Elimina .hc22000", "settings", [] { deleteHc(); });
  menu.setHint("ESC=indietro  ;.=nav  ENTER=apri");
  menu.run(true);
}

}  // namespace pcaptools_ui

#endif  // ENABLE_MODULE_PCAPTOOLS
