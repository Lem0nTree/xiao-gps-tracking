/*
  XIAO nRF52840 GPS logger + bonded BLE download

  Target board:
    Seeed XIAO nRF52840
    IMPORTANT: use the non-mbed "Seeed nRF52 Boards" Arduino core.

  GPS wiring:
    GPS TX  -> XIAO D7 / Serial1 RX
    GPS RX  -> XIAO D6 / Serial1 TX
    GPS GND -> XIAO GND
    Bare ATGM module VCC -> XIAO 3V3
    5V breakout VCC -> switched 5V boost/load-switch output
    D1 -> boost/load-switch EN (power saving; NEVER directly to GPS VCC)

  Libraries:
    TinyGPSPlus
    Adafruit SPIFlash
    Bluefruit52Lib is supplied by the Seeed/Adafruit nRF52 board core.

  BLE:
    Nordic UART Service (NUS)
    Pairing passkey below. CHANGE IT before deployment.
    The first successfully bonded phone becomes the owner.
    Later unknown/unbonded phones are disconnected.

  Storage:
    Raw circular log in the XIAO's 2 MiB P25Q16H QSPI flash.
    Sector 0 stores tracker metadata (owner phone + GPS interval).
    Remaining sectors hold 20-byte GPS records in a circular log.
    The GPS interval is configurable from Android: 1 min, 15 min, 30 min,
    1 h, 2 h, or 3 h.
*/

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <bluefruit.h>
#include <TinyGPS++.h>
#include <Adafruit_SPIFlash.h>
#include <flash_devices.h>

// ---------------- User settings ----------------

static const char BLE_DEVICE_NAME[] = "XIAO-GPS";
static const char FW_VERSION[] = "1.5.2";
static const char BLE_PAIRING_PIN[] = "482731"; // CHANGE THIS, exactly 6 digits
static const uint32_t DEFAULT_LOG_INTERVAL_SECONDS = 1800; // 30 min
static const uint32_t GPS_BAUD = 9600;
static const uint8_t OWNER_RESET_PIN = D0;       // hold to GND at boot
static const uint32_t OWNER_RESET_HOLD_MS = 5000;

// ---------------- Release 1.0 power profile ----------------
//
// Target: a small ~300 mAh LiPo.
//
// IMPORTANT: The GPS breakout shown in the project photos exposes a 5V VCC pin.
// When running from LiPo, use a 3.7V->5V boost converter (or appropriate load
// switch) and connect D1 ONLY to that converter/load-switch EN pin. Do not power
// the GPS from D1 directly.
//
// If you can access the bare ATGM module ON/OFF pin instead, it is active-low:
// D1 HIGH = run, D1 LOW = shut down. The same logic below works.
//
// If no power gate is wired, set GPS_POWER_CONTROL_ENABLED false. The firmware
// will still reduce MCU/BLE/QSPI power, but the GPS receiver itself stays powered.
static const bool POWER_OPTIMIZATION_ENABLED = true;
static const bool GPS_POWER_CONTROL_ENABLED = true;
static const uint8_t GPS_POWER_PIN = D1;
static const bool GPS_POWER_ACTIVE_HIGH = true;

// For intervals longer than one minute the GPS is normally power-gated.
// It wakes this many seconds before the next point is due, so a cold-starting
// receiver has time to reacquire satellites. Once it has a fix in that pre-wake
// window, it stays powered/tracking until the timestamp becomes due.
//
// The 1-minute profile intentionally keeps the GPS powered after the first fix.
// With this 5-pin carrier there is no exposed VBAT, so repeatedly cold-starting
// every minute is both unreliable and often less efficient than staying locked.
static const uint32_t GPS_ACQUIRE_LEAD_SECONDS = 60;
static const uint32_t GPS_ACQUIRE_TIMEOUT_MS = 120000;
static const uint32_t GPS_RETRY_SLEEP_MS = 60000;
static const uint32_t GPS_POWER_SETTLE_MS = 250;

// Release build logging: serial diagnostics are automatically enabled on USB
// power, but stay disabled on battery so TinyUSB/printing does not waste energy.
static const bool DEBUG_SERIAL_ON_USB = true;

// XIAO nRF52840 supports 50 mA / 100 mA charging. 50 mA is intentionally used
// for the 300 mAh cell profile.
static const bool LIMIT_LIPO_CHARGE_TO_50MA = true;

// Seeed's nRF52 XIAO variant exposes the onboard HICHG selector as
// PIN_CHARGING_CURRENT. Older board-core revisions may omit the macro even
// though Arduino pin index 22 still maps to physical P0.13 / HICHG.
#ifndef PIN_CHARGING_CURRENT
#define PIN_CHARGING_CURRENT 22
#endif

// ---------------- GPS ----------------

TinyGPSPlus gps;

// ---------------- QSPI flash ----------------

Adafruit_FlashTransport_QSPI flashTransport;
Adafruit_SPIFlash flash(&flashTransport, false /* no 4 KiB cache */);

static const SPIFlash_Device_t flashDevices[] = { P25Q16H };

static const uint32_t FLASH_SIZE_BYTES = 2UL * 1024UL * 1024UL;
static const uint32_t SECTOR_SIZE = 4096;
static const uint32_t SECTOR_COUNT = FLASH_SIZE_BYTES / SECTOR_SIZE;

// Physical QSPI sector 0 is reserved for tracker metadata.
// GPS circular-log data uses sectors 1..511.
static const uint32_t METADATA_SECTOR = 0;
static const uint32_t LOG_SECTOR_FIRST = 1;
static const uint32_t LOG_SECTOR_COUNT = SECTOR_COUNT - LOG_SECTOR_FIRST;

// v1.0 legacy owner record magic and v1.5 combined metadata magic.
static const uint32_t LEGACY_OWNER_MAGIC = 0x314E574FUL; // "OWN1"
static const uint32_t METADATA_MAGIC = 0x354B5254UL;     // "TRK5"

#pragma pack(push, 1)
struct LegacyOwnerRecord {
  uint32_t magic;
  uint8_t version;
  uint8_t addrType;
  uint8_t addr[6];
  uint8_t reserved[3];
  uint8_t crc8;
};

struct MetadataRecord {
  uint32_t magic;
  uint8_t version;
  uint8_t ownerSet;
  uint8_t addrType;
  uint8_t addr[6];
  uint32_t logIntervalSeconds;
  uint8_t reserved[14];
  uint8_t crc8;
};

struct GpsRecord {
  uint32_t seq;
  uint32_t epoch;       // UTC Unix epoch seconds
  int32_t latE7;        // latitude * 10^7
  int32_t lonE7;        // longitude * 10^7
  uint16_t hdopX100;    // TinyGPS++ HDOP raw value (hundredths)
  uint8_t satellites;
  uint8_t crc8;         // CRC-8 over bytes 0..18
};
#pragma pack(pop)

static_assert(sizeof(LegacyOwnerRecord) == 16, "LegacyOwnerRecord must stay 16 bytes");
static_assert(sizeof(MetadataRecord) == 32, "MetadataRecord must stay 32 bytes");
static_assert(sizeof(GpsRecord) == 20, "GpsRecord must stay 20 bytes");

static const uint32_t RECORD_SIZE = sizeof(GpsRecord);
static const uint32_t RECORDS_PER_SECTOR = SECTOR_SIZE / RECORD_SIZE; // 204
static const uint32_t LOG_CAPACITY =
    RECORDS_PER_SECTOR * LOG_SECTOR_COUNT; // 104244 (~72.4 days @ 1/min)

static uint8_t sectorBuffer[RECORDS_PER_SECTOR * RECORD_SIZE];

static MetadataRecord metadataRecord = {};
static bool ownerSet = false;
static bool ownerResetRequested = false;
static uint32_t logIntervalSeconds = DEFAULT_LOG_INTERVAL_SECONDS;

static uint32_t newestSeq = 0;
static uint32_t storedCount = 0;
static uint32_t lastStoredEpoch = 0;
static GpsRecord newestRecord = {};
static uint32_t lastGpsDiagMs = 0;
static bool firstGpsDiag = true;

static bool debugSerialActive = false;
static bool gpsPowered = false;
static bool gpsEverHadFix = false;
static uint32_t gpsAcquireStartedMs = 0;
static uint32_t gpsNextWakeMs = 0;
static bool flashSleeping = false;

// ---------------- BLE ----------------

BLEUart bleuart;
static uint16_t activeConnHandle = BLE_CONN_HANDLE_INVALID;

// Framed protocol
static const uint8_t MAGIC1 = 0xA5;
static const uint8_t MAGIC2 = 0x5A;

enum MessageType : uint8_t {
  CMD_INFO_REQ       = 0x01,
  CMD_DOWNLOAD_REQ   = 0x02,
  CMD_CLEAR_LOG_REQ  = 0x03,
  CMD_PING           = 0x04,
  CMD_SET_INTERVAL   = 0x05,

  RSP_INFO           = 0x81,
  RSP_DATA_BATCH     = 0x82,
  RSP_DOWNLOAD_DONE  = 0x83,
  RSP_ACK            = 0x84,
  RSP_ERROR          = 0xFF
};

enum ErrorCode : uint8_t {
  ERR_PAIR_REQUIRED = 1,
  ERR_BAD_COMMAND   = 2,
  ERR_BAD_PAYLOAD   = 3,
  ERR_FLASH         = 4,
  ERR_BUSY          = 5,
  ERR_BAD_INTERVAL  = 6
};

static const uint16_t MAX_COMMAND_PAYLOAD = 16;
static const uint16_t MAX_RESPONSE_PAYLOAD = 180;

// Parser state
static uint8_t rxState = 0;
static uint8_t rxType = 0;
static uint16_t rxLen = 0;
static uint16_t rxPos = 0;
static uint16_t rxCrcCalculated = 0xFFFF;
static uint16_t rxCrcReceived = 0;
static uint8_t rxPayload[MAX_COMMAND_PAYLOAD];

// Download state
static bool downloadActive = false;
static uint32_t downloadSeq = 0;
static uint32_t downloadEndSeq = 0;
static uint32_t downloadSent = 0;

// ---------------- CRC helpers ----------------

uint8_t crc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0;
  while (len--) {
    crc ^= *data++;
    for (uint8_t i = 0; i < 8; i++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

uint16_t crc16Update(uint16_t crc, uint8_t data) {
  crc ^= (uint16_t)data << 8;
  for (uint8_t i = 0; i < 8; i++) {
    crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}


#define DBG_PRINT(x) do { if (debugSerialActive) Serial.print(x); } while (0)
#define DBG_PRINTLN(x) do { if (debugSerialActive) Serial.println(x); } while (0)
#define DBG_PRINTLN0() do { if (debugSerialActive) Serial.println(); } while (0)
#define DBG_PRINTF(...) do { if (debugSerialActive) Serial.printf(__VA_ARGS__); } while (0)

bool usbVbusPresent() {
  return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
}

bool timeReached(uint32_t now, uint32_t target) {
  return (int32_t)(now - target) >= 0;
}

bool isAllowedLogInterval(uint32_t seconds) {
  switch (seconds) {
    case 60:
    case 900:
    case 1800:
    case 3600:
    case 7200:
    case 10800:
      return true;
    default:
      return false;
  }
}

const char* intervalLabel(uint32_t seconds) {
  switch (seconds) {
    case 60: return "1 min";
    case 900: return "15 min";
    case 1800: return "30 min";
    case 3600: return "1 hour";
    case 7200: return "2 hours";
    case 10800: return "3 hours";
    default: return "custom";
  }
}

uint32_t secondsUntilGpsWake() {
  if (gpsPowered) return 0;
  const uint32_t now = millis();
  if (timeReached(now, gpsNextWakeMs)) return 0;
  return (gpsNextWakeMs - now + 999UL) / 1000UL;
}

void flashWake() {
  if (!flashSleeping) return;

  // P25Q16H Release from Deep Power-down (RDP).
  flashTransport.runCommand(0xAB);
  delayMicroseconds(50);
  flashSleeping = false;
}

void flashSleep() {
  if (flashSleeping || downloadActive) return;

  flash.waitUntilReady();
  // P25Q16H Deep Power-down (DP).
  flashTransport.runCommand(0xB9);
  delayMicroseconds(10);
  flashSleeping = true;
}

void configureBatteryCharging() {
  if (!LIMIT_LIPO_CHARGE_TO_50MA) return;

  // HIGH selects the XIAO's lower 50 mA charge-current setting.
  pinMode(PIN_CHARGING_CURRENT, OUTPUT);
  digitalWrite(PIN_CHARGING_CURRENT, HIGH);
}

void setGpsPowerControl(bool enabled) {
  if (!GPS_POWER_CONTROL_ENABLED) return;

  const bool pinHigh = enabled ? GPS_POWER_ACTIVE_HIGH : !GPS_POWER_ACTIVE_HIGH;
  digitalWrite(GPS_POWER_PIN, pinHigh ? HIGH : LOW);
}

void gpsPowerOn() {
  if (gpsPowered) return;

  setGpsPowerControl(true);
  if (GPS_POWER_CONTROL_ENABLED) {
    delay(GPS_POWER_SETTLE_MS);
  }

  Serial1.begin(GPS_BAUD);
  gpsPowered = true;
  gpsAcquireStartedMs = millis();
  DBG_PRINTLN("GPS power: ON");
}

void gpsPowerOff() {
  if (!gpsPowered) return;

  Serial1.end();
  setGpsPowerControl(false);
  gpsPowered = false;
  DBG_PRINTLN("GPS power: OFF");
}

void scheduleGpsSleep(uint32_t sleepMs) {
  if (!POWER_OPTIMIZATION_ENABLED) return;

  gpsPowerOff();
  gpsNextWakeMs = millis() + sleepMs;
}

void serviceGpsPowerState() {
  if (!POWER_OPTIMIZATION_ENABLED) {
    if (!gpsPowered) gpsPowerOn();
    return;
  }

  const uint32_t now = millis();

  if (!gpsPowered) {
    if (timeReached(now, gpsNextWakeMs)) {
      gpsPowerOn();
    }
    return;
  }

  // If the GPS has been awake too long without producing an eligible saved fix,
  // give the battery a rest and retry shortly. This is especially useful indoors.
  // A successful saved fix resets gpsAcquireStartedMs, which lets the 1-minute
  // continuous-GPS profile stay powered while it is producing fixes.
  if ((uint32_t)(now - gpsAcquireStartedMs) >= GPS_ACQUIRE_TIMEOUT_MS) {
    DBG_PRINTLN("GPS acquisition window timed out; sleeping before retry.");
    scheduleGpsSleep(GPS_RETRY_SLEEP_MS);
  }
}

bool recordValid(const GpsRecord& record) {
  if (record.seq == 0 || record.seq == 0xFFFFFFFFUL) return false;
  const uint8_t expected = crc8(reinterpret_cast<const uint8_t*>(&record), sizeof(GpsRecord) - 1);
  return expected == record.crc8;
}

bool legacyOwnerRecordValid(const LegacyOwnerRecord& record) {
  if (record.magic != LEGACY_OWNER_MAGIC || record.version != 1) return false;
  const uint8_t expected =
      crc8(reinterpret_cast<const uint8_t*>(&record), sizeof(LegacyOwnerRecord) - 1);
  return expected == record.crc8;
}

bool metadataRecordValid(const MetadataRecord& record) {
  if (record.magic != METADATA_MAGIC || record.version != 1) return false;
  if (!isAllowedLogInterval(record.logIntervalSeconds)) return false;
  const uint8_t expected =
      crc8(reinterpret_cast<const uint8_t*>(&record), sizeof(MetadataRecord) - 1);
  return expected == record.crc8;
}

void initDefaultMetadata() {
  memset(&metadataRecord, 0, sizeof(metadataRecord));
  metadataRecord.magic = METADATA_MAGIC;
  metadataRecord.version = 1;
  metadataRecord.ownerSet = 0;
  metadataRecord.logIntervalSeconds = DEFAULT_LOG_INTERVAL_SECONDS;
  metadataRecord.crc8 =
      crc8(reinterpret_cast<const uint8_t*>(&metadataRecord),
           sizeof(MetadataRecord) - 1);
  ownerSet = false;
  logIntervalSeconds = DEFAULT_LOG_INTERVAL_SECONDS;
}

bool persistMetadata() {
  flashWake();

  metadataRecord.magic = METADATA_MAGIC;
  metadataRecord.version = 1;
  metadataRecord.ownerSet = ownerSet ? 1 : 0;
  metadataRecord.logIntervalSeconds = logIntervalSeconds;
  metadataRecord.crc8 =
      crc8(reinterpret_cast<const uint8_t*>(&metadataRecord),
           sizeof(MetadataRecord) - 1);

  if (!flash.eraseSector(METADATA_SECTOR)) {
    flashSleep();
    return false;
  }
  flash.waitUntilReady();

  if (flash.writeBuffer(METADATA_SECTOR * SECTOR_SIZE,
                        reinterpret_cast<const uint8_t*>(&metadataRecord),
                        sizeof(metadataRecord)) != sizeof(metadataRecord)) {
    flashSleep();
    return false;
  }
  flash.waitUntilReady();
  flashSleep();
  return true;
}

bool loadOwnerLock() {
  flashWake();

  MetadataRecord current = {};
  if (flash.readBuffer(METADATA_SECTOR * SECTOR_SIZE,
                       reinterpret_cast<uint8_t*>(&current),
                       sizeof(current)) != sizeof(current)) {
    flashSleep();
    return false;
  }

  if (metadataRecordValid(current)) {
    metadataRecord = current;
    ownerSet = current.ownerSet != 0;
    logIntervalSeconds = current.logIntervalSeconds;
    flashSleep();
    return true;
  }

  // Upgrade path from firmware 1.0.x: migrate the old 16-byte owner record
  // without losing the bonded-phone ownership identity.
  LegacyOwnerRecord legacy = {};
  memcpy(&legacy, &current, sizeof(legacy));

  initDefaultMetadata();

  if (legacyOwnerRecordValid(legacy)) {
    ownerSet = true;
    metadataRecord.ownerSet = 1;
    metadataRecord.addrType = legacy.addrType;
    memcpy(metadataRecord.addr, legacy.addr, sizeof(metadataRecord.addr));
    DBG_PRINTLN("Migrating v1.0 owner metadata to v1.5.");
  }

  flashSleep();
  return persistMetadata();
}

bool ownerMatches(const ble_gap_addr_t& address) {
  if (!ownerSet) return true;
  return metadataRecord.addrType == address.addr_type &&
         memcmp(metadataRecord.addr, address.addr, sizeof(metadataRecord.addr)) == 0;
}

bool saveOwnerLock(const ble_gap_addr_t& address) {
  ownerSet = true;
  metadataRecord.ownerSet = 1;
  metadataRecord.addrType = address.addr_type;
  memcpy(metadataRecord.addr, address.addr, sizeof(metadataRecord.addr));
  return persistMetadata();
}

bool clearOwnerLock() {
  // Preserve the user's GPS interval while allowing a new phone to become owner.
  ownerSet = false;
  metadataRecord.ownerSet = 0;
  metadataRecord.addrType = 0;
  memset(metadataRecord.addr, 0, sizeof(metadataRecord.addr));
  return persistMetadata();
}

bool setLogInterval(uint32_t seconds) {
  if (!isAllowedLogInterval(seconds)) return false;

  // Do not leave RAM and flash disagreeing if the QSPI update fails.
  const uint32_t previousInterval = logIntervalSeconds;

  logIntervalSeconds = seconds;
  metadataRecord.logIntervalSeconds = seconds;

  if (!persistMetadata()) {
    logIntervalSeconds = previousInterval;
    metadataRecord.logIntervalSeconds = previousInterval;
    DBG_PRINTLN("GPS interval save FAILED; previous interval restored in RAM.");
    return false;
  }

  DBG_PRINTF("GPS interval changed to %s (%lus).\n",
             intervalLabel(seconds),
             (unsigned long)seconds);

  // Re-evaluate the schedule using a fresh GPS UTC fix. If the receiver is
  // sleeping, wake it now; serviceGps() will either save if due or put it back
  // to sleep until the appropriate pre-wake window.
  if (POWER_OPTIMIZATION_ENABLED && !gpsPowered) {
    gpsNextWakeMs = millis();
  } else {
    gpsAcquireStartedMs = millis();
  }

  return true;
}

bool checkOwnerResetPin() {
  pinMode(OWNER_RESET_PIN, INPUT_PULLUP);
  if (digitalRead(OWNER_RESET_PIN) != LOW) return false;

  DBG_PRINTLN("Owner reset requested: keep D0 connected to GND for 5 seconds...");
  const uint32_t started = millis();

  while (digitalRead(OWNER_RESET_PIN) == LOW) {
    if (millis() - started >= OWNER_RESET_HOLD_MS) {
      DBG_PRINTLN("Owner reset confirmed.");
      return true;
    }
    delay(25);
  }

  DBG_PRINTLN("Owner reset cancelled.");
  return false;
}

// ---------------- Flash ring ----------------

uint32_t addressForSeq(uint32_t seq) {
  const uint32_t slot = (seq - 1) % LOG_CAPACITY;
  const uint32_t sector =
      LOG_SECTOR_FIRST + (slot / RECORDS_PER_SECTOR);
  const uint32_t index = slot % RECORDS_PER_SECTOR;
  return sector * SECTOR_SIZE + index * RECORD_SIZE;
}

uint32_t sectorForSeq(uint32_t seq) {
  const uint32_t slot = (seq - 1) % LOG_CAPACITY;
  return LOG_SECTOR_FIRST + (slot / RECORDS_PER_SECTOR);
}

uint32_t indexInSectorForSeq(uint32_t seq) {
  const uint32_t slot = (seq - 1) % LOG_CAPACITY;
  return slot % RECORDS_PER_SECTOR;
}

bool readRecordAtAddress(uint32_t address, GpsRecord& out) {
  return flash.readBuffer(address, reinterpret_cast<uint8_t*>(&out), sizeof(out)) == sizeof(out);
}

bool readRecordBySeq(uint32_t seq, GpsRecord& out) {
  if (seq == 0) return false;
  if (!readRecordAtAddress(addressForSeq(seq), out)) return false;
  return recordValid(out) && out.seq == seq;
}

uint32_t countValidInSector(uint32_t sector) {
  const uint32_t base = sector * SECTOR_SIZE;
  const uint32_t bytes = sizeof(sectorBuffer);

  if (flash.readBuffer(base, sectorBuffer, bytes) != bytes) return 0;

  uint32_t count = 0;
  for (uint32_t i = 0; i < RECORDS_PER_SECTOR; i++) {
    GpsRecord rec;
    memcpy(&rec, sectorBuffer + i * RECORD_SIZE, RECORD_SIZE);
    if (recordValid(rec)) count++;
  }
  return count;
}

bool scanFlash() {
  flashWake();
  newestSeq = 0;
  storedCount = 0;
  lastStoredEpoch = 0;
  memset(&newestRecord, 0, sizeof(newestRecord));

  for (uint32_t sector = LOG_SECTOR_FIRST; sector < SECTOR_COUNT; sector++) {
    const uint32_t base = sector * SECTOR_SIZE;
    const uint32_t bytes = sizeof(sectorBuffer);

    if (flash.readBuffer(base, sectorBuffer, bytes) != bytes) {
      DBG_PRINTF("Flash read failed in sector %lu\n", (unsigned long)sector);
      flashSleep();
      return false;
    }

    for (uint32_t i = 0; i < RECORDS_PER_SECTOR; i++) {
      GpsRecord rec;
      memcpy(&rec, sectorBuffer + i * RECORD_SIZE, RECORD_SIZE);

      if (!recordValid(rec)) continue;

      storedCount++;
      if (rec.seq > newestSeq) {
        newestSeq = rec.seq;
        newestRecord = rec;
        lastStoredEpoch = rec.epoch;
      }
    }

    yield();
  }

  if (storedCount > LOG_CAPACITY) storedCount = LOG_CAPACITY;

  DBG_PRINTF("Flash scan: %lu records, newest seq=%lu\n",
                (unsigned long)storedCount, (unsigned long)newestSeq);
  flashSleep();
  return true;
}

bool appendRecord(GpsRecord& rec) {
  flashWake();

  const uint32_t seq = newestSeq + 1;
  const uint32_t index = indexInSectorForSeq(seq);
  const uint32_t sector = sectorForSeq(seq);

  if (index == 0) {
    const uint32_t oldValid = countValidInSector(sector);
    if (oldValid > storedCount) storedCount = 0;
    else storedCount -= oldValid;

    if (!flash.eraseSector(sector)) {
      DBG_PRINTLN("eraseSector failed");
      if (!downloadActive) flashSleep();
      return false;
    }
    flash.waitUntilReady();
  }

  rec.seq = seq;
  rec.crc8 = crc8(reinterpret_cast<const uint8_t*>(&rec), sizeof(GpsRecord) - 1);

  const uint32_t addr = addressForSeq(seq);
  if (flash.writeBuffer(addr, reinterpret_cast<const uint8_t*>(&rec), sizeof(rec)) != sizeof(rec)) {
    DBG_PRINTLN("writeBuffer failed");
    if (!downloadActive) flashSleep();
    return false;
  }
  flash.waitUntilReady();

  newestSeq = seq;
  newestRecord = rec;
  lastStoredEpoch = rec.epoch;
  if (storedCount < LOG_CAPACITY) storedCount++;

  DBG_PRINTF("Saved #%lu: %.7f, %.7f sats=%u hdop=%.2f\n",
             (unsigned long)rec.seq,
             rec.latE7 / 10000000.0,
             rec.lonE7 / 10000000.0,
             rec.satellites,
             rec.hdopX100 / 100.0);

  if (!downloadActive) flashSleep();
  return true;
}

bool clearLog() {
  downloadActive = false;
  flashWake();

  for (uint32_t sector = LOG_SECTOR_FIRST; sector < SECTOR_COUNT; sector++) {
    if (!flash.eraseSector(sector)) {
      flashSleep();
      return false;
    }
    flash.waitUntilReady();
    yield();
  }

  newestSeq = 0;
  storedCount = 0;
  lastStoredEpoch = 0;
  memset(&newestRecord, 0, sizeof(newestRecord));
  gpsEverHadFix = false;
  flashSleep();
  return true;
}

// ---------------- UTC conversion ----------------

// Howard Hinnant civil-date algorithm, adapted for Unix epoch conversion.
int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

uint32_t gpsEpochUtc() {
  if (!gps.date.isValid() || !gps.time.isValid()) return 0;

  const int year = gps.date.year();
  const unsigned month = gps.date.month();
  const unsigned day = gps.date.day();

  if (year < 2020 || month < 1 || month > 12 || day < 1 || day > 31) return 0;

  const int64_t days = daysFromCivil(year, month, day);
  const int64_t seconds =
      days * 86400LL +
      (int64_t)gps.time.hour() * 3600LL +
      (int64_t)gps.time.minute() * 60LL +
      gps.time.second();

  if (seconds <= 0 || seconds > 0xFFFFFFFFLL) return 0;
  return (uint32_t)seconds;
}

void scheduleAfterSavedFix() {
  if (!POWER_OPTIMIZATION_ENABLED) return;

  // At 1 minute, keeping the receiver locked is intentional. Without an
  // exposed VBAT pin, cutting 5V every minute would repeatedly cold-start it.
  if (logIntervalSeconds <= GPS_ACQUIRE_LEAD_SECONDS) {
    gpsAcquireStartedMs = millis();
    DBG_PRINTLN("1-minute profile: keeping GPS powered to retain satellite lock.");
    return;
  }

  const uint32_t sleepSeconds =
      logIntervalSeconds - GPS_ACQUIRE_LEAD_SECONDS;

  DBG_PRINTF("GPS sleeping %lus; will pre-wake %lus before next point.\n",
             (unsigned long)sleepSeconds,
             (unsigned long)GPS_ACQUIRE_LEAD_SECONDS);
  scheduleGpsSleep(sleepSeconds * 1000UL);
}

void serviceGps() {
  if (!gpsPowered) return;

  while (Serial1.available()) {
    gps.encode((char)Serial1.read());
  }

  // Only act on a fresh location update.
  if (!gps.location.isUpdated() || !gps.location.isValid()) return;

  const uint32_t epoch = gpsEpochUtc();
  if (epoch == 0) return;

  // If we woke just to establish current UTC and the next configured point is
  // still far away, go back to sleep. Once inside the final acquisition lead
  // window, stay powered so the receiver keeps its lock until the point is due.
  if (lastStoredEpoch != 0 && epoch < lastStoredEpoch + logIntervalSeconds) {
    const uint32_t remainingSeconds =
        (lastStoredEpoch + logIntervalSeconds) - epoch;

    if (POWER_OPTIMIZATION_ENABLED &&
        logIntervalSeconds > GPS_ACQUIRE_LEAD_SECONDS &&
        remainingSeconds > GPS_ACQUIRE_LEAD_SECONDS) {
      const uint32_t sleepSeconds =
          remainingSeconds - GPS_ACQUIRE_LEAD_SECONDS;
      DBG_PRINTF("Next point due in %lus; sleeping %lus before pre-wake.\n",
                 (unsigned long)remainingSeconds,
                 (unsigned long)sleepSeconds);
      scheduleGpsSleep(sleepSeconds * 1000UL);
    }
    return;
  }

  GpsRecord rec = {};
  rec.epoch = epoch;
  rec.latE7 = (int32_t)(gps.location.lat() * 10000000.0);
  rec.lonE7 = (int32_t)(gps.location.lng() * 10000000.0);
  rec.hdopX100 = gps.hdop.isValid()
      ? (uint16_t)min((uint32_t)65535, (uint32_t)gps.hdop.value())
      : 0;
  rec.satellites = gps.satellites.isValid()
      ? (uint8_t)min((uint32_t)255, (uint32_t)gps.satellites.value())
      : 0;

  if (appendRecord(rec)) {
    gpsEverHadFix = true;
    gpsAcquireStartedMs = millis();
    scheduleAfterSavedFix();
  }
}

void serviceGpsDiagnostics() {
  if (!debugSerialActive) return;
  if (!firstGpsDiag && millis() - lastGpsDiagMs < 5000) return;
  firstGpsDiag = false;
  lastGpsDiagMs = millis();

  if (!gpsPowered) {
    DBG_PRINTF("GPS DIAG power=OFF nextWake=%lus stored=%lu\n",
               (unsigned long)secondsUntilGpsWake(),
               (unsigned long)storedCount);
    return;
  }

  const bool locValid = gps.location.isValid();
  const bool dateValid = gps.date.isValid();
  const bool timeValid = gps.time.isValid();

  const uint32_t chars = gps.charsProcessed();
  const uint32_t passed = gps.passedChecksum();
  const uint32_t failed = gps.failedChecksum();
  const uint32_t fixes = gps.sentencesWithFix();
  const uint32_t age =
      locValid ? gps.location.age() : 0xFFFFFFFFUL;

  const uint32_t sats =
      gps.satellites.isValid() ? gps.satellites.value() : 0;

  DBG_PRINTF(
      "GPS DIAG chars=%lu passed=%lu failed=%lu fixSent=%lu "
      "loc=%d date=%d time=%d sats=%lu ageMs=%lu stored=%lu\n",
      (unsigned long)chars,
      (unsigned long)passed,
      (unsigned long)failed,
      (unsigned long)fixes,
      locValid ? 1 : 0,
      dateValid ? 1 : 0,
      timeValid ? 1 : 0,
      (unsigned long)sats,
      (unsigned long)age,
      (unsigned long)storedCount);

  if (chars < 10) {
    DBG_PRINTLN(
        "GPS WARNING: no UART/NMEA data. Check GPS power, GND, TX->D7, and 9600 baud.");
  } else if (passed == 0 && failed > 0) {
    DBG_PRINTLN(
        "GPS WARNING: UART data is arriving but NMEA checksums fail; check baud/noise.");
  } else if (!locValid) {
    DBG_PRINTLN(
        "GPS STATUS: NMEA is arriving but there is no satellite position fix yet.");
  } else if (!dateValid || !timeValid) {
    DBG_PRINTLN(
        "GPS STATUS: position is valid but UTC date/time is not valid yet; not logging.");
  } else {
    DBG_PRINTF(
        "GPS READY: %.7f, %.7f -- point eligible for %lus logger.\n",
        gps.location.lat(),
        gps.location.lng(),
        (unsigned long)logIntervalSeconds);
  }
}

// ---------------- BLE protocol ----------------

bool connectionIsAuthorized() {
  if (activeConnHandle == BLE_CONN_HANDLE_INVALID) return false;
  BLEConnection* conn = Bluefruit.Connection(activeConnHandle);
  return conn && conn->connected() && conn->secured() && conn->bonded();
}

bool sendPacket(uint8_t type, const uint8_t* payload, uint16_t len) {
  if (len > MAX_RESPONSE_PAYLOAD) return false;
  if (activeConnHandle == BLE_CONN_HANDLE_INVALID) return false;

  uint8_t frame[2 + 1 + 2 + MAX_RESPONSE_PAYLOAD + 2];
  size_t p = 0;

  frame[p++] = MAGIC1;
  frame[p++] = MAGIC2;
  frame[p++] = type;
  frame[p++] = (uint8_t)(len & 0xFF);
  frame[p++] = (uint8_t)(len >> 8);

  uint16_t crc = 0xFFFF;
  crc = crc16Update(crc, type);
  crc = crc16Update(crc, (uint8_t)(len & 0xFF));
  crc = crc16Update(crc, (uint8_t)(len >> 8));

  for (uint16_t i = 0; i < len; i++) {
    frame[p++] = payload[i];
    crc = crc16Update(crc, payload[i]);
  }

  frame[p++] = (uint8_t)(crc & 0xFF);
  frame[p++] = (uint8_t)(crc >> 8);

  return bleuart.write(frame, p) == p;
}

void sendError(uint8_t code, const char* message) {
  uint8_t payload[64];
  size_t msgLen = strlen(message);
  if (msgLen > sizeof(payload) - 1) msgLen = sizeof(payload) - 1;
  payload[0] = code;
  memcpy(payload + 1, message, msgLen);
  sendPacket(RSP_ERROR, payload, (uint16_t)(1 + msgLen));
}

void putU16(uint8_t* p, uint16_t value) {
  p[0] = (uint8_t)(value & 0xFF);
  p[1] = (uint8_t)(value >> 8);
}

void putU32(uint8_t* p, uint32_t value) {
  p[0] = (uint8_t)(value & 0xFF);
  p[1] = (uint8_t)((value >> 8) & 0xFF);
  p[2] = (uint8_t)((value >> 16) & 0xFF);
  p[3] = (uint8_t)((value >> 24) & 0xFF);
}

uint32_t getU32(const uint8_t* p) {
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

void sendInfo() {
  uint8_t payload[65] = {};

  putU32(payload + 0, storedCount);
  putU32(payload + 4, LOG_CAPACITY);
  putU32(payload + 8, newestSeq);
  putU32(payload + 12, newestRecord.epoch);
  putU32(payload + 16, (uint32_t)newestRecord.latE7);
  putU32(payload + 20, (uint32_t)newestRecord.lonE7);
  putU16(payload + 24, newestRecord.hdopX100);
  payload[26] = newestRecord.satellites;

  putU32(payload + 27, gps.charsProcessed());
  putU32(payload + 31, gps.sentencesWithFix());
  putU32(payload + 35, gps.failedChecksum());

  const int32_t currentLatE7 = gps.location.isValid()
      ? (int32_t)(gps.location.lat() * 10000000.0)
      : 0;
  const int32_t currentLonE7 = gps.location.isValid()
      ? (int32_t)(gps.location.lng() * 10000000.0)
      : 0;

  putU32(payload + 39, (uint32_t)currentLatE7);
  putU32(payload + 43, (uint32_t)currentLonE7);
  putU16(payload + 47,
         gps.hdop.isValid()
             ? (uint16_t)min((uint32_t)65535, (uint32_t)gps.hdop.value())
             : 0);
  payload[49] = gps.satellites.isValid()
      ? (uint8_t)min((uint32_t)255, (uint32_t)gps.satellites.value())
      : 0;

  uint8_t gpsFlags = 0;
  if (gps.location.isValid()) gpsFlags |= 0x01;
  if (gps.date.isValid()) gpsFlags |= 0x02;
  if (gps.time.isValid()) gpsFlags |= 0x04;
  payload[50] = gpsFlags;

  putU32(payload + 51,
         gps.location.isValid() ? gps.location.age() : 0xFFFFFFFFUL);

  uint8_t powerFlags = 0;
  if (gpsPowered) powerFlags |= 0x01;
  if (POWER_OPTIMIZATION_ENABLED) powerFlags |= 0x02;
  if (GPS_POWER_CONTROL_ENABLED) powerFlags |= 0x04;
  if (flashSleeping) powerFlags |= 0x08;
  payload[55] = powerFlags;

  putU16(payload + 56,
         (uint16_t)min((uint32_t)65535, secondsUntilGpsWake()));
  putU16(payload + 58, (uint16_t)logIntervalSeconds);

  putU16(payload + 60, 0); // reserved battery mV
  payload[62] = 1;
  payload[63] = 5;
  payload[64] = 2;

  sendPacket(RSP_INFO, payload, sizeof(payload));
}

void sendAck(uint8_t command) {
  sendPacket(RSP_ACK, &command, 1);
}

void startDownload(uint32_t afterSeq) {
  flashWake();

  if (newestSeq == 0 || storedCount == 0) {
    uint8_t done[4] = {};
    sendPacket(RSP_DOWNLOAD_DONE, done, sizeof(done));
    flashSleep();
    return;
  }

  uint32_t oldestCandidate = 1;
  if (newestSeq >= LOG_CAPACITY) {
    oldestCandidate = newestSeq - LOG_CAPACITY + 1;
  }

  uint32_t requested = afterSeq + 1;
  if (requested < oldestCandidate || requested == 0) requested = oldestCandidate;

  downloadSeq = requested;
  downloadEndSeq = newestSeq; // snapshot; new points logged during transfer come next time
  downloadSent = 0;
  downloadActive = true;
}

void serviceDownload() {
  if (!downloadActive || !connectionIsAuthorized()) return;

  if (downloadSeq > downloadEndSeq) {
    uint8_t payload[4];
    putU32(payload, downloadSent);
    if (sendPacket(RSP_DOWNLOAD_DONE, payload, sizeof(payload))) {
      downloadActive = false;
      flashSleep();
    }
    return;
  }

  // 8 * 20 + 1 = 161 byte payload.
  uint8_t payload[1 + 8 * sizeof(GpsRecord)];
  uint8_t count = 0;
  uint32_t cursor = downloadSeq;

  while (cursor <= downloadEndSeq && count < 8) {
    GpsRecord rec;
    if (readRecordBySeq(cursor, rec)) {
      memcpy(payload + 1 + count * sizeof(GpsRecord), &rec, sizeof(rec));
      count++;
    }
    cursor++;
  }

  if (count == 0) {
    downloadSeq = cursor;
    return;
  }

  payload[0] = count;
  const uint16_t len = 1 + count * sizeof(GpsRecord);

  if (sendPacket(RSP_DATA_BATCH, payload, len)) {
    downloadSeq = cursor;
    downloadSent += count;
  }
}

void handleCommand(uint8_t type, const uint8_t* payload, uint16_t len) {
  BLEConnection* activeConn = (activeConnHandle == BLE_CONN_HANDLE_INVALID)
      ? nullptr : Bluefruit.Connection(activeConnHandle);

  DBG_PRINTF("BLE command 0x%02X len=%u secured=%d bonded=%d\n",
                type,
                len,
                activeConn ? activeConn->secured() : 0,
                activeConn ? activeConn->bonded() : 0);

  if (!connectionIsAuthorized()) {
    sendError(ERR_PAIR_REQUIRED, "PAIR_REQUIRED");
    if (activeConn && activeConn->connected() && !activeConn->secured()) {
      DBG_PRINTLN("Command arrived before encryption; requesting security.");
      activeConn->requestPairing();
    }
    return;
  }

  switch (type) {
    case CMD_INFO_REQ:
      if (len != 0) {
        sendError(ERR_BAD_PAYLOAD, "INFO_LEN");
        return;
      }
      sendInfo();
      break;

    case CMD_DOWNLOAD_REQ:
      if (len != 4) {
        sendError(ERR_BAD_PAYLOAD, "DOWNLOAD_LEN");
        return;
      }
      startDownload(getU32(payload));
      break;

    case CMD_CLEAR_LOG_REQ:
      if (len != 0) {
        sendError(ERR_BAD_PAYLOAD, "CLEAR_LEN");
        return;
      }
      if (!clearLog()) {
        sendError(ERR_FLASH, "ERASE_FAILED");
        return;
      }
      sendAck(CMD_CLEAR_LOG_REQ);
      sendInfo();
      break;

    case CMD_PING:
      if (len != 0) {
        sendError(ERR_BAD_PAYLOAD, "PING_LEN");
        return;
      }
      sendAck(CMD_PING);
      break;

    case CMD_SET_INTERVAL:
      if (len != 4) {
        sendError(ERR_BAD_PAYLOAD, "INTERVAL_LEN");
        return;
      } else {
        const uint32_t requestedInterval = getU32(payload);
        if (!isAllowedLogInterval(requestedInterval)) {
          sendError(ERR_BAD_INTERVAL, "BAD_INTERVAL");
          return;
        }
        if (!setLogInterval(requestedInterval)) {
          sendError(ERR_FLASH, "INTERVAL_SAVE_FAILED");
          return;
        }

        // One response only. Android requests a fresh INFO after receiving
        // this ACK. This avoids back-to-back notifications competing for the
        // small BLE notification queue.
        sendAck(CMD_SET_INTERVAL);
      }
      break;

    default:
      sendError(ERR_BAD_COMMAND, "UNKNOWN_COMMAND");
      break;
  }
}

void resetRxParser() {
  rxState = 0;
  rxType = 0;
  rxLen = 0;
  rxPos = 0;
  rxCrcCalculated = 0xFFFF;
  rxCrcReceived = 0;
}

void feedProtocolByte(uint8_t b) {
  switch (rxState) {
    case 0:
      if (b == MAGIC1) rxState = 1;
      break;

    case 1:
      if (b == MAGIC2) rxState = 2;
      else rxState = (b == MAGIC1) ? 1 : 0;
      break;

    case 2:
      rxType = b;
      rxCrcCalculated = 0xFFFF;
      rxCrcCalculated = crc16Update(rxCrcCalculated, b);
      rxState = 3;
      break;

    case 3:
      rxLen = b;
      rxCrcCalculated = crc16Update(rxCrcCalculated, b);
      rxState = 4;
      break;

    case 4:
      rxLen |= (uint16_t)b << 8;
      rxCrcCalculated = crc16Update(rxCrcCalculated, b);

      if (rxLen > MAX_COMMAND_PAYLOAD) {
        resetRxParser();
      } else if (rxLen == 0) {
        rxState = 6;
      } else {
        rxPos = 0;
        rxState = 5;
      }
      break;

    case 5:
      rxPayload[rxPos++] = b;
      rxCrcCalculated = crc16Update(rxCrcCalculated, b);
      if (rxPos >= rxLen) rxState = 6;
      break;

    case 6:
      rxCrcReceived = b;
      rxState = 7;
      break;

    case 7:
      rxCrcReceived |= (uint16_t)b << 8;
      if (rxCrcReceived == rxCrcCalculated) {
        handleCommand(rxType, rxPayload, rxLen);
      }
      resetRxParser();
      break;

    default:
      resetRxParser();
      break;
  }
}

void serviceBleRx() {
  while (bleuart.available()) {
    feedProtocolByte((uint8_t)bleuart.read());
  }
}

// ---------------- BLE callbacks/setup ----------------

void pairCompleteCallback(uint16_t connHandle, uint8_t authStatus) {
  BLEConnection* conn = Bluefruit.Connection(connHandle);
  if (!conn) return;

  if (authStatus != BLE_GAP_SEC_STATUS_SUCCESS || !conn->bonded()) {
    DBG_PRINTF("Pairing failed/status=0x%02X\n", authStatus);
    return;
  }

  const ble_gap_addr_t peerIdentity = conn->getPeerAddr();

  if (!ownerSet) {
    if (!saveOwnerLock(peerIdentity)) {
      DBG_PRINTLN("Could not save owner identity; disconnecting.");
      conn->disconnect();
      return;
    }
    DBG_PRINTLN("This phone is now the tracker owner.");
  } else if (!ownerMatches(peerIdentity)) {
    DBG_PRINTLN("Pairing identity does not match owner; disconnecting.");
    conn->disconnect();
  }
}

void connectCallback(uint16_t connHandle) {
  activeConnHandle = connHandle;
  downloadActive = false;
  resetRxParser();

  BLEConnection* conn = Bluefruit.Connection(connHandle);
  if (!conn) return;

  char peerName[32] = {};
  conn->getPeerName(peerName, sizeof(peerName));

  DBG_PRINT("BLE connected: ");
  DBG_PRINTLN(peerName);

  conn->requestMtuExchange(247);
  conn->requestPHY(BLE_GAP_PHY_2MBPS);

  if (ownerSet) {
    // Resolve the incoming BLE address against stored bond keys.
    // Unknown/unbonded devices are rejected before they can start a new pairing.
    bond_keys_t keys;
    if (!conn->loadBondKey(&keys) || !ownerMatches(conn->getPeerAddr())) {
      DBG_PRINTLN("Rejected non-owner BLE device.");
      conn->disconnect();
      return;
    }

    DBG_PRINTLN("Known owner device connected; restoring encrypted bond.");

    // Adafruit BLEUart creates its NUS characteristics with open GATT
    // permissions. A successful NUS subscription therefore does not prove
    // this connection has restored encryption. Explicitly authenticate.
    // With an existing bond, the saved keys are reused (no new PIN prompt).
    if (!conn->requestPairing()) {
      DBG_PRINTLN("Could not start bonded security procedure; disconnecting.");
      conn->disconnect();
      return;
    }
  } else {
    // First successful bond claims ownership.
    DBG_PRINTLN("No owner yet; requesting first-time pairing.");
    conn->requestPairing();
  }
}

void disconnectCallback(uint16_t connHandle, uint8_t reason) {
  (void)connHandle;
  DBG_PRINTF("BLE disconnected, reason=0x%02X\n", reason);
  activeConnHandle = BLE_CONN_HANDLE_INVALID;
  downloadActive = false;
  flashSleep();
  resetRxParser();
}

void securedCallback(uint16_t connHandle) {
  BLEConnection* conn = Bluefruit.Connection(connHandle);
  if (!conn) return;

  DBG_PRINTF("BLE secured. bonded=%d\n", conn->bonded());

  if (ownerSet && conn->bonded() && !ownerMatches(conn->getPeerAddr())) {
    DBG_PRINTLN("Encrypted peer is not owner; disconnecting.");
    conn->disconnect();
  }
}

void setupBle() {
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  Bluefruit.begin(1, 0);
  Bluefruit.setName(BLE_DEVICE_NAME);
  Bluefruit.setTxPower(0);

  if (ownerResetRequested) {
    Bluefruit.Periph.clearBonds();
    if (!clearOwnerLock()) {
      DBG_PRINTLN("WARNING: failed to clear owner metadata.");
    } else {
      DBG_PRINTLN("BLE bonds and owner lock cleared. GPS interval/log were preserved.");
    }
  }

  Bluefruit.Security.setPIN(BLE_PAIRING_PIN);
  Bluefruit.Security.setPairCompleteCallback(pairCompleteCallback);
  Bluefruit.Security.setSecuredCallback(securedCallback);

  Bluefruit.Periph.setConnectCallback(connectCallback);
  Bluefruit.Periph.setDisconnectCallback(disconnectCallback);

  // BLEUart's internal NUS characteristics are open in the Adafruit
  // implementation. We explicitly establish the encrypted bond in the
  // connection callback, and handleCommand() independently requires
  // secured() + bonded() before accepting tracker commands.
  bleuart.begin();

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(bleuart);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(160, 1600); // 100 ms fast, 1 s slow
  Bluefruit.Advertising.setFastTimeout(10);
  Bluefruit.Advertising.start(0);

  DBG_PRINTF("Advertising as %s, pairing PIN %s\n", BLE_DEVICE_NAME, BLE_PAIRING_PIN);
  DBG_PRINTF("Owner lock: %s\n", ownerSet ? "SET" : "not set (first phone may pair)");
}

// ---------------- Arduino ----------------

void setup() {
  debugSerialActive = DEBUG_SERIAL_ON_USB && usbVbusPresent();

  if (debugSerialActive) {
    Serial.begin(115200);
    delay(350);
    DBG_PRINTLN0();
    DBG_PRINTLN("XIAO GPS Logger starting");
    DBG_PRINTF("FW VERSION: %s\n", FW_VERSION);
    DBG_PRINTLN("USB Serial Monitor baud: 115200");
    DBG_PRINTF("GPS Serial1 baud: %lu\n", (unsigned long)GPS_BAUD);
  }

  configureBatteryCharging();

  if (GPS_POWER_CONTROL_ENABLED) {
    pinMode(GPS_POWER_PIN, OUTPUT);
    setGpsPowerControl(false);
  }

#ifdef LED_RED
  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_RED, HIGH);
#endif
#ifdef LED_GREEN
  pinMode(LED_GREEN, OUTPUT);
  digitalWrite(LED_GREEN, HIGH);
#endif
#ifdef LED_BLUE
  pinMode(LED_BLUE, OUTPUT);
  digitalWrite(LED_BLUE, HIGH);
#endif

  if (!flash.begin(flashDevices, 1)) {
    DBG_PRINTLN("FATAL: P25Q16H QSPI flash not detected");
    while (true) delay(1000);
  }

  DBG_PRINTF("QSPI JEDEC=0x%06lX size=%lu bytes\n",
             (unsigned long)flash.getJEDECID(),
             (unsigned long)flash.size());

  if (flash.size() < FLASH_SIZE_BYTES) {
    DBG_PRINTLN("FATAL: unexpected flash size");
    while (true) delay(1000);
  }

  if (!loadOwnerLock()) {
    DBG_PRINTLN("FATAL: tracker metadata read failed");
    while (true) delay(1000);
  }

  DBG_PRINTF("Configured GPS interval: %s (%lus)\n",
             intervalLabel(logIntervalSeconds),
             (unsigned long)logIntervalSeconds);

  ownerResetRequested = checkOwnerResetPin();

  if (!scanFlash()) {
    DBG_PRINTLN("FATAL: flash scan failed");
    while (true) delay(1000);
  }

  gpsEverHadFix = newestSeq != 0;
  flashSleep();

  setupBle();

  gpsNextWakeMs = millis();
  serviceGpsPowerState();
}

void loop() {
  serviceGpsPowerState();
  serviceGps();
  serviceGpsDiagnostics();
  serviceBleRx();
  serviceDownload();

  if (downloadActive || activeConnHandle != BLE_CONN_HANDLE_INVALID) {
    delay(2);
  } else if (gpsPowered) {
    delay(10);
  } else {
    delay(25);
  }
}