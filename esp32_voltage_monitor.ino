/*
====================================================================
FUNCTIONAL SPECIFICATION DOCUMENT (FSD)
====================================================================

Purpose
- Monitor battery voltage with an ESP32 and respond to low-voltage
  conditions while providing local and remote visibility.

Inputs
- ADC reading from BATTERY_PIN via voltage divider (R1/R2).
- WiFi credentials (ssid/password).

Outputs
- Relay control on RELAY_PIN (disable on low voltage).
- OLED status display (if available).
- Serial and WebSerial logging.
- HTTP GET endpoint `/voltage` (JSON/JSONP).
- HTTP POST to local endpoint when low voltage triggers.
- SMS alert via Twilio when low voltage triggers.

Functional Requirements
1) Voltage Measurement
   - Sample ADC and compute battery voltage using divider ratio.
   - Update `lastVoltage` each loop.
2) Display
   - If OLED initializes, show voltage and status.
   - If OLED is absent, continue program without blocking.
   - Show WARNING when voltage <= 17.0V.
   - Show LOW VOLTAGE when voltage <= 16.8V.
3) Low-Voltage Handling
   - At or below 16.8V: disable relay, send SMS, send local POST.
   - Prevent repeated alerts until voltage rises above 17.3V.
4) Web Interface
   - Host HTTPS AsyncWebServer on port 443 (self-signed cert).
   - `/voltage` returns JSON: {"battery_voltage": XX.XX}
   - If `callback` param provided, return JSONP: callback(...);
5) Connectivity
   - Attempt WiFi for up to 10 seconds; continue offline if not connected.

Non-Functional Requirements
- Loop period ~2 seconds.
- Use 12-bit ADC scaling for ESP32 (0-4095) at 3.3V reference.
- Keep network operations non-blocking where possible.

Assumptions
- Correct voltage divider sizing for ADC input limits.
- Relay logic level matches hardware.
- OLED uses SSD1306 over I2C at address 0x3C.

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
- Detect when battery voltage drops to or below 16.8 volts.

LOW-VOLTAGE RESPONSE
When the battery voltage reaches 16.8V or lower:
- Immediately disable the relay output to protect the system.
- Send a single SMS alert using the Twilio API.
- Send an HTTP POST request to http://192.168.1.3 containing the
  current battery voltage in JSON format.
- Prevent repeated alerts until voltage recovers above the threshold.

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
NODE -> ESP32 GPIO34 (ADC input)
BATTERY - -> ESP32 GND (common ground)

RELAY MODULE:
  VCC -> ESP32 5V (or 3.3V if module supports it)
  GND -> ESP32 GND
  IN  -> ESP32 GPIO26
  NOTE -> Relay control on GPIO26 (assumes active HIGH)

OLED (SSD1306 I2C):
  VCC -> ESP32 3.3V
  GND -> ESP32 GND
  SDA -> ESP32 GPIO21
  SCL -> ESP32 GPIO22

====================================================================
*/

/*
====================================================================
WIRING DIAGRAM (ASCII)
====================================================================

Battery + ---- R1 100k ----+---- R2 10k ---- Battery - (GND)
                           |
                           +---- GPIO34 (ADC)
Battery - -----------------+---- ESP32 GND

Relay Module:
  VCC ---- ESP32 5V (or 3.3V)
  GND ---- ESP32 GND
  IN  ---- ESP32 GPIO26
  NOTE --- Relay control on GPIO26 (assumes active HIGH)

OLED (SSD1306 I2C):
  VCC ---- 3.3V
  GND ---- GND
  SDA ---- GPIO21
  SCL ---- GPIO22

====================================================================
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <AsyncTCP_SSL.h>
#include <WebSerial.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ================= OLED CONFIG =================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDRESS  0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledAvailable = true;

// ================= CONFIG =================
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Twilio
const char* twilioSID   = "ACxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
const char* twilioToken = "your_auth_token";
const char* twilioFrom  = "+1234567890";
const char* twilioTo    = "+1987654321";

// TLS certificate/key (self-signed for local testing)
const char serverCert[] PROGMEM = R"PEM(
-----BEGIN CERTIFICATE-----
MIIDDTCCAfWgAwIBAgIUYNAlsVIAkFh82efxebPlJx88UoowDQYJKoZIhvcNAQEL
BQAwFjEUMBIGA1UEAwwLZXNwMzIubG9jYWwwHhcNMjYwMTIxMjI0NjI4WhcNMjcw
MTIxMjI0NjI4WjAWMRQwEgYDVQQDDAtlc3AzMi5sb2NhbDCCASIwDQYJKoZIhvcN
AQEBBQADggEPADCCAQoCggEBAM6tdGrNfdqa2k9FSmUyZ9mK1wi1kLt1SiwFNh6J
0e21c471Y7y8FmTfSYCZcXQY6CKcaW2f8txgA0khUEHIotlD3gwoQRXGxAzq1EMk
/NegqrtaO92+HmzfkTEWvSQOt89lQ5NrCcDP1bjh24Xlkkdy+1dQo2pSSSsc1LIR
vYMGrIT7PqilX6N6BduFilgUsJsvA2JFLh1pZSvLSIfeY0foOklw1IGUJAYyv0kX
PcOnG+7Qvd9P96GYeU4ia7HpT7SfXvtFVgIr6qmmuVLF04jzoZgt38kcZ1X9guan
bmAu1hjRjAeUzgZIwho9WmCpxGBT4m4+knzzOC0EuGJ8Ke0CAwEAAaNTMFEwHQYD
VR0OBBYEFBU7/kI5JxuX7CW/PujkNHNcrTQvMB8GA1UdIwQYMBaAFBU7/kI5JxuX
7CW/PujkNHNcrTQvMA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZIhvcNAQELBQADggEB
AF+GCiELQfyTXPngt/EMH4ftLCfh6/1atQ+9vB+LB5LUj9UxlPiPQDq8oy0zvDaA
1YKUgnFGcBvgdPt8batRseMNX50/iWOpkjY2vYlWTGi8IsTYAKxYVefcVEja8n5d
mNsNyqy/5patvwO0pbu5ZLx2ch+6enOK7sjysskH2IDab98UzDarlB9vN2Hzn5NW
3EgVn87Up+FveCvUBlRVp259Qy8HBNkcdyC0Q7sIQfyMtm53iSGvQazuHqIhO5su
eEFPl6hHENJyfH7iqUIcZMO6VdV269ytvyT9IJ9Wnv54IRvCQQ/vkidb0t8heRor
zjc5yKB+RNEERYLZcJLLVHY=
-----END CERTIFICATE-----
)PEM";

const char serverKey[] PROGMEM = R"PEM(
-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDOrXRqzX3amtpP
RUplMmfZitcItZC7dUosBTYeidHttXOO9WO8vBZk30mAmXF0GOginGltn/LcYANJ
IVBByKLZQ94MKEEVxsQM6tRDJPzXoKq7Wjvdvh5s35ExFr0kDrfPZUOTawnAz9W4
4duF5ZJHcvtXUKNqUkkrHNSyEb2DBqyE+z6opV+jegXbhYpYFLCbLwNiRS4daWUr
y0iH3mNH6DpJcNSBlCQGMr9JFz3Dpxvu0L3fT/ehmHlOImux6U+0n177RVYCK+qp
prlSxdOI86GYLd/JHGdV/YLmp25gLtYY0YwHlM4GSMIaPVpgqcRgU+JuPpJ88zgt
BLhifCntAgMBAAECggEAHGa9rseaWeYZxfbxqEJq/vwTXMEGqJwPm0kEDOJHlPDw
dl7GW/NE6Iu+oAt2Ccw6ajcwTb5DM4GGMhB/5OpbZpvq8aS+fO2Zl2TV0nxMupz7
mU9nFqu/ppp6a1KCn2feXoO96440AukMp6Fx90568ZKdc3xDWKMCJwiJOgcyEcUj
qJZ7HkIysJfRNDYPe5VWcnUO38l80BlfmCVpaRe3T9ucnv1zMXW1utHuhG0fOjtU
uhBRt2m4MVYXbHox2AK5B11rXr0zhtnLY3zrVLHF1re8UL0sA03gaSpNkQqgqZe5
Zn3L0JmU07MaDZkIT7iGWoVyKGFvn8qFGK9Yb18CCQKBgQD1vBGl44dyUmM0SoPh
DwMx/sB1Z+KMdVGtZY5KXZGMikkUT4jzxGJewJ2pfMmM5NFnbr5HnLy6ECIl0yVw
TQHHYNQ4kLdMVxBHM2ShgCywEuHEjVV6ddM6ZgffXmmQvpKbdCdpCCSiZmLjuW7W
v8O1/3Tca8uNIkEjMBkLa2x0DwKBgQDXT7PEaRg3B64a2LdsLkSo22/TByqdvuJ1
C4LRhdrl4AwPrGp1mx1w487r44JJtEtcACCuUp+IEeFpaKLdM1BzLR9ExSi4xJi0
Mk//juoFttXMp+HeWPldZtCNGD1czPoBRkIIqh87G8c7ppa+9aPEC198xrXgpoff
khXFOFeWQwKBgQDsXkXfepermKICB3cJQcaCDZiUliOtlZ/GGXyf/ZbmR5H57nM+
f3VbzQ0anYTFeMgQJM701UgX9TLTjWFivz/pxzL7YgBedxSaWE4ApujVSKRPyt8g
1zsh3kjOS+NhLl6ZF0ZdWk7aw391qsV53aVkZ9/BshJupDdhoH9Go8MDcwKBgG3S
hYslX+iRzkh7SfOwFe1bIEqvWRllB/VTjcJ5WBHwmbZU52hdWkL+r8i6HvahM98V
YZYJJr4tAKDXclsJlXtqBIz7U64K+SjQkOV1bADGJX9iEl9rWqY9jxqoxoPTOaH0
yDHLNGrd3F2ctz9n48RXWLk4UgTobF2pEdmqx2IpAoGBAI/HqLLpjLs67PU+ZpyA
PkZToOZwZonKgu6QKBItA3jlcUzST72iYaiKzKGQH1hvqVKZaF6v78tjXv46ctHI
hvEZNb/3aLI6ZMmtFcFiiMp1svZI7/h4RevZHfQfiA4aL9wISgxf4DM8fAKhmytC
PH9NbIrdiL4W6EfBPyaVHYTM
-----END PRIVATE KEY-----
)PEM";

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
          Live telemetry from the ESP32. Self-signed HTTPS may require a one-time browser trust prompt.
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
          statusMessage.textContent = "Unable to reach device. Check HTTPS trust or network.";
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
#define BATTERY_PIN 34
#define RELAY_PIN   26

// Voltage divider values
float R1 = 100000.0;
float R2 = 10000.0;

// Voltage threshold
float cutoffVoltage = 16.8;
float warningVoltage = 17.0;

// ==========================================

AsyncWebServer server(443);
bool alertSent = false;
float lastVoltage = 0.0;
bool relayOn = true;

float readBatteryVoltage() {
  int raw = analogRead(BATTERY_PIN);
  float adcVoltage = (raw / 4095.0) * 3.3;
  return adcVoltage * ((R1 + R2) / R2);
}

void updateOLED(float voltage) {
  if (!oledAvailable) {
    return;
  }

  display.clearDisplay();

  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  int16_t x1, y1;
  uint16_t w, h;
  const char* title = "Battery Voltage";
  display.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 0);
  display.println(title);

  display.setTextSize(2);
  char voltageText[16];
  snprintf(voltageText, sizeof(voltageText), "%.2f V", voltage);
  display.getTextBounds(voltageText, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 16);
  display.println(voltageText);

  display.setTextSize(1);
  int16_t statusY = 48;

  if (voltage <= cutoffVoltage) {
    const char* status = "!! STATUS: LOW VOLTAGE !!";
    display.getTextBounds(status, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, statusY);
    display.println(status);
  } else {
    const char* status = "[STATUS: OK]";
    display.getTextBounds(status, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, statusY);
    display.println(status);
  }

  if (voltage <= warningVoltage) {
    const char* warning = "WARNING: <=17.0V";
    display.getTextBounds(warning, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 56);
    display.println(warning);
  }

  display.display();
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

  Wire.begin(21, 22);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("OLED init failed");
    oledAvailable = false;
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("ESP32 Starting...");
    display.display();
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
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send_P(200, "text/html", index_html);
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
    server.beginSecure(serverCert, serverKey, nullptr);
  }
}

void loop() {
  float voltage = readBatteryVoltage();
  lastVoltage = voltage;

  Serial.printf("Battery Voltage: %.2f V\n", voltage);
  WebSerial.printf("Battery Voltage: %.2f V\n", voltage);

  updateOLED(voltage);

  if (voltage <= cutoffVoltage && !alertSent) {
    digitalWrite(RELAY_PIN, LOW);
    relayOn = false;

    sendTwilioSMS("ALERT: Battery voltage dropped to " +
                  String(voltage, 2) + "V");

    sendLocalPOST(voltage);

    alertSent = true;
  }

  if (voltage > cutoffVoltage + 0.5) {
    alertSent = false;
  }

  delay(2000);
}
