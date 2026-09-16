/*
  XIAO nRF52840 GPS logger + bonded BLE download

  Target board:
    Seeed XIAO nRF52840
    IMPORTANT: use the non-mbed "Seeed nRF52 Boards" Arduino core.

  GPS wiring:
    GPS TX  -> XIAO D7 / Serial1 RX
    GPS RX  -> XIAO D6 / Serial1 TX
    GPS GND -> XIAO GND
    ATGM336H VCC -> XIAO 3V3 (always powered)
    D1 is unused.

  Motion wiring:
    MPU6050 VCC -> 3V3, GND -> GND, SDA -> D4, SCL -> D5,
    INT -> D2.  The MPU is sampled through direct registers; no sensor
    library is required.

  Libraries:
    TinyGPSPlus
    Adafruit SPIFlash
    Bluefruit52Lib is supplied by the Seeed/Adafruit nRF52 board core.

  BLE:
    Nordic UART Service (NUS)
    Pairing passkey below. CHANGE IT before deployment.
    The first successfully bonded phone becomes the owner.
    Later unknown/unbonded phones are disconnected.  Tracker v2 adds a
    persisted Interval/Smart mode while retaining the v1.5 packet layout.

  Storage:
    Raw circular log in the XIAO's 2 MiB P25Q16H QSPI flash.
    Sector 0 stores tracker metadata (owner phone, GPS interval, and v2
    Interval/Smart configuration).
    Remaining sectors hold 20-byte GPS records in a circular log.
    The GPS interval is configurable from Android: 1 min, 15 min, 30 min,
    1 h, 2 h, or 3 h.
*/

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_TinyUSB.h>
#include <bluefruit.h>
#include <TinyGPS++.h>
#include <Adafruit_SPIFlash.h>
#include <flash_devices.h>
#include <math.h>

// ---------------- User settings ----------------

static const char BLE_DEVICE_NAME[] = "XIAO-GPS";
static const char FW_VERSION[] = "2.0.0";
static const char BLE_PAIRING_PIN[] = "482731"; // CHANGE THIS, exactly 6 digits
static const uint32_t DEFAULT_LOG_INTERVAL_SECONDS = 1800; // 30 min
static const uint32_t GPS_BAUD = 9600;
static const uint8_t OWNER_RESET_PIN = D0;       // hold to GND at boot
static const uint32_t OWNER_RESET_HOLD_MS = 5000;

// ---------------- Runtime/power profile ----------------
//
// Target: a 1S LiPo on the XIAO battery input; capacity is installation-specific.
//
// The supplied v2 hardware powers the bare ATGM336H from 3V3 continuously.
// gpsPowered below therefore means "the GPS UART/parser is active", never
// "the receiver supply is physically on".  No firmware path claims a physical
// GPS power-off capability on this wiring.
static const bool POWER_OPTIMIZATION_ENABLED = true;
static const bool GPS_POWER_CONTROL_ENABLED = false;
static const bool GPS_SUPPLY_ALWAYS_ON = true;

// For intervals longer than one minute the GPS UART is normally closed.
// It reopens this many seconds before the next point is due.  The receiver
// itself remains supplied, so this is a runtime/parser optimization rather
// than a physical power gate.
//
// The 1-minute profile intentionally keeps the GPS powered after the first fix.
// With this 5-pin carrier there is no exposed VBAT, so repeatedly cold-starting
// every minute is both unreliable and often less efficient than staying locked.
static const uint32_t GPS_ACQUIRE_LEAD_SECONDS = 60;
static const uint32_t GPS_ACQUIRE_TIMEOUT_MS = 120000;
static const uint32_t GPS_RETRY_SLEEP_MS = 60000;
static const uint32_t GPS_POWER_SETTLE_MS = 250;

// ---------------- Smart Motion v2 ----------------
//
// MPU6050 register settings: ±2 g accelerometer, ±250 dps gyroscope, DLPF
// enabled, and a 20 Hz data-ready source for verification.  Smart's armed
// state uses the MPU's own 5 Hz accelerometer-only cycle; the MCU performs no
// periodic armed-state sample reads.  Verification consumes one raw sample
// every 50 ms (20 Hz), then removes gravity in software.
static const uint8_t MPU6050_ADDRESS = 0x68;
static const uint8_t MPU_SDA_PIN = D4;
static const uint8_t MPU_SCL_PIN = D5;
static const uint8_t MPU_INT_PIN = D2;
static const uint8_t MPU_REG_SMPLRT_DIV = 0x19;
static const uint8_t MPU_REG_CONFIG = 0x1A;
static const uint8_t MPU_REG_GYRO_CONFIG = 0x1B;
static const uint8_t MPU_REG_ACCEL_CONFIG = 0x1C;
static const uint8_t MPU_REG_MOT_THR = 0x1F;
static const uint8_t MPU_REG_MOT_DUR = 0x20;
static const uint8_t MPU_REG_INT_STATUS = 0x3A;
static const uint8_t MPU_REG_INT_PIN_CFG = 0x37;
static const uint8_t MPU_REG_INT_ENABLE = 0x38;
static const uint8_t MPU_REG_ACCEL_XOUT_H = 0x3B;
static const uint8_t MPU_REG_MOT_DETECT_CTRL = 0x69;
static const uint8_t MPU_REG_PWR_MGMT_1 = 0x6B;
static const uint8_t MPU_REG_PWR_MGMT_2 = 0x6C;
static const uint8_t MPU_REG_WHO_AM_I = 0x75;

// Armed mode is handled inside the MPU's low-power accelerometer cycle.  The
// MCU performs no periodic acceleration reads in that state; it wakes only
// when the latched motion interrupt asserts.  Verification then changes to a
// 20 Hz data-ready profile and reads acceleration in the MCU loop.
static const uint32_t SMART_VERIFY_WINDOW_MS = 5000; // confirmationSeconds
// A nominal 100th sample can arrive just after 5 s because the interrupt and
// I2C read are asynchronous.  The sample count, not this watchdog, defines
// the confirmation window; the watchdog only rejects a candidate that never
// produces all 100 samples.
// Allow only 100 ms of loop/interrupt servicing tolerance beyond the nominal
// 5-second, 100-sample window.  This prevents a slower 16-19 Hz stream from
// being accepted as the configured 20 Hz confirmation profile.
static const uint32_t SMART_VERIFY_WATCHDOG_MS = 5100;
static const uint16_t SMART_VERIFY_REQUIRED_ACTIVE_SAMPLES = 60;
static const uint16_t SMART_VERIFY_TOTAL_SAMPLES = 100;
static const uint32_t SMART_VERIFY_MAX_QUIET_GAP_MS = 1000;
static const uint32_t SMART_QUIET_GAP_MS = 2000;
static const uint32_t SMART_MOTION_RECENCY_MS = 1500;
static const uint32_t SMART_ACQUISITION_TIMEOUT_MS = 90000;
static const uint32_t SMART_COOLDOWN_MS = 120000;
// Keep the UART/parser open for the final 10 seconds of every Smart cooldown.
// This observation window is deliberately short so it does not turn cooldown
// into a full-time receiver session, but is long enough for fresh 1 Hz RMC/GGA
// fixes before the cooldown classifier runs.
static const uint32_t SMART_COOLDOWN_GNSS_OBSERVATION_MS = 10000;
// Receiver-side standby is selected at build time because current draw and
// resume latency must be measured on the actual ATGM336H assembly first.  The
// 60-second default is deliberately provisional; a build may select only one
// of the measured 30/60/120-second profiles with
// -DSMART_STANDBY_SLICE_SECONDS_CONFIG=30, 60, or 120.
#ifndef SMART_STANDBY_SLICE_SECONDS_CONFIG
#define SMART_STANDBY_SLICE_SECONDS_CONFIG 60U
#endif
static_assert(SMART_STANDBY_SLICE_SECONDS_CONFIG == 30U ||
              SMART_STANDBY_SLICE_SECONDS_CONFIG == 60U ||
              SMART_STANDBY_SLICE_SECONDS_CONFIG == 120U,
              "SMART_STANDBY_SLICE_SECONDS_CONFIG must be 30, 60, or 120");
static const uint16_t SMART_STANDBY_SLICE_SECONDS =
    (uint16_t)SMART_STANDBY_SLICE_SECONDS_CONFIG;
static const uint16_t SMART_FIX_COOLDOWN_SECONDS = 120;
static const uint32_t SMART_GNSS_CONTINUED_SPEED_CM_S = 100; // 1 m/s
static const uint32_t SMART_GNSS_STALE_MS = 5000;
// GNSS displacement is an independent continued-motion signal.  A 20 m
// threshold is conservative for consumer GNSS noise and is evaluated only
// with a fresh, navigation-valid fix and the reference from the last stored
// Smart fix (or the most recent cooldown classification).
static const float SMART_GNSS_CONTINUED_DISPLACEMENT_METERS = 20.0f;
static const uint8_t SMART_GNSS_MIN_SATELLITES = 4;
static const uint16_t SMART_GNSS_MAX_HDOP_X100 = 300; // HDOP <= 3.00
static const uint8_t SMART_DEFAULT_SENSITIVITY = 1;
static const uint8_t SMART_MAX_SENSITIVITY = 2;
static const uint16_t SMART_HIGH_THRESHOLD_MG = 80;
static const uint16_t SMART_BALANCED_THRESHOLD_MG = 160;
static const uint16_t SMART_LOW_THRESHOLD_MG = 300;

// PS-MPU-6000A-00 rev 3.3, section 8.1: the low-power wake profile
// uses DLPF_CFG=0 and MOT_DUR=1. A 100-count hardware filter must not
// be treated as a 100 ms debounce in the 5 Hz cycle profile. The five-second
// software verifier rejects bumps after this deliberately permissive wake.
static const uint8_t MPU_MOTION_DURATION = 1;
static const uint8_t MPU_CONFIG_ARMED = 0x00;
// The rev 3.3 low-power motion flowchart specifies 32 mg/LSB. Older
// documentation/examples disagree on this scale, so field calibration on the
// actual module is still required. This is a permissive wake threshold only;
// the 20 Hz software verifier uses raw acceleration and the exact mg preset.
static const uint8_t MPU_MOTION_THRESHOLD_MG_PER_LSB = 32;
static const uint8_t MPU_MOTION_DETECT_CTRL = 0x00;
static const uint8_t MPU_INT_PIN_CFG_LATCHED_ACTIVE_HIGH = 0x20;
static const uint8_t MPU_INT_ENABLE_MOTION = 0x40;
static const uint8_t MPU_INT_ENABLE_DATA_READY = 0x01;
static const uint8_t MPU_INT_STATUS_MOTION = 0x40;
static const uint8_t MPU_INT_STATUS_DATA_READY = 0x01;
static const uint8_t MPU_PWR_MGMT_1_ARMED = 0x28; // CYCLE + TEMP_DIS
static const uint8_t MPU_PWR_MGMT_1_VERIFY = 0x08; // active, TEMP_DIS
// PWR_MGMT_2: LP_WAKE_CTRL=01 selects 5 Hz (bits 7..6), accelerometer
// standby is bits 5..3, and gyro standby is bits 2..0.  Thus 0x47 is the
// accel-only 5 Hz armed profile and 0x07 keeps the accel active for verify.
static const uint8_t MPU_PWR_MGMT_2_ARMED = 0x47;
static const uint8_t MPU_PWR_MGMT_2_VERIFY = 0x07;
static const uint8_t MPU_ACCEL_CONFIG_ARMED = 0x07; // DHPF hold initial gravity
static const uint8_t MPU_ACCEL_CONFIG_VERIFY = 0x00; // ±2 g, raw data for LPF
static const uint8_t MPU_MOTION_THRESHOLD_REGISTER_MAX = 255;

// CAS12 is an optional receiver-side standby optimization.  It is probed only
// after NMEA has been observed and must pass two complete silence/resumption
// cycles (including fresh RMC+GGA navigation) before use.  Smart never depends
// on this optimization: UART inactivity remains the fallback.
static const uint16_t CAS12_PROBE_STANDBY_SECONDS = 5;
static const uint32_t CAS12_PROBE_QUIET_GAP_MS = 1500;
static const uint32_t CAS12_PROBE_RESUME_TIMEOUT_MS = 7000;
static const uint32_t CAS12_PROBE_MIN_ELAPSED_MS =
    (uint32_t)CAS12_PROBE_STANDBY_SECONDS * 1000UL;
static const uint32_t CAS12_PROBE_CYCLE_TIMEOUT_MS =
    CAS12_PROBE_MIN_ELAPSED_MS + CAS12_PROBE_RESUME_TIMEOUT_MS;
static const uint32_t CAS12_NMEA_WAIT_TIMEOUT_MS = 15000;

// Release build logging: serial diagnostics are automatically enabled on USB
// power, but stay disabled on battery so TinyUSB/printing does not waste energy.
static const bool DEBUG_SERIAL_ON_USB = true;

// XIAO nRF52840 supports 50 mA / 100 mA charging. 50 mA is intentionally used
// for the current 1000 mAh assembly's charge profile.
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

// v1.0 legacy owner record magic and combined metadata magic.  Metadata v2
// consumes only the first two bytes that v1.5 left reserved, so owner,
// interval, and the 20-byte log records remain byte-for-byte compatible.
static const uint32_t LEGACY_OWNER_MAGIC = 0x314E574FUL; // "OWN1"
static const uint32_t METADATA_MAGIC = 0x354B5254UL;     // "TRK5"
static const uint8_t METADATA_VERSION_V1 = 1;
static const uint8_t METADATA_VERSION_V2 = 2;
static const uint8_t METADATA_RESERVED_MODE = 0;
static const uint8_t METADATA_RESERVED_SENSITIVITY = 1;

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

enum TrackingMode : uint8_t {
  TRACKING_INTERVAL = 0,
  TRACKING_SMART = 1
};

enum SmartState : uint8_t {
  SMART_DISABLED = 0,
  SMART_ARMED = 1,
  SMART_VERIFYING = 2,
  SMART_ACQUIRING = 3,
  SMART_TRACKING = 4,
  SMART_COOLDOWN = 5
};

enum Cas12State : uint8_t {
  CAS12_UNKNOWN = 0,
  CAS12_PROBING = 1,
  CAS12_SUPPORTED = 2,
  CAS12_UNSUPPORTED = 3
};

enum SmartWakeReason : uint8_t {
  SMART_WAKE_NONE = 0,
  SMART_WAKE_MOTION = 1,
  SMART_WAKE_RETRY = 2
};

static const uint32_t RECORD_SIZE = sizeof(GpsRecord);
static const uint32_t RECORDS_PER_SECTOR = SECTOR_SIZE / RECORD_SIZE; // 204
static const uint32_t LOG_CAPACITY =
    RECORDS_PER_SECTOR * LOG_SECTOR_COUNT; // 104244 (~72.4 days @ 1/min)

static uint8_t sectorBuffer[RECORDS_PER_SECTOR * RECORD_SIZE];

static MetadataRecord metadataRecord = {};
static bool ownerSet = false;
static bool ownerResetRequested = false;
static uint32_t logIntervalSeconds = DEFAULT_LOG_INTERVAL_SECONDS;
static TrackingMode trackingMode = TRACKING_INTERVAL;
// Smart sensitivity is persisted in MetadataRecord.reserved[1].  Values are
// intentionally bounded so an all-0xFF/corrupt record cannot select an
// undocumented classifier profile.
static uint8_t smartSensitivity = SMART_DEFAULT_SENSITIVITY;
// A Smart selection remains persisted when the motion hardware is unavailable,
// but this boot deliberately runs the proven Interval scheduler.  Keeping the
// fallback separate from trackingMode lets INFO/0x85 report the saved mode
// and the runtime limitation truthfully.
static bool runtimeIntervalFallback = false;

static uint32_t newestSeq = 0;
static uint32_t storedCount = 0;
static uint32_t lastStoredEpoch = 0;
static GpsRecord newestRecord = {};
static uint32_t lastGpsDiagMs = 0;
static bool firstGpsDiag = true;

static bool debugSerialActive = false;
// Logical GPS runtime state: true means Serial1 is open and this sketch is
// accepting NMEA.  The actual receiver supply is always on for v2 hardware.
static bool gpsPowered = false;
static bool gpsEverHadFix = false;
static uint32_t gpsAcquireStartedMs = 0;
static uint32_t gpsNextWakeMs = 0;
// Monotonic firmware-owned location-update counter.  TinyGPS++ exposes a
// latched location freshness bit; serviceGps() consumes that bit by calling
// the non-const lat()/lng() accessors and advances this counter only alongside
// a newly counted valid-fix sentence.
static uint32_t gpsLocationUpdateCounter = 0;
// Date/time freshness is tracked separately so a post-start GGA cannot pair
// a new coordinate with UTC that was cached before acquisition began.
static uint32_t gpsUtcUpdateCounter = 0;
static bool flashSleeping = false;

// ---------------- MPU6050 / Smart runtime ----------------

volatile bool mpuDataReadyPending = false;
volatile uint32_t mpuIrqCount = 0;
static uint32_t mpuMotionEventCount = 0;
static uint8_t mpuLastInterruptStatus = 0;
static bool mpuAvailable = false;
static bool mpuInterruptAttached = false;
static bool mpuSampleInitialized = false;
static uint32_t mpuLastSampleMs = 0;
static uint32_t mpuLastMotionMs = 0;
static uint32_t mpuQuietSinceMs = 0;
static float mpuGravityX = 0.0f;
static float mpuGravityY = 0.0f;
static float mpuGravityZ = 0.0f;
static uint16_t mpuDynamicMagnitudeMg = 0;

static SmartState smartState = SMART_DISABLED;
static uint32_t smartStateSinceMs = 0;
static uint32_t smartAcquisitionStartedMs = 0;
static uint32_t smartCooldownUntilMs = 0;
static uint32_t smartLastMotionMs = 0;
static bool smartRetryAfterCooldown = false;
static bool smartFirstFixPending = false;
static uint32_t smartAcquisitionFixSentenceBaseline = 0;
static uint32_t smartAcquisitionLocationBaseline = 0;
static uint32_t smartAcquisitionUtcBaseline = 0;
static bool smartAcquisitionNmeaSynchronized = true;
static bool smartStartupProbePending = false;
static bool smartMotionDetected = false;
static bool smartCooldownObservationOpen = false;
static uint32_t smartVerifyStartedMs = 0;
static uint16_t smartVerifyPeakMg = 0;
static uint16_t smartVerifySamples = 0;
static uint16_t smartVerifyActiveSamples = 0;
static uint32_t smartVerifyLastActiveMs = 0;
static SmartWakeReason lastWakeReason = SMART_WAKE_NONE;

// The acquisition fix is the displacement baseline.  It is intentionally
// retained through the 120 s cooldown while fresh GNSS is observed, then
// advanced after the cooldown classification so repeated checks measure a new
// interval instead of accumulating distance from boot.
static bool smartMotionReferenceValid = false;
static double smartMotionReferenceLat = 0.0;
static double smartMotionReferenceLon = 0.0;
static uint32_t smartMotionReferenceEpoch = 0;

// ---------------- NMEA/CAS12 runtime probe ----------------

static bool nmeaSeen = false;
static uint32_t lastNmeaByteMs = 0;
static uint32_t nmeaByteCount = 0;
static uint8_t nmeaHeader[6] = {};
static uint8_t nmeaHeaderLength = 0;
static bool nmeaInSentence = false;
static uint32_t nmeaRmcCount = 0;
static uint32_t nmeaGgaCount = 0;
static uint32_t nmeaRmcCountAtProbe = 0;
static uint32_t nmeaGgaCountAtProbe = 0;
static uint32_t nmeaBytesAtProbe = 0;
static bool cas12ProbeAttempted = false;
static uint8_t cas12ProbeCycle = 0;
static uint8_t cas12ProbePhase = 0;
static uint32_t cas12ProbeCycleStartedMs = 0;
static bool cas12ProbeQuietObserved = false;
static uint32_t cas12StartupSinceMs = 0;
static Cas12State cas12State = CAS12_UNKNOWN;
static bool cas12NmeaConfigured = false;
static bool gpsStandby = false;
static bool gpsStandbyPending = false;
static uint32_t gpsStandbyRequestedMs = 0;
static uint32_t gpsStandbyUntilMs = 0;

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
  CMD_GET_SMART_INFO  = 0x06,
  CMD_SET_SMART_CONFIG = 0x07,

  RSP_INFO           = 0x81,
  RSP_DATA_BATCH     = 0x82,
  RSP_DOWNLOAD_DONE  = 0x83,
  RSP_ACK            = 0x84,
  RSP_SMART_INFO     = 0x85,
  RSP_ERROR          = 0xFF
};

enum ErrorCode : uint8_t {
  ERR_PAIR_REQUIRED = 1,
  ERR_BAD_COMMAND   = 2,
  ERR_BAD_PAYLOAD   = 3,
  ERR_FLASH         = 4,
  ERR_BUSY          = 5,
  ERR_BAD_INTERVAL   = 6,
  ERR_BAD_MODE       = 7,
  ERR_BAD_SENSITIVITY = 8,
  ERR_SMART_STATUS   = 9
};

// RSP_SMART_INFO (0x85) flags.  Its payload is always exactly 12 bytes; these
// bits occupy only byte 10, leaving bytes 0..9 and 11 stable for the protocol.
static const uint8_t SMART_INFO_FLAG_MPU_PRESENT = 0x01;
static const uint8_t SMART_INFO_FLAG_MPU_INTERRUPT_ARMED = 0x02;
static const uint8_t SMART_INFO_FLAG_CAS12_VERIFIED = 0x04;
static const uint8_t SMART_INFO_FLAG_CAS12_ACTIVE = 0x08;
static const uint8_t SMART_INFO_FLAG_RUNTIME_INTERVAL_FALLBACK = 0x10;
static const uint8_t SMART_INFO_FLAG_GPS_RECEIVER_ACTIVE = 0x20;
static const uint8_t SMART_INFO_PROTOCOL_VERSION = 2;

// Existing INFO byte 55 keeps its v1.0 meanings in bits 0..3.  Bits 4..7 are
// additive v2 capability/runtime flags and do not change its 65-byte layout.
static const uint8_t INFO_POWER_GPS_UART_ACTIVE = 0x01;
static const uint8_t INFO_POWER_OPTIMIZATION = 0x02;
static const uint8_t INFO_POWER_PHYSICAL_GATE = 0x04;
static const uint8_t INFO_POWER_FLASH_SLEEPING = 0x08;
static const uint8_t INFO_POWER_SUPPLY_ALWAYS_ON = 0x10;
static const uint8_t INFO_POWER_MPU_AVAILABLE = 0x20;
static const uint8_t INFO_POWER_CAS12_SUPPORTED = 0x40;
static const uint8_t INFO_POWER_GPS_STANDBY = 0x80;

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

bool elapsedMs(uint32_t now, uint32_t started, uint32_t duration) {
  return (uint32_t)(now - started) >= duration;
}

const char* trackingModeLabel(TrackingMode mode) {
  return mode == TRACKING_SMART ? "Smart" : "Interval";
}

const char* smartStateLabel(SmartState state) {
  switch (state) {
    case SMART_ARMED: return "ARMED";
    case SMART_VERIFYING: return "VERIFYING";
    case SMART_ACQUIRING: return "ACQUIRING";
    case SMART_TRACKING: return "TRACKING";
    case SMART_COOLDOWN: return "COOLDOWN";
    default: return "DISABLED";
  }
}

bool smartRuntimeActive() {
  return trackingMode == TRACKING_SMART && !runtimeIntervalFallback;
}

uint16_t smartSensitivityThresholdMg() {
  switch (smartSensitivity) {
    case 0: return SMART_HIGH_THRESHOLD_MG;
    case 1: return SMART_BALANCED_THRESHOLD_MG;
    case 2: return SMART_LOW_THRESHOLD_MG;
    default: return SMART_BALANCED_THRESHOLD_MG;
  }
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
  if (smartRuntimeActive() || gpsPowered) return 0;
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

  // v2 hardware has no connected gate.  Keep this guarded hook for a future
  // board variant, but never describe UART inactivity as physical power-off.
  (void)enabled;
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
  DBG_PRINTLN("GPS runtime: UART active (receiver supply remains on)");
}

void gpsPowerOff() {
  if (gpsPowered) {
    Serial1.end();
    setGpsPowerControl(false);
  }
  gpsPowered = false;
  gpsStandby = false;
  gpsStandbyPending = false;
  gpsStandbyRequestedMs = 0;
  gpsStandbyUntilMs = 0;
  DBG_PRINTLN("GPS runtime: UART inactive (receiver supply remains on)");
}

bool gpsReceiverActiveCapability() {
  // On the assembled v2 board the receiver rail is always supplied, even when
  // Serial1 is closed for a runtime/parser optimization.  CAS12 standby is
  // the only state in which the receiver itself is intentionally quiescent.
  if (GPS_SUPPLY_ALWAYS_ON) return !gpsStandby;
  // A future gated board can only claim receiver activity while its UART/gate
  // runtime is known active.  No physical-gate bit is exposed in Smart Info.
  return gpsPowered && !gpsStandby;
}

bool smartCooldownObservationWindowDue(uint32_t now) {
  if (smartState != SMART_COOLDOWN || smartCooldownUntilMs == 0) return false;

  // SMART_COOLDOWN_MS is fixed at 120 s and the observation window is much
  // shorter, so this subtraction remains rollover-safe with timeReached().
  const uint32_t observationStart =
      smartCooldownUntilMs - SMART_COOLDOWN_GNSS_OBSERVATION_MS;
  return timeReached(now, observationStart);
}

void serviceGpsStandbyState() {
  if (!gpsStandbyPending || !gpsPowered) return;

  const uint32_t now = millis();
  // Give serviceGps() a chance to account for bytes already buffered in the
  // UART before interpreting silence as receiver-side standby.
  if (Serial1.available() > 0) return;
  const bool quietAfterRequest =
      lastNmeaByteMs != 0 &&
      elapsedMs(now, gpsStandbyRequestedMs, CAS12_PROBE_QUIET_GAP_MS) &&
      elapsedMs(now, lastNmeaByteMs, CAS12_PROBE_QUIET_GAP_MS);
  if (quietAfterRequest) {
    // CAS12 has no required acknowledgement.  Report receiver-side standby
    // only after the expected NMEA silence is actually observed.
    gpsStandbyPending = false;
    gpsStandby = true;
    gpsStandbyUntilMs = now + SMART_STANDBY_SLICE_SECONDS * 1000UL;
    DBG_PRINTLN("GPS runtime: CAS12 standby confirmed by NMEA silence");
    return;
  }

  if (elapsedMs(now, gpsStandbyRequestedMs, CAS12_PROBE_RESUME_TIMEOUT_MS)) {
    // A receiver that keeps talking (or never enters standby) is not marked
    // active.  UART close is the portable fallback and Smart remains correct.
    DBG_PRINTLN("GPS runtime: CAS12 standby not confirmed; using UART fallback");
    gpsPowerOff();
  }
}

void scheduleGpsSleep(uint32_t sleepMs) {
  if (!POWER_OPTIMIZATION_ENABLED) return;

  gpsPowerOff();
  gpsNextWakeMs = millis() + sleepMs;
}

void serviceGpsPowerState() {
  serviceGpsStandbyState();

  if (smartRuntimeActive()) {
    const uint32_t now = millis();
    // Smart's logical GPS runtime is controlled by its explicit state
    // machine.  The startup exception leaves UART open long enough to observe
    // NMEA and perform the one-boot optional CAS12 probe.
    if (smartStartupProbePending || smartState == SMART_ACQUIRING ||
        smartState == SMART_TRACKING) {
      if (!gpsPowered && !gpsStandby) gpsPowerOn();
    } else if (smartState == SMART_COOLDOWN) {
      // Keep the cooldown's final observation window alive for both CAS12 and
      // UART-fallback boots.  serviceGps() parses NMEA here but its Smart path
      // never stores a record while cooldown is active.  Leave this state
      // powered through the expiry iteration so serviceSmartState() can make
      // the speed/displacement decision from a genuinely fresh fix.
      if (smartCooldownObservationWindowDue(now)) {
        if (!smartCooldownObservationOpen) {
          smartCooldownObservationOpen = true;
          DBG_PRINTLN("Smart cooldown: opening final 10 s GNSS observation window.");
        }
        if (gpsStandby && timeReached(now, gpsStandbyUntilMs)) {
          // The finite receiver-side standby has elapsed; allow the resumed
          // NMEA stream to be consumed instead of treating the old marker as
          // an active standby indefinitely.
          gpsStandby = false;
          gpsStandbyUntilMs = 0;
        }
        if (!gpsPowered && !gpsStandby) gpsPowerOn();
      }
    } else if (smartState == SMART_ARMED ||
               smartState == SMART_VERIFYING) {
      if (gpsPowered && !gpsStandby && !gpsStandbyPending &&
          cas12State != CAS12_PROBING) {
        gpsPowerOff();
      }
    }
    return;
  }

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

  // If the GPS UART has been active too long without producing an eligible
  // saved fix, give the parser a rest and retry shortly.  The receiver supply
  // remains on; this timeout is especially useful indoors.
  if (cas12State != CAS12_PROBING &&
      elapsedMs(now, gpsAcquireStartedMs, GPS_ACQUIRE_TIMEOUT_MS)) {
    DBG_PRINTLN("GPS acquisition window timed out; sleeping before retry.");
    scheduleGpsSleep(GPS_RETRY_SLEEP_MS);
  }
}

// ---------------- MPU6050 direct-register driver ----------------

void mpuDataReadyISR() {
  // Keep the ISR bounded: all I2C traffic happens in loop().
  mpuIrqCount++;
  mpuDataReadyPending = true;
}

bool mpuWriteRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(MPU6050_ADDRESS);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool mpuReadRegister(uint8_t reg, uint8_t& value) {
  Wire.beginTransmission(MPU6050_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;

  if (Wire.requestFrom((int)MPU6050_ADDRESS, 1) != 1 || !Wire.available()) {
    return false;
  }
  value = (uint8_t)Wire.read();
  return true;
}

bool mpuReadAcceleration(int16_t& x, int16_t& y, int16_t& z) {
  Wire.beginTransmission(MPU6050_ADDRESS);
  Wire.write(MPU_REG_ACCEL_XOUT_H);
  if (Wire.endTransmission(false) != 0) return false;

  if (Wire.requestFrom((int)MPU6050_ADDRESS, 6) != 6) return false;
  if (Wire.available() < 6) return false;

  x = (int16_t)(((uint16_t)Wire.read() << 8) | (uint16_t)Wire.read());
  y = (int16_t)(((uint16_t)Wire.read() << 8) | (uint16_t)Wire.read());
  z = (int16_t)(((uint16_t)Wire.read() << 8) | (uint16_t)Wire.read());
  return true;
}

uint8_t smartMotionThresholdRegister() {
  uint16_t thresholdMg = smartSensitivityThresholdMg();
  // Round down for the candidate wake; verification enforces the full preset.
  // LPA codes: High=2, Balanced=5, Low=9. The former Balanced code of 80
  // represents 2560 mg under the documented LPA scale, not the intended 160.
  thresholdMg /= MPU_MOTION_THRESHOLD_MG_PER_LSB;
  if (thresholdMg == 0) thresholdMg = 1;
  if (thresholdMg > MPU_MOTION_THRESHOLD_REGISTER_MAX) {
    thresholdMg = MPU_MOTION_THRESHOLD_REGISTER_MAX;
  }
  return (uint8_t)thresholdMg;
}

bool mpuReadbackProfile(const uint8_t expected[][2], size_t count) {
  for (size_t i = 0; i < count; i++) {
    uint8_t actual = 0;
    if (!mpuReadRegister(expected[i][0], actual) || actual != expected[i][1]) {
      DBG_PRINTF("MPU6050 readback failed reg=0x%02X got=0x%02X expected=0x%02X\n",
                 expected[i][0], actual, expected[i][1]);
      return false;
    }
  }
  return true;
}

bool clearMpuInterruptPending() {
  // Clear the software hint before reading INT_STATUS.  If a new interrupt
  // arrives after this point, the ISR leaves the hint set for the next loop.
  noInterrupts();
  mpuDataReadyPending = false;
  interrupts();

  uint8_t ignoredStatus = 0;
  return mpuReadRegister(MPU_REG_INT_STATUS, ignoredStatus);
}

bool configureMpuArmed() {
  const uint8_t motionThreshold = smartMotionThresholdRegister();

  // Keep the device active while changing profiles, and disable the current
  // interrupt source before touching power-management or filter registers.
  if (!mpuWriteRegister(MPU_REG_PWR_MGMT_1, MPU_PWR_MGMT_1_VERIFY) ||
      !mpuWriteRegister(MPU_REG_INT_ENABLE, 0x00) ||
      !mpuWriteRegister(MPU_REG_PWR_MGMT_2, MPU_PWR_MGMT_2_VERIFY) ||
      !mpuWriteRegister(MPU_REG_CONFIG, MPU_CONFIG_ARMED) ||
      !mpuWriteRegister(MPU_REG_SMPLRT_DIV, 49) ||
      !mpuWriteRegister(MPU_REG_GYRO_CONFIG, 0x00) ||
      !mpuWriteRegister(MPU_REG_ACCEL_CONFIG, MPU_ACCEL_CONFIG_VERIFY) ||
      !mpuWriteRegister(MPU_REG_MOT_THR, motionThreshold) ||
      !mpuWriteRegister(MPU_REG_MOT_DUR, MPU_MOTION_DURATION) ||
      !mpuWriteRegister(MPU_REG_MOT_DETECT_CTRL, MPU_MOTION_DETECT_CTRL) ||
      !mpuWriteRegister(MPU_REG_INT_PIN_CFG,
                        MPU_INT_PIN_CFG_LATCHED_ACTIVE_HIGH)) {
    return false;
  }

  // ACCEL_HPF=7 holds the current acceleration as the motion detector's
  // reference, removing the static gravity vector before MOT_THR comparison.
  // Let the active accelerometer settle before capturing that reference.
  delay(100);
  if (!mpuWriteRegister(MPU_REG_ACCEL_CONFIG, MPU_ACCEL_CONFIG_ARMED) ||
      !mpuWriteRegister(MPU_REG_INT_ENABLE, MPU_INT_ENABLE_MOTION) ||
      // CYCLE starts the 5 Hz low-power accelerometer wake cycle.  TEMP_DIS
      // and PWR_MGMT_2 gyro standby keep the armed profile accel-only.
      !mpuWriteRegister(MPU_REG_PWR_MGMT_2, MPU_PWR_MGMT_2_ARMED) ||
      !mpuWriteRegister(MPU_REG_PWR_MGMT_1, MPU_PWR_MGMT_1_ARMED)) {
    return false;
  }

  const uint8_t expectedRegisters[][2] = {
    { MPU_REG_PWR_MGMT_1, MPU_PWR_MGMT_1_ARMED },
    { MPU_REG_PWR_MGMT_2, MPU_PWR_MGMT_2_ARMED },
    { MPU_REG_CONFIG, MPU_CONFIG_ARMED },
    { MPU_REG_SMPLRT_DIV, 49 },
    { MPU_REG_GYRO_CONFIG, 0x00 },
    { MPU_REG_ACCEL_CONFIG, MPU_ACCEL_CONFIG_ARMED },
    { MPU_REG_MOT_THR, motionThreshold },
    { MPU_REG_MOT_DUR, MPU_MOTION_DURATION },
    { MPU_REG_MOT_DETECT_CTRL, MPU_MOTION_DETECT_CTRL },
    { MPU_REG_INT_PIN_CFG, MPU_INT_PIN_CFG_LATCHED_ACTIVE_HIGH },
    { MPU_REG_INT_ENABLE, MPU_INT_ENABLE_MOTION }
  };
  if (!mpuReadbackProfile(expectedRegisters,
                          sizeof(expectedRegisters) / sizeof(expectedRegisters[0]))) {
    return false;
  }

  return clearMpuInterruptPending();
}

bool configureMpuVerification() {
  const uint8_t motionThreshold = smartMotionThresholdRegister();

  // Verification uses a normal active accelerometer path at 20 Hz.  Gyro and
  // temperature remain disabled; only the data-ready interrupt is enabled.
  if (!mpuWriteRegister(MPU_REG_PWR_MGMT_1, MPU_PWR_MGMT_1_VERIFY) ||
      !mpuWriteRegister(MPU_REG_INT_ENABLE, 0x00) ||
      !mpuWriteRegister(MPU_REG_PWR_MGMT_2, MPU_PWR_MGMT_2_VERIFY) ||
      !mpuWriteRegister(MPU_REG_CONFIG, 0x03) ||
      !mpuWriteRegister(MPU_REG_SMPLRT_DIV, 49) ||
      !mpuWriteRegister(MPU_REG_GYRO_CONFIG, 0x00) ||
      !mpuWriteRegister(MPU_REG_ACCEL_CONFIG, MPU_ACCEL_CONFIG_VERIFY) ||
      !mpuWriteRegister(MPU_REG_MOT_THR, motionThreshold) ||
      !mpuWriteRegister(MPU_REG_MOT_DUR, MPU_MOTION_DURATION) ||
      !mpuWriteRegister(MPU_REG_MOT_DETECT_CTRL, MPU_MOTION_DETECT_CTRL) ||
      !mpuWriteRegister(MPU_REG_INT_PIN_CFG,
                        MPU_INT_PIN_CFG_LATCHED_ACTIVE_HIGH) ||
      !mpuWriteRegister(MPU_REG_INT_ENABLE, MPU_INT_ENABLE_DATA_READY)) {
    return false;
  }

  const uint8_t expectedRegisters[][2] = {
    { MPU_REG_PWR_MGMT_1, MPU_PWR_MGMT_1_VERIFY },
    { MPU_REG_PWR_MGMT_2, MPU_PWR_MGMT_2_VERIFY },
    { MPU_REG_CONFIG, 0x03 },
    { MPU_REG_SMPLRT_DIV, 49 },
    { MPU_REG_GYRO_CONFIG, 0x00 },
    { MPU_REG_ACCEL_CONFIG, MPU_ACCEL_CONFIG_VERIFY },
    { MPU_REG_MOT_THR, motionThreshold },
    { MPU_REG_MOT_DUR, MPU_MOTION_DURATION },
    { MPU_REG_MOT_DETECT_CTRL, MPU_MOTION_DETECT_CTRL },
    { MPU_REG_INT_PIN_CFG, MPU_INT_PIN_CFG_LATCHED_ACTIVE_HIGH },
    { MPU_REG_INT_ENABLE, MPU_INT_ENABLE_DATA_READY }
  };
  if (!mpuReadbackProfile(expectedRegisters,
                          sizeof(expectedRegisters) / sizeof(expectedRegisters[0]))) {
    return false;
  }

  return clearMpuInterruptPending();
}

bool initMpu6050() {
  mpuAvailable = false;
  mpuInterruptAttached = false;
  mpuSampleInitialized = false;

  // Seeed's non-mbed nRF52 core maps the XIAO's Wire bus to D4/D5; setPins is
  // explicit here so a board variant cannot silently move the sensor bus.
  Wire.setPins(MPU_SDA_PIN, MPU_SCL_PIN);
  Wire.begin();
  Wire.setClock(400000UL);
  delay(5);

  uint8_t whoAmI = 0;
  if (!mpuReadRegister(MPU_REG_WHO_AM_I, whoAmI) || whoAmI != 0x68) {
    DBG_PRINTF("MPU6050 unavailable at 0x%02X (WHO_AM_I=0x%02X)\n",
               MPU6050_ADDRESS,
               whoAmI);
    return false;
  }

  // Reset, then install the low-power motion-interrupt profile.  Every
  // critical write is read back before the MPU is advertised as available; a
  // missing/bus-stuck device selects the documented runtime Interval fallback
  // without stopping the tracker.
  if (!mpuWriteRegister(MPU_REG_PWR_MGMT_1, 0x80)) return false;
  delay(100);
  if (!configureMpuArmed()) {
    DBG_PRINTLN("MPU6050 register initialization failed; Smart fallback enabled.");
    return false;
  }

  pinMode(MPU_INT_PIN, INPUT);
  const int interruptNumber = digitalPinToInterrupt(MPU_INT_PIN);
  if (interruptNumber < 0) {
    // Smart's contract requires the wake interrupt.  Running a polling-only
    // sensor here would claim a capability that the assembled board does not
    // have, so fail closed into the documented runtime Interval fallback.
    (void)mpuWriteRegister(MPU_REG_INT_ENABLE, 0x00);
    DBG_PRINTLN("MPU6050 D2 interrupt unavailable; Smart fallback enabled.");
    return false;
  }

  mpuDataReadyPending = false;
  // Seeed non-mbed core returns a channel mask, or zero on failure.
  if (attachInterrupt(interruptNumber, mpuDataReadyISR, RISING) == 0) {
    (void)mpuWriteRegister(MPU_REG_INT_ENABLE, 0x00);
    DBG_PRINTLN("MPU6050 D2 interrupt attachment failed; Smart fallback enabled.");
    return false;
  }
  mpuInterruptAttached = true;

  mpuAvailable = true;
  DBG_PRINTF("MPU6050 ready at 0x%02X, INT D2=%s\n",
             MPU6050_ADDRESS,
             mpuInterruptAttached ? "attached" : "unavailable");
  return true;
}

// ---------------- NMEA and optional CAS12 support ----------------

void observeNmeaByte(char value) {
  const uint32_t now = millis();
  nmeaByteCount++;
  lastNmeaByteMs = now;

  if (value == '$') {
    nmeaSeen = true;
    nmeaInSentence = true;
    nmeaHeaderLength = 0;
    return;
  }

  if (!nmeaInSentence) return;
  if (value == '\n' || value == '\r') {
    if (value == '\n') nmeaInSentence = false;
    return;
  }

  if (nmeaHeaderLength >= sizeof(nmeaHeader)) return;
  nmeaHeader[nmeaHeaderLength++] = (uint8_t)value;
  if (nmeaHeaderLength != 5) return;

  // Both GPxxx and GNxxx talker IDs are accepted.  Only the message type is
  // used for the probe; TinyGPSPlus remains the authority for validity.
  const bool isRmc = nmeaHeader[2] == 'R' && nmeaHeader[3] == 'M' &&
                     nmeaHeader[4] == 'C';
  const bool isGga = nmeaHeader[2] == 'G' && nmeaHeader[3] == 'G' &&
                     nmeaHeader[4] == 'A';
  if (isRmc) nmeaRmcCount++;
  if (isGga) nmeaGgaCount++;
}

uint8_t casicChecksum(const char* body) {
  uint8_t checksum = 0;
  while (*body) checksum ^= (uint8_t)*body++;
  return checksum;
}

bool sendCasCommandBody(const char* body) {
  if (!gpsPowered || gpsStandby || body == nullptr) return false;

  const uint8_t checksum = casicChecksum(body);
  bool wrote = true;
  // Keep writing the complete command even if one stream operation reports a
  // short write, then return the aggregate result to the probe/state machine.
  wrote = (Serial1.write('$') == 1) && wrote;
  wrote = (Serial1.print(body) == strlen(body)) && wrote;
  wrote = (Serial1.write('*') == 1) && wrote;
  char hex[3] = {};
  snprintf(hex, sizeof(hex), "%02X", checksum);
  if (checksum < 0x10) {
    // Avoid writing the first digit twice when checksum < 0x10.
    wrote = (Serial1.write('0') == 1) && wrote;
    wrote = (Serial1.write(hex[1]) == 1) && wrote;
  } else {
    wrote = (Serial1.print(hex) == 2) && wrote;
  }
  wrote = (Serial1.write('\r') == 1) && wrote;
  wrote = (Serial1.write('\n') == 1) && wrote;
  Serial1.flush();
  return wrote;
}

bool sendCas12Standby(uint16_t seconds) {
  char body[24] = {};
  snprintf(body, sizeof(body), "PCAS12,%u", (unsigned)seconds);
  return sendCasCommandBody(body);
}

bool requestGgaRmcWithoutNvWrite() {
  // CAS03 changes the receiver's live NMEA selection.  Deliberately do not
  // send CAS00 (the documented save-to-FLASH command), so a probe/configuration
  // can never overwrite receiver NVM.  Empty/disabled fields are intentional:
  // GGA and RMC at the receiver's current navigation rate are sufficient.
  return sendCasCommandBody("PCAS03,1,0,0,0,1,0,0,0");
}

enum Cas12ProbePhase : uint8_t {
  CAS12_PHASE_WAIT_QUIET = 1,
  CAS12_PHASE_WAIT_RESUME = 2
};

void finishCas12Probe(bool supported) {
  cas12State = supported ? CAS12_SUPPORTED : CAS12_UNSUPPORTED;
  cas12ProbePhase = 0;
  cas12ProbeCycleStartedMs = 0;
  cas12ProbeQuietObserved = false;

  if (supported && !cas12NmeaConfigured) {
    cas12NmeaConfigured = requestGgaRmcWithoutNvWrite();
  }

  DBG_PRINTF("CAS12 capability: %s after %u cycles\n",
             supported ? "supported" : "unsupported",
             (unsigned)cas12ProbeCycle);

  if (trackingMode == TRACKING_SMART && smartStartupProbePending) {
    smartStartupProbePending = false;
    if (smartState == SMART_ARMED && gpsPowered) gpsPowerOff();
  }
}

void startCas12ProbeIfReady() {
  // A probe is deliberately one-boot and one-shot.  It cannot run before the
  // receiver has emitted NMEA, and it cannot be repeated after a failed cycle.
  if (cas12ProbeAttempted || cas12State != CAS12_UNKNOWN || !gpsPowered ||
      !nmeaSeen || nmeaRmcCount == 0 || nmeaGgaCount == 0) return;

  cas12ProbeAttempted = true;
  cas12State = CAS12_PROBING;
  cas12ProbeCycle = 0;
  nmeaRmcCountAtProbe = nmeaRmcCount;
  nmeaGgaCountAtProbe = nmeaGgaCount;
  nmeaBytesAtProbe = nmeaByteCount;

  if (!sendCas12Standby(CAS12_PROBE_STANDBY_SECONDS)) {
    finishCas12Probe(false);
    return;
  }
  // Start the five-second minimum from the completed command write.  Sentence
  // counters are re-baselined again when silence is first observed, so late
  // pre-standby bytes cannot masquerade as resumed output.
  cas12ProbeCycleStartedMs = millis();
  cas12ProbeQuietObserved = false;
  cas12ProbePhase = CAS12_PHASE_WAIT_QUIET;
  DBG_PRINTLN("CAS12 probe started; requiring 5 s standby, silence, and resumption.");
}

void serviceCas12Probe() {
  const uint32_t now = millis();

  if (trackingMode == TRACKING_SMART && smartStartupProbePending &&
      !cas12ProbeAttempted && cas12State == CAS12_UNKNOWN &&
      cas12StartupSinceMs != 0 &&
      elapsedMs(now, cas12StartupSinceMs, CAS12_NMEA_WAIT_TIMEOUT_MS)) {
    // No NMEA means there is no safe evidence to probe.  End the startup
    // window and leave capability UNKNOWN; a later Smart acquisition still
    // uses the non-CAS12 path and remains correct.
    cas12ProbeAttempted = true;
    smartStartupProbePending = false;
    if (gpsPowered) gpsPowerOff();
    DBG_PRINTLN("CAS12 probe skipped: no NMEA observed during startup window.");
  }

  if (cas12State == CAS12_UNKNOWN) {
    startCas12ProbeIfReady();
    return;
  }
  if (cas12State != CAS12_PROBING || !gpsPowered || gpsStandby) return;

  if (cas12ProbePhase == CAS12_PHASE_WAIT_QUIET) {
    const bool nmeaQuiet =
        lastNmeaByteMs != 0 &&
        elapsedMs(now, lastNmeaByteMs, CAS12_PROBE_QUIET_GAP_MS);
    if (!cas12ProbeQuietObserved && nmeaQuiet) {
      // Latch the first observed quiet gap.  A supported receiver may resume
      // exactly at the requested five-second boundary; requiring the line to
      // still be quiet when the minimum elapses would miss those first bytes.
      // Counters after this boundary are the resumed half of this cycle.
      cas12ProbeQuietObserved = true;
      nmeaRmcCountAtProbe = nmeaRmcCount;
      nmeaGgaCountAtProbe = nmeaGgaCount;
      nmeaBytesAtProbe = nmeaByteCount;
    }

    if (cas12ProbeQuietObserved) {
      const bool resumed = nmeaByteCount > nmeaBytesAtProbe;
      const bool minimumElapsed =
          elapsedMs(now, cas12ProbeCycleStartedMs, CAS12_PROBE_MIN_ELAPSED_MS);
      if (resumed && !minimumElapsed) {
        // NMEA resumed before the requested standby duration; do not accept a
        // short pause as proof of the five-second receiver command.
        finishCas12Probe(false);
        return;
      }
      if (minimumElapsed) {
        cas12ProbePhase = CAS12_PHASE_WAIT_RESUME;
        return;
      }
    }

    if (elapsedMs(now, cas12ProbeCycleStartedMs, CAS12_PROBE_CYCLE_TIMEOUT_MS)) {
      finishCas12Probe(false);
    }
    return;
  }

  if (cas12ProbePhase == CAS12_PHASE_WAIT_RESUME) {
    const bool freshNmea = nmeaByteCount > nmeaBytesAtProbe;
    const bool freshRmc = nmeaRmcCount > nmeaRmcCountAtProbe;
    const bool freshGga = nmeaGgaCount > nmeaGgaCountAtProbe;
    const bool validNavigation = gps.location.isValid() && gps.date.isValid() &&
                                 gps.time.isValid() &&
                                 gps.location.age() <= CAS12_PROBE_RESUME_TIMEOUT_MS;
    const bool standbyElapsed =
        elapsedMs(now, cas12ProbeCycleStartedMs, CAS12_PROBE_MIN_ELAPSED_MS);
    if (standbyElapsed && freshNmea && freshRmc && freshGga && validNavigation) {
      cas12ProbeCycle++;
      if (cas12ProbeCycle >= 2) {
        finishCas12Probe(true);
        return;
      }

      // The first cycle passed.  Require another complete silence/resumption
      // cycle with fresh sentence counters before declaring support.
      nmeaRmcCountAtProbe = nmeaRmcCount;
      nmeaGgaCountAtProbe = nmeaGgaCount;
      nmeaBytesAtProbe = nmeaByteCount;
      if (!sendCas12Standby(CAS12_PROBE_STANDBY_SECONDS)) {
        finishCas12Probe(false);
        return;
      }
      cas12ProbeCycleStartedMs = millis();
      cas12ProbeQuietObserved = false;
      cas12ProbePhase = CAS12_PHASE_WAIT_QUIET;
      return;
    }

    if (elapsedMs(now, cas12ProbeCycleStartedMs, CAS12_PROBE_CYCLE_TIMEOUT_MS)) {
      finishCas12Probe(false);
    }
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
  if (record.magic != METADATA_MAGIC ||
      (record.version != METADATA_VERSION_V1 &&
       record.version != METADATA_VERSION_V2)) return false;
  if (!isAllowedLogInterval(record.logIntervalSeconds)) return false;
  if (record.version == METADATA_VERSION_V2 &&
      (record.reserved[METADATA_RESERVED_MODE] > TRACKING_SMART ||
       record.reserved[METADATA_RESERVED_SENSITIVITY] > SMART_MAX_SENSITIVITY)) {
    return false;
  }
  const uint8_t expected =
      crc8(reinterpret_cast<const uint8_t*>(&record), sizeof(MetadataRecord) - 1);
  return expected == record.crc8;
}

void initDefaultMetadata() {
  memset(&metadataRecord, 0, sizeof(metadataRecord));
  metadataRecord.magic = METADATA_MAGIC;
  metadataRecord.version = METADATA_VERSION_V2;
  metadataRecord.ownerSet = 0;
  metadataRecord.logIntervalSeconds = DEFAULT_LOG_INTERVAL_SECONDS;
  metadataRecord.reserved[METADATA_RESERVED_MODE] = TRACKING_INTERVAL;
  metadataRecord.reserved[METADATA_RESERVED_SENSITIVITY] = SMART_DEFAULT_SENSITIVITY;
  metadataRecord.crc8 =
      crc8(reinterpret_cast<const uint8_t*>(&metadataRecord),
           sizeof(MetadataRecord) - 1);
  ownerSet = false;
  logIntervalSeconds = DEFAULT_LOG_INTERVAL_SECONDS;
  trackingMode = TRACKING_INTERVAL;
  smartSensitivity = SMART_DEFAULT_SENSITIVITY;
}

bool persistMetadata() {
  flashWake();

  metadataRecord.magic = METADATA_MAGIC;
  metadataRecord.version = METADATA_VERSION_V2;
  metadataRecord.ownerSet = ownerSet ? 1 : 0;
  metadataRecord.logIntervalSeconds = logIntervalSeconds;
  metadataRecord.reserved[METADATA_RESERVED_MODE] = (uint8_t)trackingMode;
  metadataRecord.reserved[METADATA_RESERVED_SENSITIVITY] = smartSensitivity;
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

    if (current.version == METADATA_VERSION_V1) {
      // v1.5 had no mode field.  Its behavior was Interval, so migrate that
      // behavior while consuming only the first two reserved bytes for v2.
      trackingMode = TRACKING_INTERVAL;
      metadataRecord.reserved[METADATA_RESERVED_MODE] = TRACKING_INTERVAL;
      metadataRecord.reserved[METADATA_RESERVED_SENSITIVITY] =
          SMART_DEFAULT_SENSITIVITY;
      smartSensitivity = SMART_DEFAULT_SENSITIVITY;
      DBG_PRINTLN("Migrating v1 metadata to v2 (Interval mode preserved).");
      flashSleep();
      return persistMetadata();
    }

    trackingMode = current.reserved[METADATA_RESERVED_MODE] == TRACKING_SMART
        ? TRACKING_SMART : TRACKING_INTERVAL;
    smartSensitivity = current.reserved[METADATA_RESERVED_SENSITIVITY];
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
    DBG_PRINTLN("Migrating v1.0 owner metadata to v2.");
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

void resetSmartClassifierRuntime() {
  smartMotionDetected = false;
  smartRetryAfterCooldown = false;
  smartFirstFixPending = false;
  smartCooldownObservationOpen = false;
  smartCooldownUntilMs = 0;
  smartVerifyStartedMs = 0;
  smartVerifyPeakMg = 0;
  smartVerifySamples = 0;
  smartVerifyActiveSamples = 0;
  smartVerifyLastActiveMs = 0;
  mpuQuietSinceMs = 0;
  mpuLastSampleMs = 0;
  smartMotionReferenceValid = false;
  smartMotionReferenceLat = 0.0;
  smartMotionReferenceLon = 0.0;
  smartMotionReferenceEpoch = 0;
}

void cancelCas12ProbeForModeChange() {
  if (gpsStandbyPending) gpsPowerOff();
  if (cas12State != CAS12_PROBING) return;

  // A mode change may close the UART while a probe is in flight.  Do not leave
  // the capability stuck in PROBING, and do not retry it repeatedly this boot.
  cas12State = CAS12_UNSUPPORTED;
  cas12ProbePhase = 0;
  DBG_PRINTLN("CAS12 probe cancelled by tracking-mode change.");
}

void disableSmartMpuRuntime();

void applyTrackingRuntime() {
  const uint32_t now = millis();
  resetSmartClassifierRuntime();
  smartLastMotionMs = now;
  runtimeIntervalFallback = false;

  if (trackingMode != TRACKING_SMART) {
    lastWakeReason = SMART_WAKE_NONE;
    smartState = SMART_DISABLED;
    smartStateSinceMs = now;
    smartStartupProbePending = false;
    cas12StartupSinceMs = 0;
    if (gpsStandby || gpsStandbyPending) gpsPowerOff();
    gpsNextWakeMs = now;
    gpsAcquireStartedMs = now;
    return;
  }

  if (!mpuAvailable || !mpuInterruptAttached) {
    // Keep the user's Smart selection in metadata, but run the proven
    // Interval scheduler for this boot.  A missing motion interrupt must not
    // silently turn Smart into an always-on GNSS logger or claim Smart motion
    // evidence that was never available.
    runtimeIntervalFallback = true;
    lastWakeReason = SMART_WAKE_NONE;
    smartState = SMART_DISABLED;
    smartStateSinceMs = now;
    smartStartupProbePending = false;
    cas12StartupSinceMs = 0;
    if (gpsStandby || gpsStandbyPending) gpsPowerOff();
    gpsNextWakeMs = now;
    gpsAcquireStartedMs = now;
    DBG_PRINTLN("Smart selection retained; this boot uses runtime Interval fallback.");
    return;
  }

  if (!configureMpuArmed()) {
    // A profile/readback failure is a runtime capability loss, not a reason to
    // erase the user's Smart selection.  The fallback helper detaches INT and
    // routes this boot through the existing Interval scheduler.
    disableSmartMpuRuntime();
    return;
  }

  gpsNextWakeMs = 0; // Retire the old Interval deadline; Smart owns its timers.
  smartState = SMART_ARMED;
  smartStateSinceMs = now;
  smartStartupProbePending = !cas12ProbeAttempted && cas12State == CAS12_UNKNOWN;
  cas12StartupSinceMs = smartStartupProbePending ? now : 0;
  if (gpsStandby || gpsStandbyPending) {
    // A runtime reset (including a sensitivity-only update) invalidates any
    // receiver-side standby bookkeeping.  Reopen/close through the UART path
    // rather than leaving an ARMED state stuck behind an old standby timer.
    gpsPowerOff();
  } else if (gpsPowered && !gpsStandby) {
    gpsPowerOff();
  }
}

bool setSmartConfig(TrackingMode requestedMode, uint8_t requestedSensitivity) {
  if (requestedMode != TRACKING_INTERVAL && requestedMode != TRACKING_SMART) {
    return false;
  }
  if (requestedSensitivity > SMART_MAX_SENSITIVITY) return false;

  // The mode and sensitivity are one v2 configuration transaction.  Do not
  // persist one field and then discover that the other field was invalid.
  const TrackingMode previousMode = trackingMode;
  const uint8_t previousSensitivity = smartSensitivity;
  const uint8_t previousModeByte = metadataRecord.reserved[METADATA_RESERVED_MODE];
  const uint8_t previousSensitivityByte =
      metadataRecord.reserved[METADATA_RESERVED_SENSITIVITY];

  trackingMode = requestedMode;
  smartSensitivity = requestedSensitivity;
  if (!persistMetadata()) {
    trackingMode = previousMode;
    smartSensitivity = previousSensitivity;
    metadataRecord.reserved[METADATA_RESERVED_MODE] = previousModeByte;
    metadataRecord.reserved[METADATA_RESERVED_SENSITIVITY] = previousSensitivityByte;
    DBG_PRINTLN("Smart configuration save FAILED; previous values restored in RAM.");
    return false;
  }

  // A sensitivity-only update still resets the Smart runtime.  Cancel an
  // in-flight one-boot probe as well, otherwise applyTrackingRuntime() would
  // close its UART while leaving CAS12 in PROBING forever.
  if (previousMode != requestedMode || cas12State == CAS12_PROBING) {
    cancelCas12ProbeForModeChange();
  }
  applyTrackingRuntime();
  DBG_PRINTF("Tracking mode changed to %s, sensitivity=%u.\n",
             trackingModeLabel(trackingMode),
             (unsigned)smartSensitivity);
  return true;
}

// Kept for internal/source compatibility with the v1 mode abstraction.  The
// wire-level v2 setter always calls setSmartConfig() with both bytes.
bool setTrackingMode(TrackingMode requestedMode) {
  return setSmartConfig(requestedMode, smartSensitivity);
}

bool setLogInterval(uint32_t seconds) {
  if (!isAllowedLogInterval(seconds)) return false;

  // Do not leave RAM and flash disagreeing if the QSPI update fails.
  const uint32_t previousInterval = logIntervalSeconds;
  const TrackingMode previousMode = trackingMode;

  logIntervalSeconds = seconds;
  metadataRecord.logIntervalSeconds = seconds;
  // The legacy interval command is also the explicit way back to Interval
  // mode.  Persist both values atomically in the v2 metadata record.
  trackingMode = TRACKING_INTERVAL;

  if (!persistMetadata()) {
    logIntervalSeconds = previousInterval;
    metadataRecord.logIntervalSeconds = previousInterval;
    trackingMode = previousMode;
    DBG_PRINTLN("GPS interval save FAILED; previous interval restored in RAM.");
    return false;
  }

  if (previousMode != TRACKING_INTERVAL) cancelCas12ProbeForModeChange();
  runtimeIntervalFallback = false;
  lastWakeReason = SMART_WAKE_NONE;

  DBG_PRINTF("GPS interval changed to %s (%lus).\n",
             intervalLabel(seconds),
             (unsigned long)seconds);

  // Re-evaluate the schedule using a fresh GPS UTC fix. If the receiver is
  // sleeping, wake it now; serviceGps() will either save if due or put it back
  // to sleep until the appropriate pre-wake window.
  smartState = SMART_DISABLED;
  smartStateSinceMs = millis();
  smartStartupProbePending = false;
  cas12StartupSinceMs = 0;
  resetSmartClassifierRuntime();
  if (gpsStandby || gpsStandbyPending) {
    // Closing the UART is the portable wake/cancel path for a finite standby;
    // the receiver supply remains on throughout.
    gpsPowerOff();
  }
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
    if (elapsedMs(millis(), started, OWNER_RESET_HOLD_MS)) {
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
  smartMotionReferenceValid = false;
  smartMotionReferenceLat = 0.0;
  smartMotionReferenceLon = 0.0;
  smartMotionReferenceEpoch = 0;
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

bool consumeGpsLocationFreshness() {
  // TinyGPS++'s isUpdated() query is intentionally paired with the official
  // non-const lat()/lng() accessors here: those accessors clear the library's
  // latched freshness bit.  A cached valid location therefore cannot satisfy
  // a later acquisition after this edge has been consumed.
  if (!gps.location.isUpdated()) return false;
  (void)gps.location.lat();
  (void)gps.location.lng();
  return true;
}

bool consumeGpsUtcFreshness() {
  // Date and time have the same latched-update behavior as location.  Read a
  // representative field from each updated object to clear its freshness bit;
  // the values themselves remain available for gpsEpochUtc().
  const bool dateUpdated = gps.date.isUpdated();
  const bool timeUpdated = gps.time.isUpdated();
  if (dateUpdated) (void)gps.date.year();
  if (timeUpdated) (void)gps.time.hour();
  return dateUpdated || timeUpdated;
}

void discardPendingGpsInputAtAcquisitionStart() {
  // serviceMpu() runs before serviceGps() in loop().  Drain bytes that were
  // already buffered when the motion candidate woke us, so a pre-start RMC or
  // GGA cannot cross the acquisition baseline.  Reset the local NMEA header
  // parser as well; TinyGPS++ will resynchronise on the next '$'.
  while (gpsPowered && Serial1.available() > 0) {
    (void)Serial1.read();
  }
  nmeaHeaderLength = 0;
  nmeaInSentence = false;
  memset(nmeaHeader, 0, sizeof(nmeaHeader));
  (void)consumeGpsLocationFreshness();
  (void)consumeGpsUtcFreshness();
  // TinyGPS++ does not expose a parser-reset API.  serviceGps() therefore
  // drops post-start continuation bytes until the next '$', which forces its
  // internal parser to begin at a complete sentence boundary.
  smartAcquisitionNmeaSynchronized = false;
}

// ---------------- Smart Motion state machine ----------------

void setSmartState(SmartState nextState) {
  if (smartState == nextState) return;
  smartState = nextState;
  smartStateSinceMs = millis();
  DBG_PRINTF("Smart state -> %s\n", smartStateLabel(smartState));
}

void startSmartAcquisition() {
  if (!smartRuntimeActive()) return;

  const bool retryWake = smartRetryAfterCooldown;
  smartRetryAfterCooldown = false;
  smartFirstFixPending = true;
  smartMotionDetected = true;
  mpuQuietSinceMs = 0;
  lastWakeReason = retryWake ? SMART_WAKE_RETRY : SMART_WAKE_MOTION;
  setSmartState(SMART_ACQUIRING);
  if (!gpsPowered && !gpsStandby) gpsPowerOn();
  // Establish both baselines only after opening the UART and discarding any
  // bytes/freshness left from before this candidate.  Acquisition must later
  // observe a strictly newer TinyGPS++ valid-fix sentence and location commit.
  discardPendingGpsInputAtAcquisitionStart();
  smartAcquisitionStartedMs = millis();
  smartAcquisitionFixSentenceBaseline = gps.sentencesWithFix();
  smartAcquisitionLocationBaseline = gpsLocationUpdateCounter;
  smartAcquisitionUtcBaseline = gpsUtcUpdateCounter;
  DBG_PRINTLN("Smart motion verified; GNSS acquisition started (90 s window).");
}

void enterSmartCooldown(bool retryAfterCooldown) {
  if (!smartRuntimeActive()) return;

  const uint32_t now = millis();
  smartRetryAfterCooldown = retryAfterCooldown;
  smartCooldownUntilMs = now + SMART_COOLDOWN_MS;
  smartMotionDetected = false;
  smartCooldownObservationOpen = false;
  setSmartState(SMART_COOLDOWN);

  if (gpsPowered && !gpsStandby) {
    // CAS12 is only an optional receiver-side optimization.  If the two-cycle
    // probe did not pass, close the UART and use the same state machine; Smart
    // correctness never depends on CAS12.
    if (cas12State == CAS12_SUPPORTED &&
        sendCas12Standby(SMART_STANDBY_SLICE_SECONDS)) {
      // Do not claim receiver standby from a command write alone.  CASIC does
      // not require an acknowledgement; serviceGpsStandbyState() promotes
      // this request only after the expected NMEA quiet gap is observed.
      gpsStandby = false;
      gpsStandbyPending = true;
      gpsStandbyRequestedMs = now;
      DBG_PRINTF("Smart cooldown: receiver standby requested via CAS12 (%us slice); awaiting silence.\n",
                 (unsigned)SMART_STANDBY_SLICE_SECONDS);
    } else {
      gpsPowerOff();
    }
  }
}

bool smartGnssFixFreshAndConfident(uint32_t now) {
  (void)now;
  if (!gps.location.isValid() || !gps.date.isValid() || !gps.time.isValid()) {
    return false;
  }
  if (gps.location.age() > SMART_GNSS_STALE_MS) return false;
  if (!gps.satellites.isValid() ||
      gps.satellites.value() < SMART_GNSS_MIN_SATELLITES) {
    return false;
  }
  if (!gps.hdop.isValid() || gps.hdop.value() > SMART_GNSS_MAX_HDOP_X100) {
    return false;
  }
  return true;
}

void updateSmartMotionReferenceFromGps(uint32_t now) {
  if (!smartGnssFixFreshAndConfident(now)) return;

  const uint32_t epoch = gpsEpochUtc();
  if (epoch == 0) return;

  smartMotionReferenceLat = gps.location.lat();
  smartMotionReferenceLon = gps.location.lng();
  smartMotionReferenceEpoch = epoch;
  smartMotionReferenceValid = true;
}

void updateSmartMotionReferenceFromRecord(const GpsRecord& record) {
  if (record.epoch == 0) return;

  smartMotionReferenceLat = (double)record.latE7 / 10000000.0;
  smartMotionReferenceLon = (double)record.lonE7 / 10000000.0;
  smartMotionReferenceEpoch = record.epoch;
  smartMotionReferenceValid = true;
}

bool gnssContinuedMotion(uint32_t now) {
  if (!smartGnssFixFreshAndConfident(now)) return false;

  // TinyGPSPlus exposes speed in km/h across the supported versions.  Keep
  // the classifier threshold in cm/s to make the documented 1 m/s rule clear.
  bool speedEvidence = false;
  if (gps.speed.isValid()) {
    const float speedCmPerSecond = (float)gps.speed.kmph() * 27.777778f;
    speedEvidence = speedCmPerSecond >= SMART_GNSS_CONTINUED_SPEED_CM_S;
  }

  bool displacementEvidence = false;
  const uint32_t currentEpoch = gpsEpochUtc();
  if (smartMotionReferenceValid && currentEpoch != 0 &&
      currentEpoch != smartMotionReferenceEpoch) {
    // TinyGPSPlus uses the same spherical-earth distance calculation as its
    // distanceBetween helper.  Requiring a fresh confident fix above avoids
    // promoting stale/no-fix coordinates or poor-HDOP noise to motion.
    const double displacementMeters = TinyGPSPlus::distanceBetween(
        smartMotionReferenceLat,
        smartMotionReferenceLon,
        gps.location.lat(),
        gps.location.lng());
    displacementEvidence =
        displacementMeters >= SMART_GNSS_CONTINUED_DISPLACEMENT_METERS;
  }

  if (!speedEvidence && !displacementEvidence) return false;

  smartLastMotionMs = now;
  smartMotionDetected = true;
  mpuQuietSinceMs = 0;
  return true;
}

bool smartMotionRecent(uint32_t now) {
  bool recent = false;
  if (mpuAvailable && mpuLastMotionMs != 0 &&
      !elapsedMs(now, mpuLastMotionMs, SMART_MOTION_RECENCY_MS)) {
    recent = true;
  }
  if (gnssContinuedMotion(now)) recent = true;
  if (recent) {
    smartLastMotionMs = now;
    smartMotionDetected = true;
    return true;
  }
  return false;
}

void disableSmartMpuRuntime() {
  const uint32_t now = millis();
  if (mpuInterruptAttached) {
    const int interruptNumber = digitalPinToInterrupt(MPU_INT_PIN);
    if (interruptNumber >= 0) detachInterrupt(interruptNumber);
  }
  mpuAvailable = false;
  mpuInterruptAttached = false;
  runtimeIntervalFallback = true;
  smartState = SMART_DISABLED;
  smartStateSinceMs = now;
  smartStartupProbePending = false;
  cas12StartupSinceMs = 0;
  if (cas12State == CAS12_PROBING) {
    cas12State = CAS12_UNSUPPORTED;
    cas12ProbePhase = 0;
  }
  if (gpsPowered || gpsStandby || gpsStandbyPending) gpsPowerOff();
  gpsNextWakeMs = now;
  gpsAcquireStartedMs = now;
  DBG_PRINTLN("MPU6050 runtime failure; using Interval fallback this boot.");
}

bool rejectMpuVerification(const char* reason) {
  DBG_PRINTF("Smart verification rejected: %s samples=%u active=%u peakMg=%u\n",
             reason, smartVerifySamples, smartVerifyActiveSamples, smartVerifyPeakMg);
  smartMotionDetected = false;
  smartVerifySamples = 0;
  smartVerifyActiveSamples = 0;
  smartVerifyLastActiveMs = 0;
  if (!configureMpuArmed()) {
    disableSmartMpuRuntime();
    return false;
  }
  mpuSampleInitialized = false;
  mpuLastSampleMs = 0;
  setSmartState(SMART_ARMED);
  return true;
}

void serviceMpu() {
  if (!mpuAvailable || !mpuInterruptAttached || !smartRuntimeActive()) return;

  const uint32_t now = millis();
  // The watchdog and quiet-gap rule must still terminate a verification if a
  // data-ready interrupt is lost entirely.  Keep this independent of the
  // pending-IRQ path; otherwise a 60-99 sample candidate could remain stuck
  // in VERIFYING forever instead of failing closed back to ARMED.
  if (smartState == SMART_VERIFYING) {
    const bool verificationWatchdogExpired =
        elapsedMs(now, smartVerifyStartedMs, SMART_VERIFY_WATCHDOG_MS) &&
        smartVerifySamples < SMART_VERIFY_TOTAL_SAMPLES;
    const bool quietGapExceeded =
        smartVerifyLastActiveMs == 0 ||
        elapsedMs(now, smartVerifyLastActiveMs, SMART_VERIFY_MAX_QUIET_GAP_MS);
    if (verificationWatchdogExpired || quietGapExceeded) {
      rejectMpuVerification(verificationWatchdogExpired ? "sample timeout" : "quiet gap");
      return;
    }
  }

  bool interruptPending = false;
  noInterrupts();
  interruptPending = mpuDataReadyPending;
  mpuDataReadyPending = false;
  interrupts();

  // Recover an already-latched HIGH even if its rising edge was missed during
  // profile changes/IRQ attachment. Without this, no new rising edge can
  // arrive until INT_STATUS is read, leaving ARMED stuck indefinitely.
  // This checks a GPIO only; there is no periodic I2C/acceleration polling.
  if (!interruptPending && digitalRead(MPU_INT_PIN) != HIGH) return;

  uint8_t interruptStatus = 0;
  if (!mpuReadRegister(MPU_REG_INT_STATUS, interruptStatus)) {
    disableSmartMpuRuntime();
    return;
  }
  mpuLastInterruptStatus = interruptStatus;
  if ((interruptStatus & MPU_INT_STATUS_MOTION) != 0) mpuMotionEventCount++;

  if (smartState == SMART_ARMED || smartState == SMART_TRACKING) {
    if ((interruptStatus & MPU_INT_STATUS_MOTION) == 0) return;

    mpuLastMotionMs = now;
    smartLastMotionMs = now;
    smartMotionDetected = false;
    mpuQuietSinceMs = 0;
    if (!configureMpuVerification()) {
      disableSmartMpuRuntime();
      return;
    }

    // The hardware event is only the candidate.  Verification starts with a
    // fresh gravity estimate and exactly 100 subsequent 20 Hz samples.
    mpuSampleInitialized = false;
    mpuDynamicMagnitudeMg = 0;
    mpuLastSampleMs = 0;
    // Start the confirmation window after the verification profile has been
    // installed/read back, not before the I2C setup latency.
    smartVerifyStartedMs = millis();
    smartVerifyPeakMg = 0;
    smartVerifySamples = 0;
    smartVerifyActiveSamples = 0;
    smartVerifyLastActiveMs = smartVerifyStartedMs;
    setSmartState(SMART_VERIFYING);
    return;
  }

  if (smartState != SMART_VERIFYING) {
    // Motion interrupts remain enabled in the armed profile during GNSS
    // acquisition and cooldown so the continued-motion classifier can use a
    // recent hardware event without starting a second verification.
    if ((interruptStatus & MPU_INT_STATUS_MOTION) != 0) {
      mpuLastMotionMs = now;
      smartLastMotionMs = now;
      smartMotionDetected = true;
      mpuQuietSinceMs = 0;
    }
    return;
  }

  // Verification is driven by the MPU data-ready source configured at 20 Hz;
  // stale/spurious motion-only status does not cause an acceleration read.
  // DATA_READY is the hardware sample clock. A second 50 ms software gate
  // discards real samples when loop servicing jitter makes two reads 49 ms
  // apart, causing the 100-sample watchdog to reject continuous movement.
  if ((interruptStatus & MPU_INT_STATUS_DATA_READY) == 0) {
    return;
  }

  // Reject only after the deterministic watchdog, not at the nominal 5 s
  // boundary.  The sample count defines the confirmation window, so a normal
  // 100th interrupt that arrives just after 5000 ms remains eligible.
  if (smartVerifySamples < SMART_VERIFY_TOTAL_SAMPLES &&
      elapsedMs(now, smartVerifyStartedMs, SMART_VERIFY_WATCHDOG_MS)) {
    rejectMpuVerification("sample timeout");
    return;
  }

  int16_t rawX = 0;
  int16_t rawY = 0;
  int16_t rawZ = 0;
  if (!mpuReadAcceleration(rawX, rawY, rawZ)) {
    disableSmartMpuRuntime();
    return;
  }
  mpuLastSampleMs = now;

  // ±2 g is 16,384 LSB/g.  The low-pass estimate tracks gravity while the
  // residual vector represents movement/vibration.
  const float accelX = (float)rawX / 16384.0f;
  const float accelY = (float)rawY / 16384.0f;
  const float accelZ = (float)rawZ / 16384.0f;
  static const float SMART_GRAVITY_LPF_ALPHA = 0.10f;

  if (!mpuSampleInitialized) {
    mpuGravityX = accelX;
    mpuGravityY = accelY;
    mpuGravityZ = accelZ;
    mpuSampleInitialized = true;
  } else {
    mpuGravityX += SMART_GRAVITY_LPF_ALPHA * (accelX - mpuGravityX);
    mpuGravityY += SMART_GRAVITY_LPF_ALPHA * (accelY - mpuGravityY);
    mpuGravityZ += SMART_GRAVITY_LPF_ALPHA * (accelZ - mpuGravityZ);
  }

  const float linearX = accelX - mpuGravityX;
  const float linearY = accelY - mpuGravityY;
  const float linearZ = accelZ - mpuGravityZ;
  const float magnitudeMg = sqrtf(linearX * linearX + linearY * linearY +
                                  linearZ * linearZ) * 1000.0f;
  mpuDynamicMagnitudeMg = magnitudeMg >= 65535.0f
      ? 65535 : (uint16_t)magnitudeMg;

  const bool activeEvidence =
      mpuDynamicMagnitudeMg >= smartSensitivityThresholdMg();

  if (activeEvidence) {
    mpuLastMotionMs = now;
    smartLastMotionMs = now;
    mpuQuietSinceMs = 0;
  } else if (mpuQuietSinceMs == 0) {
    mpuQuietSinceMs = now;
  }

  if (mpuDynamicMagnitudeMg > smartVerifyPeakMg) {
    smartVerifyPeakMg = mpuDynamicMagnitudeMg;
  }
  if (smartVerifySamples < SMART_VERIFY_TOTAL_SAMPLES) {
    smartVerifySamples++;
  }
  if (activeEvidence) {
    if (smartVerifyActiveSamples < SMART_VERIFY_TOTAL_SAMPLES) {
      smartVerifyActiveSamples++;
    }
    smartVerifyLastActiveMs = now;
    smartMotionDetected = true;
  }

  // A quiet gap cancels a false candidate before it can acquire GNSS.  The
  // verifier accepts only after all 100 samples (5 s at 20 Hz), with at least
  // 60 active samples and no gap longer than one second.  If the watchdog
  // expires while fewer than 100 samples arrived, fail safely rather than
  // accepting a partial window (for example, 60-99 active samples).
  const bool quietGapExceeded =
      smartVerifyLastActiveMs == 0 ||
      elapsedMs(now, smartVerifyLastActiveMs, SMART_VERIFY_MAX_QUIET_GAP_MS);
  const bool verificationWatchdogExpiredBeforeAllSamples =
      elapsedMs(now, smartVerifyStartedMs, SMART_VERIFY_WATCHDOG_MS) &&
      smartVerifySamples < SMART_VERIFY_TOTAL_SAMPLES;
  if (quietGapExceeded || verificationWatchdogExpiredBeforeAllSamples) {
    rejectMpuVerification(verificationWatchdogExpiredBeforeAllSamples ? "sample timeout" : "quiet gap");
    return;
  }

  const bool verificationComplete =
      smartVerifySamples >= SMART_VERIFY_TOTAL_SAMPLES;
  if (verificationComplete) {
    const bool accepted =
        smartVerifyActiveSamples >= SMART_VERIFY_REQUIRED_ACTIVE_SAMPLES;
    DBG_PRINTF("Smart verification %s: samples=%u active=%u peakMg=%u\n",
               accepted ? "accepted" : "rejected", smartVerifySamples,
               smartVerifyActiveSamples, smartVerifyPeakMg);
    smartVerifySamples = 0;
    smartVerifyActiveSamples = 0;
    smartVerifyLastActiveMs = 0;
    if (!configureMpuArmed()) {
      disableSmartMpuRuntime();
      return;
    }
    mpuSampleInitialized = false;
    mpuLastSampleMs = 0;
    if (accepted) {
      smartMotionDetected = true;
      startSmartAcquisition();
    } else {
      smartMotionDetected = false;
      setSmartState(SMART_ARMED);
    }
  }
}

void serviceSmartState() {
  if (!smartRuntimeActive()) return;

  const uint32_t now = millis();
  switch (smartState) {
    case SMART_DISABLED:
      smartState = SMART_ARMED;
      smartStateSinceMs = now;
      break;

    case SMART_ARMED:
      // The MPU interrupt/sample verifier is the only Smart wake trigger.
      // When it is unavailable, applyTrackingRuntime() keeps the saved Smart
      // mode but routes this boot through Interval behavior instead.
      break;

    case SMART_VERIFYING:
      // The 20 Hz verifier runs in serviceMpu().
      break;

    case SMART_ACQUIRING:
      if (elapsedMs(now, smartAcquisitionStartedMs, SMART_ACQUISITION_TIMEOUT_MS)) {
        DBG_PRINTLN("Smart GNSS acquisition timed out; entering 120 s cooldown.");
        enterSmartCooldown(true);
      }
      break;

    case SMART_TRACKING:
      // A valid, fresh GNSS speed above the continued-motion threshold can
      // start the next event-driven acquisition after cooldown.  This covers
      // smooth travel where dynamic acceleration is quiet; MPU motion still
      // uses the stricter 20 Hz verifier in serviceMpu().
      if (gnssContinuedMotion(now)) {
        startSmartAcquisition();
        break;
      }
      if (smartMotionRecent(now)) break;
      if (smartLastMotionMs != 0 &&
          elapsedMs(now, smartLastMotionMs, SMART_QUIET_GAP_MS)) {
        DBG_PRINTLN("Smart continued-motion classifier quiet; returning to armed.");
        smartMotionDetected = false;
        setSmartState(SMART_ARMED);
        if (gpsPowered && !gpsStandby) gpsPowerOff();
      }
      break;

    case SMART_COOLDOWN: {
      if (gpsStandby && timeReached(now, gpsStandbyUntilMs) &&
          !smartCooldownObservationWindowDue(now)) {
        // CAS12's finite standby should have expired.  Stop consuming its
        // resumed stream before returning to the low-power armed state.
        gpsPowerOff();
      }
      if (!timeReached(now, smartCooldownUntilMs)) break;
      const bool continuedMotion = smartMotionRecent(now);
      // Establish a new displacement baseline after this classification.  The
      // current fix was parsed during cooldown but is never stored here.
      updateSmartMotionReferenceFromGps(now);
      smartMotionDetected = continuedMotion;
      smartLastMotionMs = continuedMotion ? now : 0;
      smartCooldownObservationOpen = false;
      if (continuedMotion && !smartRetryAfterCooldown) {
        // Consume the movement decision now. TRACKING used to discard the
        // displacement baseline and demand a second MPU verification.
        startSmartAcquisition();
        break;
      }
      if (gpsPowered || gpsStandby || gpsStandbyPending) gpsPowerOff();
      if (!configureMpuArmed()) {
        disableSmartMpuRuntime();
        break;
      }
      setSmartState(SMART_ARMED);
      if (smartRetryAfterCooldown) {
        // A failed fix is eligible for another attempt only after the full
        // cooldown and a fresh motion verification.  Keep the retry marker
        // until that verifier accepts; this also makes lastWakeReason truthful
        // when the next acquisition actually starts.
        DBG_PRINTLN("Smart cooldown complete; waiting for motion reconfirmation before retry.");
      }
      break;
    }
  }
}

void scheduleAfterSavedFix() {
  if (!POWER_OPTIMIZATION_ENABLED) return;

  // At 1 minute, keeping the UART/parser active is intentional.  The receiver
  // supply is always on and there is no physical gate on this hardware.
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

  const uint32_t fixSentencesBefore = gps.sentencesWithFix();
  while (Serial1.available()) {
    const char value = (char)Serial1.read();
    if (smartState == SMART_ACQUIRING &&
        !smartAcquisitionNmeaSynchronized) {
      if (value != '$') continue;
      smartAcquisitionNmeaSynchronized = true;
    }
    observeNmeaByte(value);
    gps.encode(value);
  }

  const uint32_t fixSentencesAfter = gps.sentencesWithFix();
  const bool validFixSentenceReceived =
      fixSentencesAfter != fixSentencesBefore;
  const bool locationUpdated = consumeGpsLocationFreshness();
  if (consumeGpsUtcFreshness()) gpsUtcUpdateCounter++;
  // TinyGPS++ increments sentencesWithFix() only for checksum-valid RMC/GGA
  // navigation fixes.  Requiring that counter edge together with a consumed
  // location update rejects status-V RMCs, which update cached date/time but
  // deliberately leave the previous valid coordinates untouched.
  if (!validFixSentenceReceived || !locationUpdated ||
      !gps.location.isValid()) return;

  const uint32_t epoch = gpsEpochUtc();
  if (epoch == 0) return;

  gpsLocationUpdateCounter++;

  const bool smartActive = smartRuntimeActive();
  const bool intervalRuntime = trackingMode == TRACKING_INTERVAL ||
                               runtimeIntervalFallback;

  if (smartActive) {
    // GNSS speed is a second, independent continued-motion signal.  MPU
    // samples remain the trigger; GNSS can keep an active route alive when a
    // device is moving smoothly and the accelerometer is quiet.
    gnssContinuedMotion(millis());

    const bool postStartNavigation =
        gps.sentencesWithFix() != smartAcquisitionFixSentenceBaseline &&
        gpsLocationUpdateCounter != smartAcquisitionLocationBaseline &&
        gpsUtcUpdateCounter != smartAcquisitionUtcBaseline &&
        gps.location.isValid() && gps.date.isValid() && gps.time.isValid() &&
        gps.location.age() <= SMART_GNSS_STALE_MS;

    if (smartState == SMART_ACQUIRING && smartFirstFixPending &&
        postStartNavigation) {
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
        smartFirstFixPending = false;
        smartAcquisitionStartedMs = millis();
        smartLastMotionMs = smartAcquisitionStartedMs;
        smartMotionDetected = true;
        updateSmartMotionReferenceFromRecord(rec);
        gpsEverHadFix = true;
        DBG_PRINTLN("Smart GNSS fix acquired; entering 120 s cooldown.");
        enterSmartCooldown(false);
      }
      return;
    }

    // No Smart state uses the Interval scheduler.  In particular, a valid
    // NMEA update during CAS12/cooldown must not bypass the fix cooldown.
    return;
  }

  // If we woke just to establish current UTC and the next configured point is
  // still far away, go back to sleep. Once inside the final acquisition lead
  // window, stay powered so the receiver keeps its lock until the point is due.
  const uint32_t nextDueEpoch = lastStoredEpoch + logIntervalSeconds;
  if (lastStoredEpoch != 0 && !timeReached(epoch, nextDueEpoch)) {
    const uint32_t remainingSeconds = nextDueEpoch - epoch;

    if (intervalRuntime && POWER_OPTIMIZATION_ENABLED &&
        logIntervalSeconds > GPS_ACQUIRE_LEAD_SECONDS &&
        remainingSeconds > GPS_ACQUIRE_LEAD_SECONDS) {
      const uint32_t sleepSeconds =
          remainingSeconds - GPS_ACQUIRE_LEAD_SECONDS;
      DBG_PRINTF("Next point due in %lus; sleeping %lus before pre-wake.\n",
                 (unsigned long)remainingSeconds,
                 (unsigned long)sleepSeconds);
      scheduleGpsSleep(sleepSeconds * 1000UL);
    }

    // Interval mode (including a Smart hardware fallback) waits for its
    // configured due time before saving the current fix.
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
    if (intervalRuntime) {
      scheduleAfterSavedFix();
    } else if (smartState == SMART_TRACKING) {
      smartLastMotionMs = millis();
    }
  }
}

void serviceGpsDiagnostics() {
  if (!debugSerialActive) return;
  const uint32_t now = millis();
  if (!firstGpsDiag && !elapsedMs(now, lastGpsDiagMs, 5000)) return;
  firstGpsDiag = false;
  lastGpsDiagMs = now;

  DBG_PRINTF("TRACKING DIAG saved=%s runtime=%s fallback=%u\n",
             trackingModeLabel(trackingMode),
             smartRuntimeActive() ? "Smart" : "Interval",
             runtimeIntervalFallback ? 1U : 0U);
  if (trackingMode == TRACKING_SMART) {
    DBG_PRINTF("SMART DIAG state=%s cooldown=%us mpu=%u irqAttached=%u "
               "intPin=%u irqCount=%lu motionEvents=%lu intStatus=0x%02X "
               "samples=%u active=%u\n",
               smartStateLabel(smartState), smartCooldownRemainingSeconds(now),
               mpuAvailable ? 1U : 0U, mpuInterruptAttached ? 1U : 0U,
               (unsigned)digitalRead(MPU_INT_PIN), (unsigned long)mpuIrqCount,
               (unsigned long)mpuMotionEventCount, mpuLastInterruptStatus,
               smartVerifySamples, smartVerifyActiveSamples);
  }

  if (!gpsPowered) {
    if (smartRuntimeActive()) {
      DBG_PRINTF("GPS DIAG uart=INACTIVE supply=ALWAYS_ON stored=%lu\n",
                 (unsigned long)storedCount);
    } else {
      DBG_PRINTF("GPS DIAG uart=INACTIVE supply=ALWAYS_ON nextWake=%lus stored=%lu\n",
                 (unsigned long)secondsUntilGpsWake(),
                 (unsigned long)storedCount);
    }
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
        "GPS LOCATION: %.7f, %.7f ageMs=%lu -- logging controlled by %s state.\n",
        gps.location.lat(),
        gps.location.lng(),
        (unsigned long)age,
        smartRuntimeActive() ? "Smart" : "Interval");
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

uint16_t smartCooldownRemainingSeconds(uint32_t now) {
  if (smartState != SMART_COOLDOWN || timeReached(now, smartCooldownUntilMs)) {
    return 0;
  }

  const uint32_t remainingMs = smartCooldownUntilMs - now;
  const uint32_t remainingSeconds = (remainingMs + 999UL) / 1000UL;
  return remainingSeconds > 65535UL ? 65535U : (uint16_t)remainingSeconds;
}

void sendSmartInfo() {
  uint8_t payload[12] = {};
  const uint32_t now = millis();

  // Fixed v2 wire layout.  Do not append fields: clients use the exact
  // 12-byte length to distinguish Smart Info from the legacy 65-byte INFO.
  payload[0] = SMART_INFO_PROTOCOL_VERSION;
  payload[1] = (uint8_t)trackingMode;
  payload[2] = smartSensitivity;
  payload[3] = (uint8_t)smartState;
  payload[4] = (uint8_t)(SMART_VERIFY_WINDOW_MS / 1000UL);
  payload[5] = (uint8_t)min((uint16_t)255, SMART_STANDBY_SLICE_SECONDS);
  putU16(payload + 6, SMART_FIX_COOLDOWN_SECONDS);
  putU16(payload + 8, smartCooldownRemainingSeconds(now));

  uint8_t flags = 0;
  if (mpuAvailable) flags |= SMART_INFO_FLAG_MPU_PRESENT;
  if (mpuInterruptAttached) flags |= SMART_INFO_FLAG_MPU_INTERRUPT_ARMED;
  if (cas12State == CAS12_SUPPORTED) flags |= SMART_INFO_FLAG_CAS12_VERIFIED;
  if (gpsStandby && cas12State == CAS12_SUPPORTED) {
    flags |= SMART_INFO_FLAG_CAS12_ACTIVE;
  }
  if (runtimeIntervalFallback) flags |= SMART_INFO_FLAG_RUNTIME_INTERVAL_FALLBACK;
  if (gpsReceiverActiveCapability()) {
    flags |= SMART_INFO_FLAG_GPS_RECEIVER_ACTIVE;
  }
  payload[10] = flags;
  payload[11] = (uint8_t)lastWakeReason;

  sendPacket(RSP_SMART_INFO, payload, sizeof(payload));
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
  if (gpsPowered) powerFlags |= INFO_POWER_GPS_UART_ACTIVE;
  if (POWER_OPTIMIZATION_ENABLED) powerFlags |= INFO_POWER_OPTIMIZATION;
  if (GPS_POWER_CONTROL_ENABLED) powerFlags |= INFO_POWER_PHYSICAL_GATE;
  if (flashSleeping) powerFlags |= INFO_POWER_FLASH_SLEEPING;
  if (GPS_SUPPLY_ALWAYS_ON) powerFlags |= INFO_POWER_SUPPLY_ALWAYS_ON;
  if (mpuAvailable) powerFlags |= INFO_POWER_MPU_AVAILABLE;
  if (cas12State == CAS12_SUPPORTED) powerFlags |= INFO_POWER_CAS12_SUPPORTED;
  if (gpsStandby) powerFlags |= INFO_POWER_GPS_STANDBY;
  payload[55] = powerFlags;

  putU16(payload + 56,
         (uint16_t)min((uint32_t)65535, secondsUntilGpsWake()));
  putU16(payload + 58, (uint16_t)logIntervalSeconds);

  putU16(payload + 60, 0); // reserved battery mV
  payload[62] = 2;
  payload[63] = 0;
  payload[64] = 0;

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

    case CMD_GET_SMART_INFO:
      if (len != 0) {
        sendError(ERR_BAD_PAYLOAD, "SMART_INFO_LEN");
        return;
      }
      sendSmartInfo();
      break;

    case CMD_SET_SMART_CONFIG:
      if (len != 2) {
        sendError(ERR_BAD_PAYLOAD, "SMART_CONFIG_LEN");
        return;
      }
      if (payload[0] > TRACKING_SMART) {
        sendError(ERR_BAD_MODE, "BAD_MODE");
        return;
      }
      if (payload[1] > SMART_MAX_SENSITIVITY) {
        sendError(ERR_BAD_SENSITIVITY, "BAD_SENSITIVITY");
        return;
      }
      if (!setSmartConfig((TrackingMode)payload[0], payload[1])) {
        sendError(ERR_FLASH, "SMART_CONFIG_SAVE_FAILED");
        return;
      }
      sendAck(CMD_SET_SMART_CONFIG);
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
  Bluefruit.Advertising.setInterval(160, 3200); // 100 ms fast, 2 s slow
  Bluefruit.Advertising.setFastTimeout(10);
  Bluefruit.Advertising.start(0);

  DBG_PRINTF("Advertising as %s, pairing PIN %s\n", BLE_DEVICE_NAME, BLE_PAIRING_PIN);
  DBG_PRINTF("Owner lock: %s\n", ownerSet ? "SET" : "not set (first phone may pair)");
}

void initializeTrackingRuntime() {
  applyTrackingRuntime();
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
    DBG_PRINTLN("FW diagnostic build: motion-rearm-1");
    DBG_PRINTLN("USB Serial Monitor baud: 115200");
    DBG_PRINTF("GPS Serial1 baud: %lu\n", (unsigned long)GPS_BAUD);
  }

  configureBatteryCharging();

  // D1 is intentionally unused on v2 hardware: the GPS VCC rail is 3V3 and
  // remains physically powered.  gpsPowerOn/Off only open/close Serial1.

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

  if (!initMpu6050()) {
    // The MPU is optional at runtime.  Keep the tracker alive and expose the
    // missing capability/runtime fallback in INFO/0x85 instead of failing the
    // whole device.
    mpuAvailable = false;
    DBG_PRINTLN("MPU6050 unavailable; saved Smart mode will use Interval fallback this boot.");
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

  initializeTrackingRuntime();

  gpsNextWakeMs = millis();
  if (trackingMode == TRACKING_SMART && smartStartupProbePending) {
    cas12StartupSinceMs = millis();
  }
  serviceGpsPowerState();
}

void loop() {
  serviceGpsPowerState();
  serviceMpu();
  serviceGps();
  serviceCas12Probe();
  serviceSmartState();
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
