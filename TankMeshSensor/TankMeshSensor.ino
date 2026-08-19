// ============================================================================
// TankMeshSensor v3.0
//
// Wakes every SLEEP_SEC seconds, samples the tank level, broadcasts it over
// BLE advertising (no connection needed for that part), then stays briefly
// connectable so a screen or the phone app can read/write settings before it
// goes back to deep sleep.
//
// Board: plain ESP32 ("ESP32S" dev board) — needs classic BLE, works fine.
// Only ESP32-S2 lacks BLE; if your sender is actually an S2, swap to a C3.
//
// Libraries: built-in BLE (BLEDevice/BLEServer/BLEUtils/BLEAdvertising),
// Preferences — same libraries the v2 code already used, nothing new added.
// ============================================================================

#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLEAdvertising.h>
#include "TankMeshProtocol.h"

// ---------------- Hardware config — adjust to your wiring ----------------
#define ANALOG_PIN            36     // tank level sense, same pin as v2
#define BATTERY_MONITORING    1      // set to 0 if this sensor runs off mains/USB only
#define BATTERY_ADC_PIN       39     // ADC pin reading the battery divider (VN on most devkits)
#define BATTERY_DIVIDER_RATIO 2.0f   // (R1+R2)/R2 for your divider — 2.0 = equal resistors
#define ADC_VREF_MV           3300.0f

#define DEFAULT_SLEEP_SEC     60
#define CONNECT_WINDOW_MS     15000  // minimum time spent connectable after waking
#define MAX_SESSION_MS        30000  // hard cap on total awake time even if a client lingers

const float ALPHA = 0.25f;  // EMA smoothing across wake cycles (was per-4s-loop in v2)

// filteredAdc/counter live in RTC memory so they survive deep sleep (but not power loss)
RTC_DATA_ATTR float    rtcFilteredAdc = -1.0f;
RTC_DATA_ATTR uint32_t rtcCounter     = 0;

Preferences prefs;

TankMeshNameColor nameColor;
TankMeshCal        cal;
TankMeshMesh        mesh;
TankMeshDiag        diag;

uint8_t settingsVersion = 0;
volatile bool deviceConnected = false;

// ---------------- Persistence ----------------
void loadSettings() {
  prefs.begin("tm_nc", true);
  String n = prefs.getString("name", "");
  uint32_t c = prefs.getUInt("color", 0x808080);
  prefs.end();
  memset(nameColor.name, 0, sizeof(nameColor.name));
  n.toCharArray(nameColor.name, sizeof(nameColor.name));
  nameColor.colorRGB = c;

  prefs.begin("tm_cal", true);
  cal.autoCalEnabled = prefs.getUChar("auto_cal", 1);
  cal.adcMin = prefs.getUShort("adc_min", 4095);
  cal.adcMax = prefs.getUShort("adc_max", 0);
  prefs.end();

  prefs.begin("tm_mesh", true);
  String p = prefs.getString("prefix", "TankMesh");
  uint16_t sleepSec = prefs.getUShort("sleep_sec", DEFAULT_SLEEP_SEC);
  prefs.end();
  memset(mesh.prefix, 0, sizeof(mesh.prefix));
  p.toCharArray(mesh.prefix, sizeof(mesh.prefix));
  mesh.sleepIntervalSec = sleepSec;

  prefs.begin("tm_sys", true);
  settingsVersion = prefs.getUChar("ver", 0);
  prefs.end();
}

void saveNameColor() {
  prefs.begin("tm_nc", false);
  prefs.putString("name", String(nameColor.name));
  prefs.putUInt("color", nameColor.colorRGB);
  prefs.end();
}

void saveCal() {
  prefs.begin("tm_cal", false);
  prefs.putUChar("auto_cal", cal.autoCalEnabled);
  prefs.putUShort("adc_min", cal.adcMin);
  prefs.putUShort("adc_max", cal.adcMax);
  prefs.end();
}

void saveMesh() {
  prefs.begin("tm_mesh", false);
  prefs.putString("prefix", String(mesh.prefix));
  prefs.putUShort("sleep_sec", mesh.sleepIntervalSec);
  prefs.end();
}

void bumpSettingsVersion() {
  settingsVersion++;
  prefs.begin("tm_sys", false);
  prefs.putUChar("ver", settingsVersion);
  prefs.end();
}

// ---------------- BLE server callbacks ----------------
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override { deviceConnected = true; }
  void onDisconnect(BLEServer *s) override {
    deviceConnected = false;
    BLEDevice::startAdvertising(); // stay available in case another client wants in
  }
};

class NameColorCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String v = c->getValue();
    if (v.length() == sizeof(TankMeshNameColor)) {
      memcpy(&nameColor, v.c_str(), sizeof(TankMeshNameColor));
      nameColor.name[15] = '\0';
      saveNameColor();
      bumpSettingsVersion();
    }
  }
  void onRead(BLECharacteristic *c) override {
    c->setValue((uint8_t *)&nameColor, sizeof(nameColor));
  }
};

class CalCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String v = c->getValue();
    if (v.length() == sizeof(TankMeshCal)) {
      memcpy(&cal, v.c_str(), sizeof(TankMeshCal));
      saveCal();
      bumpSettingsVersion();
    }
  }
  void onRead(BLECharacteristic *c) override {
    c->setValue((uint8_t *)&cal, sizeof(cal));
  }
};

class MeshCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String v = c->getValue();
    if (v.length() == sizeof(TankMeshMesh)) {
      memcpy(&mesh, v.c_str(), sizeof(TankMeshMesh));
      mesh.prefix[15] = '\0';
      if (mesh.sleepIntervalSec < 5) mesh.sleepIntervalSec = 5; // safety floor
      saveMesh();
      bumpSettingsVersion();
    }
  }
  void onRead(BLECharacteristic *c) override {
    c->setValue((uint8_t *)&mesh, sizeof(mesh));
  }
};

class DiagCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic *c) override {
    c->setValue((uint8_t *)&diag, sizeof(diag));
  }
};

// ---------------- Sensing ----------------
uint16_t readBatteryMv() {
#if BATTERY_MONITORING
  uint32_t raw = analogRead(BATTERY_ADC_PIN);
  return (uint16_t)((raw / 4095.0f) * ADC_VREF_MV * BATTERY_DIVIDER_RATIO);
#else
  return 0;
#endif
}

uint8_t sampleLevel() {
  analogSetAttenuation(ADC_11db);
  uint16_t rawAdc = 0;
  // short burst of samples to settle the EMA since we no longer sample continuously
  for (int i = 0; i < 8; i++) {
    rawAdc = analogRead(ANALOG_PIN);
    if (rtcFilteredAdc < 0) rtcFilteredAdc = (float)rawAdc;
    else rtcFilteredAdc = (ALPHA * (float)rawAdc) + ((1.0f - ALPHA) * rtcFilteredAdc);
    delay(40);
  }
  uint16_t workingAdc = (uint16_t)rtcFilteredAdc;

  if (cal.autoCalEnabled) {
    bool changed = false;
    if (cal.adcMax <= cal.adcMin) {
      cal.adcMin = workingAdc; cal.adcMax = workingAdc; changed = true;
    } else {
      if (workingAdc < cal.adcMin) { cal.adcMin = workingAdc; changed = true; }
      if (workingAdc > cal.adcMax) { cal.adcMax = workingAdc; changed = true; }
    }
    if (changed) saveCal();
  }

  diag.rawAdc = rawAdc;
  diag.filteredAdc = workingAdc;
  diag.isCalibrated = (cal.adcMax > cal.adcMin) ? 1 : 0;

  if (cal.adcMax > cal.adcMin) {
    uint32_t pct = ((uint32_t)(workingAdc - cal.adcMin) * 100) / (cal.adcMax - cal.adcMin);
    return (uint8_t)constrain(pct, 0, 100);
  }
  return 0;
}

// ---------------- Advertising ----------------
void buildAndStartAdvertising(uint8_t levelPercent, uint16_t battMv) {
  TankMeshAdvData adv;
  adv.magic0 = TANKMESH_MAGIC0;
  adv.magic1 = TANKMESH_MAGIC1;
  adv.protoVersion = TANKMESH_PROTO_VERSION;
  adv.levelPercent = levelPercent;
  adv.batteryMillivolts = battMv;
  adv.counter = rtcCounter;
  adv.settingsVersion = settingsVersion;
  adv.flags = 0;
  if (BATTERY_MONITORING && battMv > 0 && battMv < 3400) adv.flags |= TM_FLAG_LOW_BATT;
  if (diag.isCalibrated) adv.flags |= TM_FLAG_CAL_VALID;
#if BATTERY_MONITORING
  adv.flags |= TM_FLAG_ON_BATTERY;
#endif

  diag.settingsVersion = settingsVersion;
  diag.batteryMillivolts = battMv;

  // Manufacturer field = 2-byte company ID + our payload.
  // Built as a raw byte buffer first, then wrapped in the length-aware String
  // constructor — the adv struct contains embedded zero bytes (e.g. counter),
  // so a plain null-terminated String would truncate the payload.
  uint8_t mfgBuf[2 + sizeof(TankMeshAdvData)];
  mfgBuf[0] = TANKMESH_MFG_ID & 0xFF;
  mfgBuf[1] = (TANKMESH_MFG_ID >> 8) & 0xFF;
  memcpy(mfgBuf + 2, &adv, sizeof(adv));
  String mfg((const char *)mfgBuf, sizeof(mfgBuf));

  BLEAdvertisementData advData;
  advData.setFlags(0x06);
  advData.setManufacturerData(mfg);

  // Device name goes in the scan response instead, so the main adv packet
  // (flags + mfg data, ~19 bytes) stays well under the 31-byte legacy limit.
  BLEAdvertisementData scanResp;
  String devName = "TM_";
  devName += (nameColor.name[0] ? nameColor.name : "Sensor");
  if (devName.length() > 20) devName = devName.substring(0, 20);
  scanResp.setName(devName.c_str());

  BLEAdvertising *pAdv = BLEDevice::getAdvertising();
  pAdv->setAdvertisementData(advData);
  pAdv->setScanResponseData(scanResp);
  pAdv->setMinPreferred(0x06);
  pAdv->start();
}

// ---------------- Sleep ----------------
void goToSleep(unsigned long activeMs) {
  BLEDevice::deinit(false);
  uint64_t sleepUs  = (uint64_t)mesh.sleepIntervalSec * 1000000ULL;
  uint64_t activeUs = (uint64_t)activeMs * 1000ULL;
  uint64_t remainUs = (sleepUs > activeUs) ? (sleepUs - activeUs) : 1000000ULL;
  esp_sleep_enable_timer_wakeup(remainUs);
  esp_deep_sleep_start();
}

// ---------------- Setup / loop ----------------
// Everything happens once per wake in setup(); loop() is never reached because
// setup() ends in deep sleep, which resets the chip on the next wake.
void setup() {
  Serial.begin(115200);
  unsigned long startMs = millis();

  loadSettings();
  rtcCounter++;

  uint16_t battMv = readBatteryMv();
  uint8_t levelPercent = sampleLevel();

  BLEDevice::init("TankMeshSensor");
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  BLEService *svc = pServer->createService(TANKMESH_SERVICE_UUID);

  BLECharacteristic *ncChar = svc->createCharacteristic(
      TANKMESH_NAMECOLOR_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  ncChar->setCallbacks(new NameColorCallbacks());
  ncChar->setValue((uint8_t *)&nameColor, sizeof(nameColor));

  BLECharacteristic *calChar = svc->createCharacteristic(
      TANKMESH_CAL_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  calChar->setCallbacks(new CalCallbacks());
  calChar->setValue((uint8_t *)&cal, sizeof(cal));

  BLECharacteristic *meshChar = svc->createCharacteristic(
      TANKMESH_MESH_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  meshChar->setCallbacks(new MeshCallbacks());
  meshChar->setValue((uint8_t *)&mesh, sizeof(mesh));

  BLECharacteristic *diagChar = svc->createCharacteristic(
      TANKMESH_DIAG_UUID, BLECharacteristic::PROPERTY_READ);
  diagChar->setCallbacks(new DiagCallbacks());
  diagChar->setValue((uint8_t *)&diag, sizeof(diag));

  svc->start();
  buildAndStartAdvertising(levelPercent, battMv);

  unsigned long windowStart = millis();
  while ((millis() - windowStart < CONNECT_WINDOW_MS) || deviceConnected) {
    delay(50);
    if ((millis() - startMs) > MAX_SESSION_MS) break; // safety cap
  }

  unsigned long activeMs = millis() - startMs;
  goToSleep(activeMs);
}

void loop() {
  // unreachable — device is asleep by the end of setup()
}
