/*
====================================================================
FUNCTIONAL SPECIFICATION DOCUMENT (FSD)
====================================================================

Purpose
- Monitor battery voltage with an ESP32-C3 and respond to low-voltage
  conditions while providing local and remote visibility.

Inputs
- ADC reading from BATTERY_PIN via voltage divider (R1/R2).
- WiFi credentials via secrets.h.

Outputs
- Relay control on RELAY_PIN (disable on low voltage).
- OLED status display (if available, SSD1306 72x40 over I2C).
- Serial and WebSerial logging.
- HTTP GET endpoint `/voltage` (JSON/JSONP).
- HTTP GET endpoint `/status` (JSON status snapshot).
- HTTP GET endpoint `/relay` (toggle or set relay state).
- HTTP POST to local endpoint when low voltage triggers.
- SMS alert via Twilio when low voltage triggers.

Functional Requirements
1) Voltage Measurement
   - Sample ADC and compute battery voltage using divider ratio.
   - Update `lastVoltage` each loop.
2) Display
   - If OLED initializes, show voltage and status.
   - If OLED is absent, continue program without blocking.
  - Show WARNING when voltage <= 16.4V.
  - Show LOW VOLTAGE when voltage <= 15.6V.
3) Low-Voltage Handling
   - At or below 15.6V: disable relay, send SMS, send local POST.
   - Prevent repeated alerts until voltage rises above 16.1V.
4) Web Interface
   - Host HTTP AsyncWebServer on port 80.
   - `/voltage` returns JSON: {"battery_voltage": XX.XX}
   - `/status` returns JSON: voltage, relay state, alert state, WiFi state.
   - `/relay` toggles or sets relay state via query param `state=on|off|toggle`.
5) Connectivity
   - Attempt WiFi for up to 10 seconds; continue offline if not connected.

Non-Functional Requirements
- Loop period ~2 seconds.
- Use 12-bit ADC scaling for ESP32 (0-4095) at 3.3V reference.
- Keep network operations non-blocking where possible.

Assumptions
- Correct voltage divider sizing for ADC input limits.
- Relay logic level matches hardware.
- OLED uses SSD1306 72x40 over I2C.

Hardware Defaults (ESP32-C3 Super Mini)
- BATTERY_PIN: GPIO4
- RELAY_PIN: GPIO0
- I2C SDA/SCL: GPIO5/GPIO6

====================================================================
*/

/*
====================================================================
USE3D PROMPT — SYSTEM DESCRIPTION
====================================================================

Create an ESP32-based monitoring system with the following behavior:

FUNCTIONALITY
- Continuously monitor battery voltage using an ESP32 ADC pin and
  a properly sized voltage divider.
- Detect when battery voltage drops to or below 15.6 volts.

LOW-VOLTAGE RESPONSE
When the battery voltage reaches 15.6V or lower:
- Immediately disable the relay output to protect the system.
- Send a single SMS alert using the Twilio API.
- Send an HTTP POST request to http://192.168.1.3 containing the
  current battery voltage in JSON format.
- Prevent repeated alerts until voltage recovers above 16.1V.

NETWORK & INTERFACE
- Connect to WiFi using configured credentials.
- Host a web server on the ESP32.
- Stream serial output to a browser in real time for monitoring.

DISPLAY
- Show live battery voltage and system status on an internal OLED.

OPERATIONAL CONSTRAINTS
- A voltage divider must be used to ensure ADC input voltage
  remains within ESP32 limits.
- Relay logic level may vary depending on hardware.
- Alert state resets only after voltage rises above the cutoff
  plus hysteresis.

====================================================================
*/

/*
====================================================================
WIRING DIAGRAM (TEXT)
====================================================================

BATTERY + -> R1 (100k) -> NODE -> R2 (10k) -> BATTERY - (GND)
NODE -> ESP32 GPIO4 (ADC input)
BATTERY - -> ESP32 GND (common ground)

RELAY MODULE:
  VCC -> ESP32 5V (or 3.3V if module supports it)
  GND -> ESP32 GND
  IN  -> ESP32 GPIO0
  NOTE -> Relay control on GPIO0 (assumes active HIGH)

OLED (SSD1306 I2C 0.42", ESP32-C3 Super Mini):
  VCC -> ESP32 3.3V
  GND -> ESP32 GND
  SDA -> ESP32 GPIO5
  SCL -> ESP32 GPIO6

====================================================================
*/

/*
====================================================================
WIRING DIAGRAM (ASCII)
====================================================================

Battery + ---- R1 100k ----+---- R2 10k ---- Battery - (GND)
                           |
                           +---- GPIO4 (ADC)
Battery - -----------------+---- ESP32 GND

Relay Module:
  VCC ---- ESP32 5V (or 3.3V)
  GND ---- ESP32 GND
  IN  ---- ESP32 GPIO0
  NOTE --- Relay control on GPIO0 (assumes active HIGH)

OLED (SSD1306 I2C 0.42", ESP32-C3 Super Mini):
  VCC ---- 3.3V
  GND ---- GND
  SDA ---- GPIO5
  SCL ---- GPIO6

====================================================================
*/

#include <WiFi.h>
#include "secrets.h"
#include <HTTPClient.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <WebSerial.h>
#include <Wire.h>
#include <U8g2lib.h>

// ================= OLED CONFIG =================
// ESP32-C3 Super Mini 0.42" OLED (I2C) pins from hello-world example.
#define OLED_SDA 5
#define OLED_SCL 6

U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
bool oledAvailable = true;

// ================= CONFIG =================
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// Twilio
const char* twilioSID   = "ACxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
const char* twilioToken = "your_auth_token";
const char* twilioFrom  = "+1234567890";
const char* twilioTo    = "+1987654321";


const char index_html[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0" />
    <title>ESP32 Voltage Monitor</title>
    <script src="https://cdn.tailwindcss.com"></script>
    <link rel="preconnect" href="https://fonts.googleapis.com">
    <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
    <link href="https://fonts.googleapis.com/css2?family=Roboto:wght@400;500;700&display=swap" rel="stylesheet">
    <style>
      body { font-family: "Roboto", sans-serif; }
    </style>
  </head>
  <body class="min-h-screen bg-gradient-to-br from-slate-950 via-slate-900 to-zinc-900 text-slate-100">
    <main class="mx-auto flex min-h-screen w-full max-w-5xl flex-col gap-8 px-6 py-10">
      <header class="flex flex-col gap-2">
        <p class="text-sm uppercase tracking-[0.3em] text-amber-300">ESP32 Monitor</p>
        <h1 class="text-3xl font-bold sm:text-4xl">Battery Voltage Status</h1>
        <p class="max-w-2xl text-sm text-slate-300">
          Live telemetry from the ESP32 over local HTTP.
        </p>
      </header>

      <section class="grid gap-6 md:grid-cols-[2fr_1fr]">
        <div class="rounded-3xl border border-slate-700/60 bg-slate-900/60 p-6 shadow-xl">
          <div class="flex items-center justify-between">
            <h2 class="text-lg font-semibold text-slate-200">Battery Voltage</h2>
            <span id="statusBadge" class="rounded-full bg-emerald-400/20 px-4 py-1 text-xs font-semibold uppercase tracking-wide text-emerald-200">OK</span>
          </div>
          <div class="mt-6 flex items-end gap-4">
            <p id="voltageValue" class="text-5xl font-bold tracking-tight">--</p>
            <p class="pb-2 text-sm text-slate-400">volts</p>
          </div>
          <p id="statusMessage" class="mt-4 text-sm text-slate-300">
            Waiting for data...
          </p>
        </div>

        <div class="rounded-3xl border border-slate-700/60 bg-slate-900/60 p-6 shadow-xl">
          <h2 class="text-lg font-semibold text-slate-200">System Signals</h2>
          <div class="mt-4 space-y-3 text-sm text-slate-300">
            <div class="flex items-center justify-between">
              <span>Relay</span>
              <span id="relayStatus" class="font-semibold text-slate-100">--</span>
            </div>
            <div class="flex items-center justify-between">
              <span>Alert State</span>
              <span id="alertStatus" class="font-semibold text-slate-100">--</span>
            </div>
            <div class="flex items-center justify-between">
              <span>WiFi</span>
              <span id="wifiStatus" class="font-semibold text-slate-100">--</span>
            </div>
            <div class="pt-2 text-xs text-slate-400">
              Last update: <span id="lastUpdate">--</span>
            </div>
          </div>
        </div>
      </section>
    </main>

    <script>
      const statusBadge = document.getElementById("statusBadge");
      const voltageValue = document.getElementById("voltageValue");
      const statusMessage = document.getElementById("statusMessage");
      const relayStatus = document.getElementById("relayStatus");
      const alertStatus = document.getElementById("alertStatus");
      const wifiStatus = document.getElementById("wifiStatus");
      const lastUpdate = document.getElementById("lastUpdate");

      const badgeStyles = {
        OK: "bg-emerald-400/20 text-emerald-200",
        WARNING: "bg-amber-400/20 text-amber-200",
        LOW: "bg-rose-500/20 text-rose-200",
        OFFLINE: "bg-slate-600/30 text-slate-200",
      };

      function setBadge(status) {
        statusBadge.className = "rounded-full px-4 py-1 text-xs font-semibold uppercase tracking-wide";
        statusBadge.classList.add(...badgeStyles[status].split(" "));
        statusBadge.textContent = status;
      }

      async function refresh() {
        try {
          const response = await fetch("/status", { cache: "no-store" });
          if (!response.ok) throw new Error("bad response");
          const data = await response.json();
          const status = data.status || "OK";

          voltageValue.textContent = data.battery_voltage?.toFixed(2) ?? "--";
          statusMessage.textContent = data.message || "Status updated.";
          relayStatus.textContent = data.relay_on ? "ON" : "OFF";
          alertStatus.textContent = data.alert_sent ? "TRIGGERED" : "ARMED";
          wifiStatus.textContent = data.wifi || "offline";
          lastUpdate.textContent = new Date().toLocaleTimeString();
          setBadge(status);
        } catch (err) {
          setBadge("OFFLINE");
          statusMessage.textContent = "Unable to reach device. Check network connection.";
          wifiStatus.textContent = "offline";
        }
      }

      refresh();
      setInterval(refresh, 2000);
    </script>
  </body>
</html>
)HTML";

// Pins
#define BATTERY_PIN 4
#define RELAY_PIN   0

// Voltage divider values
float R1 = 100000.0;
float R2 = 10000.0;

// Voltage threshold
float cutoffVoltage = 15.6;
float warningVoltage = 16.4;

// ==========================================

AsyncWebServer server(80);
bool alertSent = false;
float lastVoltage = 0.0;
bool relayOn = true;
bool webSerialEnabled = false;
const size_t historySize = 120;
float voltageHistory[historySize] = {};
size_t historyIndex = 0;
bool historyFilled = false;

float readBatteryVoltage() {
  int raw = analogRead(BATTERY_PIN);
  float adcVoltage = (raw / 4095.0) * 3.3;
  return adcVoltage * ((R1 + R2) / R2);
}

void updateOLED(float voltage) {
  if (!oledAvailable) {
    return;
  }

  u8g2.clearBuffer();

  const int displayWidth = 72;
  const int line1Y = 10;
  const int line2Y = 32;
  const int line3Y = 39;

  auto drawCentered = [&](const char *text, int y, const uint8_t *font) {
    u8g2.setFont(font);
    int width = u8g2.getStrWidth(text);
    int x = (displayWidth - width) / 2;
    if (x < 0) {
      x = 0;
    }
    u8g2.drawStr(x, y, text);
  };

  // Title
  drawCentered("Battery", line1Y, u8g2_font_6x10_tr);

  // Voltage (large)
  char voltageText[16];
  snprintf(voltageText, sizeof(voltageText), "%.2fV", voltage);
  drawCentered(voltageText, line2Y, u8g2_font_logisoso20_tr);

  // Status line
  if (voltage <= cutoffVoltage) {
    drawCentered("LOW VOLT", line3Y, u8g2_font_6x10_tr);
  } else if (voltage <= warningVoltage) {
    drawCentered("WARNING", line3Y, u8g2_font_6x10_tr);
  } else {
    drawCentered("STATUS OK", line3Y, u8g2_font_6x10_tr);
  }

  u8g2.sendBuffer();
}

void sendTwilioSMS(String message) {
  if (String(twilioSID) == "ACxxxxxxxxxxxxxxxxxxxxxxxxxxxx") {
    Serial.println("Twilio SID not configured, skipping SMS.");
    return;
  }
  HTTPClient http;
  String url = "https://api.twilio.com/2010-04-01/Accounts/" +
               String(twilioSID) + "/Messages.json";

  http.begin(url);
  http.setAuthorization(twilioSID, twilioToken);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "From=" + String(twilioFrom) +
                "&To=" + String(twilioTo) +
                "&Body=" + message;

  http.POST(body);
  http.end();
}

void sendLocalPOST(float voltage) {
  HTTPClient http;
  http.begin("http://192.168.1.3");
  http.addHeader("Content-Type", "application/json");

  String payload = "{\"battery_voltage\": " + String(voltage, 2) + "}";
  http.POST(payload);
  http.end();
}

void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  relayOn = true;

  Wire.begin(OLED_SDA, OLED_SCL);

  if (!u8g2.begin()) {
    Serial.println("OLED init failed");
    oledAvailable = false;
  } else {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(0, 10, "ESP32 Starting...");
    u8g2.sendBuffer();
  }

  WiFi.begin(ssid, password);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - wifiStart) < 10000) {
    delay(500);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, continuing offline");
  }

  if (String(ssid) == "YOUR_WIFI_SSID") {
    Serial.println("WiFi SSID placeholder in use, skipping web server init.");
  } else {
    WebSerial.begin(&server);
    webSerialEnabled = true;
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send(200, "text/html", index_html);
    });

    server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
      String status = "OK";
      String message = "System healthy.";
      if (lastVoltage <= cutoffVoltage) {
        status = "LOW";
        message = "Voltage below cutoff. Relay disabled.";
      } else if (lastVoltage <= warningVoltage) {
        status = "WARNING";
        message = "Voltage approaching cutoff.";
      }

      String payload = "{";
      payload += "\"battery_voltage\":" + String(lastVoltage, 2);
      payload += ",\"status\":\"" + status + "\"";
      payload += ",\"message\":\"" + message + "\"";
      payload += ",\"relay_on\":" + String(relayOn ? "true" : "false");
      payload += ",\"alert_sent\":" + String(alertSent ? "true" : "false");
      payload += ",\"wifi\":\"" + String(WiFi.status() == WL_CONNECTED ? "connected" : "offline") + "\"";
      payload += "}";
      request->send(200, "application/json", payload);
    });

    server.on("/relay", HTTP_GET, [](AsyncWebServerRequest *request) {
      String action = "toggle";
      if (request->hasParam("state")) {
        action = request->getParam("state")->value();
        action.toLowerCase();
      }

      if (action == "on") {
        relayOn = true;
      } else if (action == "off") {
        relayOn = false;
      } else if (action == "toggle") {
        relayOn = !relayOn;
      } else {
        String errorPayload = "{\"ok\":false,\"error\":\"state must be on, off, or toggle\"}";
        request->send(400, "application/json", errorPayload);
        return;
      }

      digitalWrite(RELAY_PIN, relayOn ? HIGH : LOW);

      String payload = "{";
      payload += "\"ok\":true";
      payload += ",\"relay_on\":" + String(relayOn ? "true" : "false");
      payload += "}";
      request->send(200, "application/json", payload);
    });

    server.on("/history", HTTP_GET, [](AsyncWebServerRequest *request) {
      const char historyPage[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0" />
    <title>Voltage History</title>
    <script src="https://cdn.tailwindcss.com"></script>
    <link rel="preconnect" href="https://fonts.googleapis.com">
    <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
    <link href="https://fonts.googleapis.com/css2?family=Roboto:wght@400;500;700&display=swap" rel="stylesheet">
    <style>
      body { font-family: "Roboto", sans-serif; }
      canvas { width: 100%; height: 320px; }
    </style>
  </head>
  <body class="min-h-screen bg-gradient-to-br from-slate-950 via-slate-900 to-zinc-900 text-slate-100">
    <main class="mx-auto flex min-h-screen w-full max-w-5xl flex-col gap-6 px-6 py-10">
      <header class="flex flex-col gap-2">
        <p class="text-sm uppercase tracking-[0.3em] text-amber-300">ESP32 Monitor</p>
        <h1 class="text-3xl font-bold sm:text-4xl">Voltage History</h1>
        <p class="text-sm text-slate-300">Recent battery voltage samples (auto-refresh).</p>
      </header>

      <section class="rounded-3xl border border-slate-700/60 bg-slate-900/60 p-6 shadow-xl">
        <div class="flex items-center justify-between">
          <h2 class="text-lg font-semibold text-slate-200">Historical Trend</h2>
          <span id="latestValue" class="text-sm text-slate-400">--</span>
        </div>
        <div class="mt-4">
          <canvas id="historyChart" width="800" height="320"></canvas>
        </div>
        <p class="mt-4 text-xs text-slate-400">Last update: <span id="lastUpdate">--</span></p>
      </section>
    </main>

    <script>
      const canvas = document.getElementById("historyChart");
      const ctx = canvas.getContext("2d");
      const latestValue = document.getElementById("latestValue");
      const lastUpdate = document.getElementById("lastUpdate");

      function drawChart(points) {
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        if (!points.length) return;

        const padding = 32;
        const width = canvas.width - padding * 2;
        const height = canvas.height - padding * 2;
        const min = Math.min(...points);
        const max = Math.max(...points);
        const range = max - min || 1;

        ctx.strokeStyle = "#1f2937";
        ctx.lineWidth = 1;
        for (let i = 0; i <= 4; i++) {
          const y = padding + (height / 4) * i;
          ctx.beginPath();
          ctx.moveTo(padding, y);
          ctx.lineTo(padding + width, y);
          ctx.stroke();
        }

        ctx.strokeStyle = "#fbbf24";
        ctx.lineWidth = 2;
        ctx.beginPath();
        points.forEach((value, index) => {
          const x = padding + (width * index) / (points.length - 1 || 1);
          const y = padding + height - ((value - min) / range) * height;
          if (index === 0) {
            ctx.moveTo(x, y);
          } else {
            ctx.lineTo(x, y);
          }
        });
        ctx.stroke();

        ctx.fillStyle = "#94a3b8";
        ctx.font = "12px Roboto";
        ctx.fillText(`${max.toFixed(2)} V`, padding, padding - 10);
        ctx.fillText(`${min.toFixed(2)} V`, padding, padding + height + 18);
      }

      async function refresh() {
        try {
          const response = await fetch("/history.json", { cache: "no-store" });
          if (!response.ok) throw new Error("bad response");
          const data = await response.json();
          const values = data.values || [];
          drawChart(values);
          latestValue.textContent = values.length ? `${values[values.length - 1].toFixed(2)} V` : "--";
          lastUpdate.textContent = new Date().toLocaleTimeString();
        } catch (err) {
          latestValue.textContent = "offline";
        }
      }

      refresh();
      setInterval(refresh, 2000);
    </script>
  </body>
</html>
)HTML";
      request->send(200, "text/html", historyPage);
    });
    server.on("/history.json", HTTP_GET, [](AsyncWebServerRequest *request) {
      String payload = "{\"values\":[";
      size_t count = historyFilled ? historySize : historyIndex;
      for (size_t i = 0; i < count; ++i) {
        size_t idx = historyFilled ? (historyIndex + i) % historySize : i;
        payload += String(voltageHistory[idx], 2);
        if (i + 1 < count) {
          payload += ",";
        }
      }
      payload += "]}";
      request->send(200, "application/json", payload);
    });

    server.on("/voltage", HTTP_GET, [](AsyncWebServerRequest *request) {
      String payload = "{\"battery_voltage\": " + String(lastVoltage, 2) + "}";
      if (request->hasParam("callback")) {
        String callback = request->getParam("callback")->value();
        String jsonp = callback + "(" + payload + ");";
        request->send(200, "application/javascript", jsonp);
      } else {
        request->send(200, "application/json", payload);
      }
    });
    server.begin();
  }
}

void loop() {
  float voltage = readBatteryVoltage();
  lastVoltage = voltage;
  voltageHistory[historyIndex] = voltage;
  historyIndex = (historyIndex + 1) % historySize;
  if (historyIndex == 0) {
    historyFilled = true;
  }

  Serial.printf("Battery Voltage: %.2f V\n", voltage);
  if (webSerialEnabled) {
    WebSerial.printf("Battery Voltage: %.2f V\n", voltage);
  }

  updateOLED(voltage);

  if (voltage <= cutoffVoltage && !alertSent) {
    sendTwilioSMS("ALERT: Battery voltage dropped to " +
                  String(voltage, 2) + "V");

    sendLocalPOST(voltage);

    alertSent = true;
    digitalWrite(RELAY_PIN, LOW);
    relayOn = false;
  }

  if (voltage > cutoffVoltage + 0.5) {
    alertSent = false;
    if (!relayOn) {
      digitalWrite(RELAY_PIN, HIGH);
      relayOn = true;
    }
  }

  delay(2000);
}
