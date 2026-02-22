/***************************************************************************************
//
//                                  L o L i n   2 6
//
//                                                                      қuran feb 2026
***************************************************************************************/



/*******************************************************************
    LoLin32 (ESP32) - Minimal reusable STA + WebSocket template

    Goals (simplified project skeleton)
    - WiFi STA mode (multiple clients may connect)
    - AsyncWebServer + AsyncWebSocket
    - LittleFS (NOT SPIFFS)
    - One single ON/OFF button in the browser toggles the on-board LED
      * The browser sends a JSON message to the ESP32
      * "processor()" is still used for %STATE% placeholders
      * The WebSocket handler only sets a flag (no direct hardware writes)
      * The loop() runs a tiny state machine (STATE_ON / STATE_OFF)
    - A hardware timer interrupt runs periodically and sets a flag every 5 s
      * loop() detects the flag and broadcasts JSON {min,sec} to ALL clients
      * JS unwraps JSON and updates an element in the HTML
    - Additionally: When the button is pressed, JS reads the browser time
      and sends {min,sec} to the ESP32, which prints it to Serial.

    Wolfgang Uriel / HTL reuse template
*******************************************************************/

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

#ifndef WIFI_SSID
  #define WIFI_SSID "YOUR_SSID"
#endif

#ifndef WIFI_PASS
  #define WIFI_PASS "YOUR_PASSWORD"
#endif

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

enum SystemState : uint8_t
{
  	STATE_OFF = 0,
  	STATE_ON  = 1
};

static volatile bool g_flagFiveSeconds = false;   // set in ISR, consumed in loop
static volatile bool g_ledRequested = false;      // set by WebSocket handler
static volatile bool g_ledRequestPending = false; // set by WebSocket handler

static SystemState g_state = STATE_OFF;

// time base (simple "uptime" in seconds)
static volatile uint32_t g_uptimeSeconds = 0;

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
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("[WiFi] Connecting");
  uint8_t tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20)
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
    return (g_state == STATE_ON) ? "ON" : "OFF";
  }
  return String();
}

// -----------------------------------------------------------------
// WebSocket: broadcast helpers
// -----------------------------------------------------------------

static void wsBroadcastLedState()
{
  StaticJsonDocument<96> doc;
  doc["type"]  = "led";
  doc["state"] = (g_state == STATE_ON);

  String out;
  serializeJson(doc, out);
  ws.textAll(out);
}

static void wsBroadcastUptimeMmSs()
{
  const uint32_t seconds = g_uptimeSeconds; // snapshot
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

  // Backward compatibility: accept the old "bON" / "bOFF" commands
  if (msg == "bON")
  {
    g_ledRequested = true;
    g_ledRequestPending = true;
    return;
  }
  if (msg == "bOFF")
  {
    g_ledRequested = false;
    g_ledRequestPending = true;
    return;
  }

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
    g_ledRequested = desired;
    g_ledRequestPending = true;

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

static hw_timer_t *g_timer = nullptr;

static void IRAM_ATTR onTimer()
{
  static uint32_t tick5s = 0;
  static uint32_t tick1s = 0;

  tick5s++;
  tick1s++;

  if (tick1s >= 10000) // 1 second (10,000 * 0.1 ms)
  {
    tick1s = 0;
    g_uptimeSeconds++;
  }

  if (tick5s >= TICKS_5S)
  {
    tick5s = 0;
    g_flagFiveSeconds = true;
  }
}

static void initTimer()
{
  g_timer = timerBegin(0, TIMER_PRESCALER, true);
  timerAttachInterrupt(g_timer, &onTimer, true);
  timerAlarmWrite(g_timer, TIMER_ALARM_US, true);
  timerAlarmEnable(g_timer);
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
  initLittleFS();
  initWiFi();
  initWebSocket();

  // Root page with template processor
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
  {
    request->send(LittleFS, "/index.html", "text/html", false, processor);
  });

  // Serve static assets (CSS/JS/etc.) from LittleFS
  server.serveStatic("/", LittleFS, "/");

  // Logo: explicit endpoint with a lambda
  server.on("/logo", HTTP_GET, [](AsyncWebServerRequest *request)
  {
    request->send(LittleFS, "/logo.png", "image/png");
  });

  server.begin();
  Serial.println("[HTTP] Server started");
}

void loop()
{
  ws.cleanupClients();

  // --- Apply pending LED requests (set by WebSocket handler) ------
  if (g_ledRequestPending)
  {
    g_ledRequestPending = false;

    g_state = g_ledRequested ? STATE_ON : STATE_OFF;
    writeOnboardLed(g_state == STATE_ON);

    // Broadcast to ALL clients so everyone stays in sync
    wsBroadcastLedState();
  }

  // --- Every 5 seconds: send time as JSON to browser --------------
  if (g_flagFiveSeconds)
  {
    g_flagFiveSeconds = false;
    wsBroadcastUptimeMmSs();
  }

  // --- Tiny state machine placeholder -----------------------------
  switch (g_state)
  {
    case STATE_OFF:
      // nothing
      break;

    case STATE_ON:
      // nothing
      break;
  }
}
