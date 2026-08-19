#pragma once
// ============================================================================
// TankMesh v3 — shared BLE protocol definitions
// This exact file must be identical across the sensor, main display, and
// portable display sketches — it is the contract between them.
// ============================================================================
#include <Arduino.h>

// ---- BLE identity (private 128-bit UUIDs, generated for this project) ----
#define TANKMESH_SERVICE_UUID    "8f9e1a00-4b2e-4f1a-9c3d-0123456789ab"
#define TANKMESH_NAMECOLOR_UUID  "8f9e1a01-4b2e-4f1a-9c3d-0123456789ab"  // rw: name + color
#define TANKMESH_CAL_UUID        "8f9e1a02-4b2e-4f1a-9c3d-0123456789ab"  // rw: calibration
#define TANKMESH_MESH_UUID       "8f9e1a03-4b2e-4f1a-9c3d-0123456789ab"  // rw: group label + sleep interval
#define TANKMESH_DIAG_UUID       "8f9e1a04-4b2e-4f1a-9c3d-0123456789ab"  // ro: diagnostics

// Manufacturer ID 0xFFFF is the Bluetooth SIG's reserved "for testing / private
// use only" value — fine for a closed hobby system like this, not for a
// commercial product you plan to sell.
#define TANKMESH_MFG_ID           0xFFFF
#define TANKMESH_MAGIC0           'T'
#define TANKMESH_MAGIC1           'M'
#define TANKMESH_PROTO_VERSION    1

// ---- advertising flag bits ----
#define TM_FLAG_LOW_BATT    0x01
#define TM_FLAG_CAL_VALID   0x02
#define TM_FLAG_ON_BATTERY  0x04

#pragma pack(push, 1)

// Broadcast every wake cycle inside BLE manufacturer-specific data.
// Total 12 bytes -> fits comfortably in a single (non-scan-response) adv packet.
struct TankMeshAdvData {
  uint8_t  magic0, magic1;       // 'T','M' — cheap filter before UUID check
  uint8_t  protoVersion;
  uint8_t  levelPercent;         // 0-100
  uint16_t batteryMillivolts;    // 0 if not battery powered / not measured
  uint32_t counter;              // wake-cycle counter, survives deep sleep via RTC mem
  uint8_t  settingsVersion;      // bumped by sensor whenever settings change
  uint8_t  flags;
};

// GATT characteristic: name + color. 20 bytes = fits default 23-byte MTU exactly.
struct TankMeshNameColor {
  char     name[16];             // NUL-terminated, <=15 visible chars
  uint32_t colorRGB;             // 0x00RRGGBB
};

// GATT characteristic: calibration
struct TankMeshCal {
  uint8_t  autoCalEnabled;
  uint16_t adcMin;
  uint16_t adcMax;
};

// GATT characteristic: group label + wake interval
struct TankMeshMesh {
  char     prefix[16];           // cosmetic group/install label, NUL-terminated
  uint16_t sleepIntervalSec;     // wake period; default 60
};

// GATT characteristic: read-only diagnostics
struct TankMeshDiag {
  uint16_t rawAdc;
  uint16_t filteredAdc;
  uint8_t  isCalibrated;
  uint8_t  settingsVersion;      // authoritative version, used by clients to confirm sync
  uint16_t batteryMillivolts;
};

#pragma pack(pop)
