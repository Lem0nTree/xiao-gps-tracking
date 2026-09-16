#!/usr/bin/env python3
"""Host checks of the sketch's actual profile persistence and GPS-save paths.

Requires g++. Fake flash, navigation fields, and UART isolate boundary behavior;
this does not emulate NMEA decoding or physical peripherals.
"""
from pathlib import Path
import re
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] /
          'XiaoGpsTracker/XiaoGpsTracker.ino').read_text()


def function(name):
    match = re.search(r'^\w+ ' + name + r'\([^\n]*\) \{', source, re.M)
    assert match, name
    end = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


constants = '\n'.join(re.findall(
    r'^static const (?:uint\d+_t|float|bool) (?:SMART_|MPU_|GPS_|POWER_|METADATA_|LEGACY_OWNER_|DEFAULT_LOG_)[\s\S]*?;',
    source, re.M))
types = source[source.index('#pragma pack(push, 1)'):source.index('static const uint32_t RECORD_SIZE')]
preamble = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
using std::min;
#define D2 2
#define D4 4
#define D5 5
#define SMART_STANDBY_SLICE_SECONDS_CONFIG 60U
#define DBG_PRINTF(...) ((void)0)
#define DBG_PRINTLN(...) ((void)0)
uint32_t fakeNow = 10000;
uint32_t millis() { return fakeNow; }
'''
fakes = r'''
static const uint32_t SECTOR_SIZE = 4096;
MetadataRecord disk{}, metadataRecord{};
struct FakeFlash {
  bool fail = false;
  int erases = 0, writes = 0;
  bool eraseSector(int) { erases++; return !fail; }
  void waitUntilReady() {}
  size_t writeBuffer(int, const uint8_t* data, size_t n) {
    writes++; memcpy(&disk, data, n); return n;
  }
  size_t readBuffer(int, uint8_t* data, size_t n) {
    memcpy(data, &disk, n); return n;
  }
} flash;
void flashWake() {}
void flashSleep() {}
bool ownerSet;
uint32_t logIntervalSeconds;
TrackingMode trackingMode;
uint8_t smartSensitivity, smartTrackingProfile;
SmartState smartState;
SmartWakeReason lastWakeReason;
bool runtimeIntervalFallback;
Cas12State cas12State = CAS12_UNKNOWN;
bool smartStartupProbePending, cas12ProbeAttempted;
void finishCas12Probe(bool supported) {
  cas12State = supported ? CAS12_SUPPORTED : CAS12_UNSUPPORTED;
}
int resets = 0;
void applyTrackingRuntime() { resets++; smartState = SMART_ARMED; }
void cancelCas12ProbeForModeChange() {}
bool gpsPowered, gpsStandby, gpsStandbyPending, smartFirstFixPending;
bool smartRetryAfterCooldown, smartMotionDetected, smartAcquisitionNmeaSynchronized;
bool gpsEverHadFix;
uint32_t smartAcquisitionStartedMs, smartLastMotionMs, mpuQuietSinceMs;
uint32_t smartAcquisitionFixSentenceBaseline, smartAcquisitionLocationBaseline;
uint32_t smartAcquisitionUtcBaseline, gpsLocationUpdateCounter, gpsUtcUpdateCounter;
uint32_t gpsAcquireStartedMs, lastStoredEpoch;
int writes = 0, gnssCalls = 0;
bool appendOk = true;
struct Field {
  bool valid = true;
  uint32_t raw = 1, old = 0;
  bool isValid() { return valid; }
  uint32_t age() { return old; }
  uint32_t value() { return raw; }
  double lat() { return 45.0; }
  double lng() { return 9.0; }
};
struct FakeGps {
  Field location, date, time, hdop, satellites;
  uint32_t fixes = 10;
  uint32_t sentencesWithFix() { return fixes; }
  void encode(char) { fixes++; }
} gps;
struct FakeSerial {
  bool pending = false;
  bool available() { return pending; }
  char read() { pending = false; return '$'; }
} Serial1;
void observeNmeaByte(char) {}
bool consumeGpsLocationFreshness() { return true; }
bool consumeGpsUtcFreshness() { return true; }
uint32_t gpsEpochUtc() { return 1700000000 + fakeNow / 1000; }
bool appendRecord(GpsRecord&) { if (appendOk) writes++; return appendOk; }
void setSmartState(SmartState state) { smartState = state; }
void gpsPowerOn() { gpsPowered = true; }
void gpsPowerOff() { gpsPowered = gpsStandby = gpsStandbyPending = false; }
void discardPendingGpsInputAtAcquisitionStart() {
  Serial1.pending = false; smartAcquisitionNmeaSynchronized = false;
}
bool configureMpuArmed() { return true; }
void disableSmartMpuRuntime() { runtimeIntervalFallback = true; }
void updateSmartMotionReferenceFromRecord(const GpsRecord&) {}
void gnssContinuedMotion(uint32_t) { gnssCalls++; smartLastMotionMs = fakeNow; }
void enterSmartCooldown(bool) { smartState = SMART_COOLDOWN; }
void scheduleAfterSavedFix() {}
void scheduleGpsSleep(uint32_t) {}
'''
names = ['crc8', 'timeReached', 'elapsedMs', 'isAllowedLogInterval',
         'legacyOwnerRecordValid', 'metadataRecordValid', 'initDefaultMetadata',
         'persistMetadata', 'loadOwnerLock', 'setSmartConfig', 'clearOwnerLock',
         'smartRuntimeActive', 'smartPointToPoint', 'smartAcquisitionActive',
         'smartAcquisitionTimeoutMs', 'startSmartAcquisition',
         'finishSmartPointAcquisition', 'smartPointFixQualityAcceptable', 'serviceGps']
tests = r'''
void seal(MetadataRecord& value) {
  value.crc8 = crc8(reinterpret_cast<const uint8_t*>(&value), sizeof(value) - 1);
}
void navigation() { Serial1.pending = true; serviceGps(); }
int main() {
  initDefaultMetadata();
  assert(sizeof(MetadataRecord) == 32 && sizeof(GpsRecord) == 20);
  assert(persistMetadata() && metadataRecordValid(disk));
  disk.ownerSet = 1; disk.addr[0] = 42; disk.logIntervalSeconds = 900;
  disk.version = METADATA_VERSION_V2;
  disk.reserved[METADATA_RESERVED_MODE] = TRACKING_SMART;
  disk.reserved[METADATA_RESERVED_SENSITIVITY] = 2;
  disk.reserved[METADATA_RESERVED_PROFILE] = 255;
  seal(disk);
  MetadataRecord beforeBoot = disk;
  const int bootErases = flash.erases, bootWrites = flash.writes;
  flash.fail = true; // Reads work; upgrade must boot even if writes cannot.
  assert(loadOwnerLock());
  assert(disk.version == METADATA_VERSION_V2 && metadataRecordValid(disk));
  assert(memcmp(&disk, &beforeBoot, sizeof(disk)) == 0);
  assert(flash.erases == bootErases && flash.writes == bootWrites);
  flash.fail = false;
  assert(ownerSet && metadataRecord.addr[0] == 42 && logIntervalSeconds == 900);
  assert(trackingMode == TRACKING_SMART && smartSensitivity == 2);
  assert(smartTrackingProfile == SMART_PROFILE_CONTINUOUS);
  assert(setSmartConfig(TRACKING_SMART, 1, SMART_PROFILE_POINT_TO_POINT));
  assert(disk.version == METADATA_VERSION_V3 && metadataRecordValid(disk));
  smartTrackingProfile = 0;
  assert(loadOwnerLock() && smartTrackingProfile == SMART_PROFILE_POINT_TO_POINT);
  assert(clearOwnerLock());
  assert(smartTrackingProfile == SMART_PROFILE_POINT_TO_POINT && logIntervalSeconds == 900);
  MetadataRecord before = metadataRecord;
  int previousResets = resets;
  assert(!setSmartConfig(TRACKING_SMART, 1, 2));
  assert(resets == previousResets && memcmp(&metadataRecord, &before, sizeof(before)) == 0);
  flash.fail = true;
  assert(!setSmartConfig(TRACKING_INTERVAL, 0, 0));
  assert(smartTrackingProfile == 1 && trackingMode == TRACKING_SMART && smartSensitivity == 1);
  assert(memcmp(&metadataRecord, &before, sizeof(before)) == 0);
  flash.fail = false;
  disk.version = METADATA_VERSION_V1; disk.ownerSet = 1;
  disk.reserved[0] = disk.reserved[1] = disk.reserved[2] = 255; seal(disk);
  beforeBoot = disk;
  flash.fail = true;
  assert(loadOwnerLock() && trackingMode == TRACKING_INTERVAL);
  assert(memcmp(&disk, &beforeBoot, sizeof(disk)) == 0);
  flash.fail = false;
  assert(ownerSet && logIntervalSeconds == 900 && smartSensitivity == 1 && smartTrackingProfile == 0);
  disk.version = METADATA_VERSION_V3; disk.reserved[0] = 1; disk.reserved[1] = 1;
  disk.reserved[2] = 2; seal(disk); assert(!metadataRecordValid(disk));
  puts("PASS v1/v2 read-only boot with failing flash writes, upgrade on save, owner reset and rollback");

  trackingMode = TRACKING_SMART; smartTrackingProfile = SMART_PROFILE_POINT_TO_POINT;
  gps.hdop.raw = 150; gps.satellites.raw = 6;
  smartLastMotionMs = fakeNow;
  cas12State = CAS12_PROBING; smartStartupProbePending = true;
  startSmartAcquisition();
  assert(cas12State == CAS12_UNSUPPORTED && !smartStartupProbePending && cas12ProbeAttempted);
  puts("PASS Point-to-point retires startup probe before endpoint GPS acquisition");
  serviceGps(); assert(writes == 0); // Cached fields alone cannot create a fix.
  gps.hdop.raw = 151; navigation(); assert(writes == 0);
  gps.hdop.raw = 0; navigation(); assert(writes == 0);
  gps.hdop.raw = 150; gps.satellites.raw = 5; navigation(); assert(writes == 0);
  gps.satellites.raw = 6; gps.hdop.old = 2001; navigation(); assert(writes == 0);
  gps.hdop.old = 0; gps.satellites.valid = false; navigation(); assert(writes == 0);
  gps.satellites.valid = true; gps.time.old = 2001; navigation(); assert(writes == 0);
  gps.time.old = 0; gps.date.old = 2001; navigation(); assert(writes == 0);
  gps.date.old = 0; gps.location.old = 2001; navigation(); assert(writes == 0);
  gps.location.old = 0; fakeNow += 100000; navigation();
  assert(writes == 1 && smartState == SMART_WAITING_FOR_STOP && !gpsPowered);
  assert(smartLastMotionMs == 10000 && gnssCalls == 0);
  gpsPowered = true; navigation(); assert(writes == 1); // No mid-journey records.
  startSmartAcquisition(); smartState = SMART_ACQUIRING_STOP;
  appendOk = false; navigation(); assert(smartFirstFixPending && writes == 1);
  appendOk = true; navigation();
  assert(writes == 2 && smartState == SMART_ARMED && !gpsPowered);
  gpsPowered = true; navigation(); assert(writes == 2);
  puts("PASS fresh high-quality departure/arrival only; GPS never resets acceleration clock");

  startSmartAcquisition(); fakeNow += SMART_POINT_ACQUISITION_TIMEOUT_MS;
  navigation(); assert(writes == 2); // Deadline enforced before scheduler runs.
  smartTrackingProfile = SMART_PROFILE_CONTINUOUS;
  startSmartAcquisition(); gps.hdop.raw = 400; gps.satellites.raw = 4;
  navigation(); assert(writes == 3 && smartState == SMART_COOLDOWN);
  puts("PASS GPS deadline ordering and unchanged Continuous acceptance");
}
'''
with tempfile.TemporaryDirectory(prefix='xiao-profile-tests-') as directory:
    root = Path(directory)
    translation = root / 'profiles.cpp'
    translation.write_text(preamble + constants + '\n' + types + fakes +
                           '\n'.join(function(name) for name in names) + tests)
    binary = root / 'profiles'
    subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra',
                    '-Wno-unused-const-variable', str(translation),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
