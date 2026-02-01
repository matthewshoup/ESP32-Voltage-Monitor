# ESP32 Voltage Monitor

====================================================================
FUNCTIONAL SPECIFICATION DOCUMENT (FSD)
====================================================================

Purpose
- Monitor battery voltage with an ESP32-C3 and respond to low-voltage
  conditions while providing local and remote visibility.

Inputs
- ADC reading from BATTERY_PIN via voltage divider (R1/R2).
- WiFi credentials via `secrets.h` (not committed).

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
- OLED uses SSD1306 72x40 over I2C.

====================================================================

## Wiring Diagram (Text)

```
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

```

## Wiring Diagram (ASCII)

```
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

```
