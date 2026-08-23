# TankMesh

A wireless tank-level monitoring system built on ESP32, using Bluetooth Low Energy sensor nodes and two display form factors (mains-powered and battery-portable). No app required — sensors and displays are configured entirely through a browser.

> **Status:** actively developed, functional, and in real-world testing. See [Known Limitations](#known-limitations) below before deploying.

---

## Features

**Sensor network**
- Up to 9 independent tank sensors per system, each identified by its own BLE address — no manual addressing
- Battery-powered sensors with deep-sleep operation between readings (default 60s wake interval, adjustable per sensor)
- BLE-only sensors (no WiFi on the sensor side) for lower power draw and simpler wireless range
- Self-calibrating level sensing, with manual override and a live raw-ADC readout for hand tuning
- Group-label isolation — multiple independent TankMesh installations can operate near each other (e.g. neighboring boats) without showing each other's tanks

**Displays**
- Two form factors sharing the same underlying system: a 3.5" mains-powered display and a 2.2"/2.4" battery-portable display
- Dark-themed UI with per-tank custom colors, consistent across every screen and the web portal
- Multi-tank overview with color-coded fill bars, and a detailed per-tank graphical view
- User-configurable tank display order per screen
- Custom-branded setup screen

**Configuration**
- Entirely browser-based — connect to the display's own WiFi access point, no app install required
- Remote sensor configuration over BLE (name, color, calibration, wake interval) without physically accessing the sensor
- New sensors are auto-discovered and appear ready to be named

**Power management**
- Adjustable idle-timeout screen dimming on the portable display, with instant wake on touch
- Sensor battery voltage monitoring, shown as a percentage

---

## Hardware

| Role | Board | Notes |
|---|---|---|
| Sensor | Generic ESP32 dev board ("ESP32S") with BLE | ESP32-S2 will **not** work — it has no BLE. ESP32, S3, and C3 are fine. |
| Main display | CYD ("Cheap Yellow Display") ESP32 3.5" | ST7796 panel, XPT2046 resistive touch |
| Portable display | CYD ESP32 2.2"/2.4" | ST7789 panel (parallel bus), capacitive touch |

Each sensor also needs a simple 2-resistor voltage divider from its battery lead to a free ADC1 pin (see firmware comments for the default pin and how to adjust the divider ratio) for battery reporting.

---

## Repository structure

```
TankMeshSensor/         Sensor firmware
  TankMeshSensor.ino
  TankMeshProtocol.h     Shared BLE protocol definitions — must be identical across all three sketches

TankMeshMainDisplay/     3.5" display firmware
  TankMeshMainDisplay.ino
  TankMeshProtocol.h
  TankMeshSetupLogo.h     Embedded splash graphic for the setup screen

TankMeshPortable/        2.2"/2.4" portable display firmware
  TankMeshPortable.ino
  TankMeshProtocol.h
  TankMeshSetupLogo.h
```

Each folder is a self-contained Arduino sketch — open the `.ino` file in Arduino IDE with the rest of that folder's contents alongside it.

---

## Getting started

### Requirements
- Arduino IDE with the ESP32 board package installed
- Libraries: [LovyanGFX](https://github.com/lovyan03/LovyanGFX) (display driver); everything else (WiFi, WebServer, Preferences, BLE) ships with the ESP32 Arduino core

### Flashing
1. Flash `TankMeshSensor` to each sensor board.
2. Flash `TankMeshMainDisplay` and/or `TankMeshPortable` to your display board(s).
3. Power everything on. Each display starts its own WiFi access point (default SSID/password shown on its Setup screen).
4. Connect to a display's WiFi from a phone or laptop and browse to `192.168.4.1`.
5. Newly-seen sensors appear under **Tank Settings** — name them, assign a color, and set their group label to match the screen's own group label (also configurable on the portal's settings page) so they show up on the main Overview screen.

---

## Known limitations

- **No battery percentage on the portable display.** The specific portable board's ESP32 module has no ADC-capable GPIO exposed on any accessible header — this is a hardware constraint, not a firmware gap. The idle-timeout screen dimming still provides real power savings without it.
- **OTA firmware updates are not possible on sensors.** By design, sensors have no WiFi, so reflashing requires a physical USB connection.
- Calibration and settings writes to a sensor can occasionally need a retry if the sensor isn't inside its brief post-wake connectable window at the moment you save — this is expected given the battery-saving sleep cycle, not a bug.

---

## License

This project is currently shared for personal, non-commercial use. A commercial version may be released separately in the future. *(Replace this section with a specific license once chosen — e.g. [PolyForm Noncommercial](https://polyformproject.org/licenses/noncommercial/1.0.0/) or [CC BY-NC 4.0](https://creativecommons.org/licenses/by-nc/4.0/) are common choices for "free for personal use, commercial rights reserved" projects. This isn't legal advice — worth a quick read of a couple of options, or a proper license consult, before you rely on it commercially.)*

---

## Contributing

Issues and pull requests are welcome for personal-use improvements and bug fixes. *(Add contribution guidelines here if you want outside contributions once this is public.)*
