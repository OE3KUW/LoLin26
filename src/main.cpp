/***************************************************************************************
//
//                                  L o L i n   2 6
//
//                                                                      қuran feb 2026
***************************************************************************************/

/**************************************************************************************
    LoLin32 (ESP32) - Minimal reusable STA + WebSocket template
    Wolfgang Uriel / HTL reuse template
***************************************************************************************/

#include <Arduino.h>

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

#include <LittleFS.h>

// JSON parsing/serialization
#include <ArduinoJson.h>

// -----------------------------------------------------------------
// WiFi credentials
// -----------------------------------------------------------------
// Keep it simple: set your WiFi here or provide via build flags.
// Example (platformio.ini):
//   build_flags = -DWIFI_SSID=\"MySSID\" -DWIFI_PASS=\"MyPass\"

//#ifndef WIFI_SSID
  #define WIFI_SSID "..."
//#endif

//#ifndef WIFI_PASS
  #define WIFI_PASS "..."
//#endif

// -----------------------------------------------------------------
// Hardware
// -----------------------------------------------------------------

// NOTE: On many LoLin32 boards the on-board LED is GPIO5 (often active-low).
// If your board differs, change this pin.
static constexpr uint8_t ONBOARD_LED_PIN = 5;

// Active-low LED handling (GPIO5 is commonly inverted on LoLin32)
static constexpr bool LED_ACTIVE_LOW = true;

// -----------------------------------------------------------------
// Timer interrupt configuration
// -----------------------------------------------------------------
// We use a 0.1 ms tick like in your base project:
// - timer prescaler 80 => 1 MHz (1 us per tick)
// - alarm at 100       => 100 us = 0.1 ms

static constexpr uint32_t TIMER_PRESCALER = 80;
static constexpr uint32_t TIMER_ALARM_US  = 100; // 100 us

// 5 seconds / 0.1 ms = 50,000 ticks
static constexpr uint32_t TICKS_5S = 50000;

// -----------------------------------------------------------------
// Web server
// -----------------------------------------------------------------

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// -----------------------------------------------------------------
// State machine
// -----------------------------------------------------------------

#define STATE_OFF                       0
#define STATE_ON                        1

static volatile bool flagFiveSeconds = false;   // set in ISR, consumed in loop
static volatile bool ledRequested = false;      // set by WebSocket handler
static volatile bool ledRequestPending = false; // set by WebSocket handler

static uint32_t state = STATE_OFF;

// time base (simple "uptime" in seconds)
static volatile uint32_t uptimeSeconds = 0;

// -----------------------------------------------------------------
// Helper: LED write with optional inversion
// -----------------------------------------------------------------

static inline void writeOnboardLed(bool on)
{
    if (LED_ACTIVE_LOW)
    {
        digitalWrite(ONBOARD_LED_PIN, on ? LOW : HIGH);
    }
    else
    {
        digitalWrite(ONBOARD_LED_PIN, on ? HIGH : LOW);
    }
}

// -----------------------------------------------------------------
// LittleFS
// -----------------------------------------------------------------

static void initLittleFS()
{
    if (!LittleFS.begin(true))
    {
        Serial.println("[FS] LittleFS mount failed");
        return;
    }
    Serial.println("[FS] LittleFS mounted");
}

// -----------------------------------------------------------------
// WiFi STA
// -----------------------------------------------------------------

static void initWiFi()
{
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);

    delay(500);                 // give radio/stack time


    WiFi.begin(WIFI_SSID, WIFI_PASS);

    Serial.print("[WiFi] Connecting");
    uint8_t tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 30)
    {
        delay(500);
        Serial.print('.');
        tries++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.print("[WiFi] Connected. IP: ");
        Serial.println(WiFi.localIP());
    }
    else
    {
        Serial.println("[WiFi] Not connected (check SSID/PASS)");
    }
}

// -----------------------------------------------------------------
// Template processor (%STATE%)
// -----------------------------------------------------------------

static String processor(const String &var)
{
    if (var == "STATE")
    {
        return (state == STATE_ON) ? "ON" : "OFF";
    }
    return String();
}

// -----------------------------------------------------------------
// WebSocket: broadcast helpers
// -----------------------------------------------------------------

static void wsBroadcastLedState()
{
    StaticJsonDocument<96> doc;  // 96 Byte werden hier reserviert. 
    doc["type"]  = "led";
    doc["state"] = (state == STATE_ON);

    String out;
    serializeJson(doc, out);
    ws.textAll(out);
}

static void wsBroadcastUptimeMmSs()
{
    const uint32_t seconds = uptimeSeconds; // snapshot
    const uint32_t min = seconds / 60;
    const uint32_t sec = seconds % 60;

    StaticJsonDocument<128> doc;
    doc["type"] = "time";
    doc["min"]  = min;
    doc["sec"]  = sec;

    String out;
    serializeJson(doc, out);
    ws.textAll(out);
}

// -----------------------------------------------------------------
// WebSocket: incoming JSON handler
// -----------------------------------------------------------------

static void handleWebSocketMessage(void *arg, uint8_t *data, size_t len)
{
    AwsFrameInfo *info = reinterpret_cast<AwsFrameInfo *>(arg);
    if (!(info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT))
    {
        return;
    }

  // Make a safe copy
    String msg;
    msg.reserve(len + 1);
    for (size_t i = 0; i < len; i++) msg += static_cast<char>(data[i]);

  // Preferred: JSON messages
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, msg);
    if (err)
    {
        Serial.print("[WS] JSON parse failed: ");
        Serial.println(err.c_str());
        return;
    }

    const char *type = doc["type"] | "";

    // Button command from browser
    if (strcmp(type, "button") == 0)
    {
    // Desired LED state
    const bool desired = doc["state"] | false;
    ledRequested = desired;
    ledRequestPending = true;

    // Browser time (min/sec) => print to Serial
    const uint32_t bMin = doc["browser"]["min"] | 0;
    const uint32_t bSec = doc["browser"]["sec"] | 0;
    Serial.printf("[BrowserTime] %lu:%02lu\n",
                  static_cast<unsigned long>(bMin),
                  static_cast<unsigned long>(bSec));
    }
}

static void onEvent(AsyncWebSocket *serverPtr,
                    AsyncWebSocketClient *client,
                    AwsEventType type,
                    void *arg,
                    uint8_t *data,
                    size_t len)
{
    (void)serverPtr;

    switch (type)
    {
        case WS_EVT_CONNECT:
            Serial.printf("[WS] Client #%u connected from %s\n",
                    client->id(), client->remoteIP().toString().c_str());
            // Immediately sync the new client
            wsBroadcastLedState();
            wsBroadcastUptimeMmSs();
        break;

        case WS_EVT_DISCONNECT:
            Serial.printf("[WS] Client #%u disconnected\n", client->id());
        break;

        case WS_EVT_DATA:
            handleWebSocketMessage(arg, data, len);
        break;

        case WS_EVT_PONG:
        case WS_EVT_ERROR:
        default:
        break;
    }
}

static void initWebSocket()
{
    ws.onEvent(onEvent);
    server.addHandler(&ws);
}

// -----------------------------------------------------------------
// Timer ISR
// -----------------------------------------------------------------

static hw_timer_t *timer = nullptr;

static void IRAM_ATTR onTimer()
{
    static uint32_t tick5s = 0;
    static uint32_t tick1s = 0;

    tick5s++;
    tick1s++;

    if (tick1s >= 10000) // 1 second (10,000 * 0.1 ms)
    {
        tick1s = 0;
        uptimeSeconds++;
    }

    if (tick5s >= TICKS_5S)
    {
        tick5s = 0;
        flagFiveSeconds = true;
    }
}

static void initTimer()
{
    timer = timerBegin(0, TIMER_PRESCALER, true);
    timerAttachInterrupt(timer, &onTimer, true);
    timerAlarmWrite(timer, TIMER_ALARM_US, true);
    timerAlarmEnable(timer);
}

// -----------------------------------------------------------------
// Setup / Loop
// -----------------------------------------------------------------

void setup()
{
    Serial.begin(115200);
    delay(50);
    Serial.println("\n[BOOT] Minimal LoLin32 template");

    pinMode(ONBOARD_LED_PIN, OUTPUT);
    writeOnboardLed(false);

    initTimer();
    initLittleFS();   // File System
    initWiFi();
    initWebSocket();

    // Logo: explicit endpoint with a lambda
    server.on("/logo", HTTP_GET, [](AsyncWebServerRequest *request)
    {
        request->send(LittleFS, "/logo.png", "image/png");
    });

    // Optional: stop browser favicon spam (return "No Content")
    server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request)
    {
        request->send(204);
    });

    // Root page with template processor
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
    {
        request->send(LittleFS, "/index.html", "text/html", false, processor);
    });

    // Serve static assets (CSS/JS/etc.) from LittleFS
    server.serveStatic("/", LittleFS, "/");

    
    server.begin();
    Serial.println("[HTTP] Server started");
}

void loop()
{
    ws.cleanupClients();  // entfernt nicht mehr vorhandene Clients

    // --- Apply pending LED requests (set by WebSocket handler) ------
    if (ledRequestPending)
    {
        ledRequestPending = false;

        state = ledRequested ? STATE_ON : STATE_OFF;
        writeOnboardLed(state == STATE_ON);

        // Broadcast to ALL clients so everyone stays in sync
        wsBroadcastLedState();
    }

    // --- Every 5 seconds: send time as JSON to browser --------------
    if (flagFiveSeconds)
    {
        flagFiveSeconds = false;
        wsBroadcastUptimeMmSs();
    }

    // --- Tiny state machine placeholder -----------------------------
    switch (state)
    {
        case STATE_OFF:
            // nothing
        break;

        case STATE_ON:
            // nothing
        break;
    }
}
