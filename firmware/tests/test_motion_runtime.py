#!/usr/bin/env python3
"""Host regression checks against actual sketch function bodies (C-compatible subset).

Requires Python 3 and gcc. Peripherals are fakes; this is not a target build or
proof of physical interrupt delivery. Generated artifacts live in /tmp.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

SKETCH = Path(__file__).resolve().parents[1] / 'XiaoGpsTracker/XiaoGpsTracker.ino'
source = SKETCH.read_text()
if '--baseline' in sys.argv:
    source = subprocess.check_output(
        ['git', 'show', 'HEAD:firmware/XiaoGpsTracker/XiaoGpsTracker.ino'],
        cwd=SKETCH.parents[2], text=True)


def function(name):
    match = re.search(r'^\w+ ' + name + r'\([^\n]*\) \{', source, re.M)
    assert match, name
    # All selected functions have balanced braces, including comments.
    start = match.start()
    pos = source.index('{', start)
    depth = 1
    end = pos + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


constants = '\n'.join(re.findall(
    r'^static const (?:uint\d+_t|float|bool) (?:SMART_|MPU_|GPS_|POWER_)[\s\S]*?;', source, re.M))
# Only the constants used in these functions are needed. Resolve pin aliases
# and standby build configuration for the host translation unit.
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#define D2 2
#define D4 4
#define D5 5
#define SMART_STANDBY_SLICE_SECONDS_CONFIG 60U
#define HIGH 1
#define DBG_PRINTF(...) ((void)0)
#define DBG_PRINTLN(...) ((void)0)
#define noInterrupts() ((void)0)
#define interrupts() ((void)0)
enum { SMART_DISABLED, SMART_ARMED, SMART_VERIFYING, SMART_ACQUIRING,
       SMART_TRACKING, SMART_COOLDOWN, SMART_WAITING_FOR_STOP, SMART_ACQUIRING_STOP };
enum { SMART_WAKE_NONE, SMART_WAKE_MOTION, SMART_WAKE_RETRY, SMART_WAKE_STOP };
uint8_t smartTrackingProfile, lastWakeReason;
bool smartFirstFixPending;
uint32_t fakeNow;
uint32_t millis(void) { return fakeNow; }
bool mpuAvailable, mpuInterruptAttached, mpuDataReadyPending;
bool runtimeIntervalFallback, mpuSampleInitialized, smartMotionDetected;
bool smartRetryAfterCooldown, smartCooldownObservationOpen;
bool gpsPowered, gpsStandby, gpsStandbyPending;
bool smartSelected = true, smartStartupProbePending;
enum { CAS12_UNKNOWN, CAS12_PROBING };
int cas12State;
uint32_t gpsNextWakeMs, gpsAcquireStartedMs;
uint8_t mpuLastInterruptStatus;
uint32_t mpuMotionEventCount, mpuLastMotionMs, smartLastMotionMs;
uint32_t mpuQuietSinceMs, mpuLastSampleMs, smartVerifyStartedMs;
uint32_t smartVerifyLastActiveMs, smartStateSinceMs, smartAcquisitionStartedMs;
uint32_t gpsStandbyUntilMs, smartCooldownUntilMs;
uint16_t smartVerifySamples, smartVerifyActiveSamples, smartVerifyPeakMg;
uint16_t mpuDynamicMagnitudeMg;
float mpuGravityX, mpuGravityY, mpuGravityZ;
int smartState, fakePin, fakeStatus, acquisitions, rearmCalls, sampleNumber;
bool motionEvidence, gnssEvidence, shake, i2cOk = true;
bool smartRuntimeActive(void) { return smartSelected && !runtimeIntervalFallback; }
uint16_t smartSensitivityThresholdMg(void) { return 160; }
int digitalRead(int pin) { (void)pin; return fakePin; }
bool fakeReadStatus(uint8_t reg, uint8_t *value) {
  (void)reg; *value = fakeStatus; fakeStatus = 0; fakePin = 0;
  return i2cOk;
}
#define mpuReadRegister(reg, value) fakeReadStatus(reg, &(value))
bool fakeAcceleration(int16_t *x, int16_t *y, int16_t *z) {
  *x = shake ? (++sampleNumber % 2 ? 14000 : -14000) : 0;
  *y = 0; *z = 16384; return i2cOk;
}
#define mpuReadAcceleration(x,y,z) fakeAcceleration(&(x), &(y), &(z))
bool configureMpuArmed(void) { rearmCalls++; return i2cOk; }
bool configureMpuVerification(void) { return i2cOk; }
void disableSmartMpuRuntime(void) {
  runtimeIntervalFallback = true; smartState = SMART_DISABLED;
}
void setSmartState(int state) { smartState = state; }
void startSmartAcquisition(void) {
  acquisitions++; smartState = SMART_ACQUIRING;
  smartAcquisitionStartedMs = fakeNow; smartRetryAfterCooldown = false;
}
void enterSmartCooldown(bool retry) {
  smartState = SMART_COOLDOWN; smartCooldownUntilMs = fakeNow + 120000;
  smartRetryAfterCooldown = retry;
}
bool gnssContinuedMotion(uint32_t now) { (void)now; return gnssEvidence; }
bool smartMotionRecent(uint32_t now) { (void)now; return motionEvidence; }
void updateSmartMotionReferenceFromGps(uint32_t now) { (void)now; }
void gpsPowerOff(void) { gpsPowered = gpsStandby = gpsStandbyPending = false; }
void gpsPowerOn(void) { gpsPowered = true; gpsAcquireStartedMs = fakeNow; }
void serviceGpsStandbyState(void) {}
'''

tests = r'''
static void candidate(bool edge) {
  fakeStatus = MPU_INT_STATUS_MOTION;
  fakePin = HIGH; mpuDataReadyPending = edge;
  serviceMpu();
  assert(smartState == SMART_VERIFYING);
}
static void continuousMovement(void) {
  uint32_t start = fakeNow;
  shake = true;
  // Exactly 20 Hz samples, processed alternately 1 ms late / on time.
  // The old 50 ms gate discards the 49 ms inter-read gaps.
  for (int i = 1; i <= 100; i++) {
    fakeNow = start + i * 50 + (i % 2);
    fakeStatus = MPU_INT_STATUS_DATA_READY; fakePin = HIGH;
    mpuDataReadyPending = true; serviceMpu();
  }
  assert(smartState == SMART_ACQUIRING);
}
int main(void) {
  fakeNow = 10000; mpuAvailable = mpuInterruptAttached = true;
  smartState = SMART_ARMED;
  // Lost edge / latched HIGH must still start verification.
  candidate(false); continuousMovement(); assert(acquisitions == 1);
  puts("PASS latched HIGH recovery and 100 jittered samples");

  for (int cycle = 0; cycle < 3; cycle++) {
    enterSmartCooldown(false); motionEvidence = true; gnssEvidence = false;
    int before = acquisitions;
    fakeNow = smartCooldownUntilMs - 1; serviceSmartState();
    assert(smartState == SMART_COOLDOWN && acquisitions == before);
    fakeNow++; serviceSmartState();
    assert(smartState == SMART_ACQUIRING && acquisitions == before + 1);
  }
  puts("PASS three consecutive cooldowns: no early acquire; MPU-only continuation");

  enterSmartCooldown(false); motionEvidence = false;
  fakeNow = smartCooldownUntilMs; int rearms = rearmCalls;
  serviceSmartState(); assert(smartState == SMART_ARMED);
  assert(rearmCalls == rearms + 1);
  candidate(true); continuousMovement();
  puts("PASS stopped tracker rearms and a new departure acquires");

  enterSmartCooldown(true); motionEvidence = true;
  fakeNow = smartCooldownUntilMs; serviceSmartState();
  assert(smartState == SMART_ARMED);
  candidate(true); continuousMovement();
  puts("PASS failed-fix retry requires fresh motion verification");

  fakeNow = UINT32_MAX - 60000; enterSmartCooldown(false);
  fakeNow = smartCooldownUntilMs - 1; serviceSmartState();
  assert(smartState == SMART_COOLDOWN);
  fakeNow++; serviceSmartState(); assert(smartState == SMART_ACQUIRING);
  puts("PASS cooldown across millis rollover");

  smartState = SMART_ARMED; candidate(true); shake = false;
  for (int i = 0; i < 22; i++) {
    fakeNow += 50; fakeStatus = MPU_INT_STATUS_DATA_READY;
    fakePin = HIGH; mpuDataReadyPending = true; serviceMpu();
  }
  assert(smartState == SMART_ARMED);
  puts("PASS stationary samples rejected by quiet-gap rule");

  gpsPowered = false; gpsNextWakeMs = fakeNow + 980000;
  serviceGpsPowerState(); assert(!gpsPowered);
  fakeNow = gpsNextWakeMs; serviceGpsPowerState(); assert(!gpsPowered);
  candidate(false); continuousMovement();
  puts("PASS obsolete Interval deadline does not gate Smart movement");

  smartSelected = false; gpsPowered = false;
  gpsNextWakeMs = fakeNow + 10000;
  serviceGpsPowerState(); assert(!gpsPowered);
  fakeNow = gpsNextWakeMs; serviceGpsPowerState(); assert(gpsPowered);
  puts("PASS Interval scheduler still wakes at its own deadline");
  smartSelected = true; smartState = SMART_ARMED;

  smartTrackingProfile = SMART_PROFILE_POINT_TO_POINT;
  fakeNow = 10000; smartLastMotionMs = fakeNow;
  candidate(true); continuousMovement();
  assert(smartAcquisitionTimeoutMs() == 300000);
  uint32_t lastAcceleration = smartLastMotionMs;
  fakeNow += 90000; serviceSmartState();
  assert(smartState == SMART_ACQUIRING);
  finishSmartPointAcquisition();
  assert(smartState == SMART_WAITING_FOR_STOP && !gpsPowered);
  assert(smartLastMotionMs == lastAcceleration);
  int beforePoints = acquisitions;
  for (int i = 0; i < 5; i++) {
    fakeNow += 120000; fakeStatus = MPU_INT_STATUS_MOTION; fakePin = HIGH;
    serviceMpu(); serviceSmartState(); serviceGpsPowerState();
    assert(smartState == SMART_WAITING_FOR_STOP && !gpsPowered);
    assert(acquisitions == beforePoints && smartLastMotionMs == fakeNow);
  }
  puts("PASS Point-to-point movement resets stop clock without periodic GPS");

  lastAcceleration = smartLastMotionMs;
  fakeNow = lastAcceleration + 599999; serviceSmartState();
  assert(smartState == SMART_WAITING_FOR_STOP);
  fakeNow++; serviceSmartState(); serviceGpsPowerState();
  assert(smartState == SMART_ACQUIRING_STOP && gpsPowered);
  assert(acquisitions == beforePoints + 1 && lastWakeReason == SMART_WAKE_STOP);
  fakeNow += 1000; fakeStatus = MPU_INT_STATUS_MOTION; fakePin = HIGH;
  serviceMpu();
  assert(smartState == SMART_WAITING_FOR_STOP && !gpsPowered);
  fakeNow += 599999; serviceSmartState();
  assert(smartState == SMART_WAITING_FOR_STOP);
  fakeNow++; serviceSmartState(); finishSmartPointAcquisition();
  assert(smartState == SMART_ARMED);
  beforePoints = acquisitions;
  fakeNow += 1200000; serviceSmartState();
  assert(smartState == SMART_ARMED && acquisitions == beforePoints);
  puts("PASS exact 10-minute stop, resumed-motion cancellation, one arrival only");

  candidate(true); continuousMovement();
  fakeNow = smartAcquisitionStartedMs + 299999; serviceSmartState();
  assert(smartState == SMART_ACQUIRING);
  fakeNow++; serviceSmartState();
  assert(smartState == SMART_WAITING_FOR_STOP);
  fakeNow = smartLastMotionMs + 600000; serviceSmartState();
  assert(smartState == SMART_ACQUIRING_STOP);
  fakeNow += 300000; serviceSmartState();
  assert(smartState == SMART_ARMED);
  puts("PASS Point-to-point five-minute GPS timeouts remain bounded");

  smartState = SMART_WAITING_FOR_STOP;
  smartLastMotionMs = UINT32_MAX - 300000;
  fakeNow = smartLastMotionMs + 599999; serviceSmartState();
  assert(smartState == SMART_WAITING_FOR_STOP);
  fakeNow++; serviceSmartState();
  assert(smartState == SMART_ACQUIRING_STOP);
  finishSmartPointAcquisition();
  smartLastMotionMs = 0; smartState = SMART_WAITING_FOR_STOP;
  fakeNow = 600000; serviceSmartState();
  assert(smartState == SMART_ACQUIRING_STOP);
  puts("PASS Point-to-point stop clock across rollover and zero timestamp");

  i2cOk = false; fakePin = HIGH; serviceMpu();
  assert(runtimeIntervalFallback);
  puts("PASS I2C failure falls back to Interval");
  return 0;
}
'''

names = ['timeReached', 'elapsedMs', 'smartPointToPoint', 'smartAcquisitionActive',
         'smartAcquisitionTimeoutMs', 'finishSmartPointAcquisition',
         'smartCooldownObservationWindowDue',
         'secondsUntilGpsWake', 'scheduleGpsSleep', 'serviceGpsPowerState',
         'rejectMpuVerification', 'serviceMpu', 'serviceSmartState']
with tempfile.TemporaryDirectory(prefix='xiao-motion-tests-') as directory:
    root = Path(directory)
    translation = root / 'motion.c'
    translation.write_text(preamble + constants + '\n' +
                           '\n'.join(function(name) for name in names) + tests)
    binary = root / 'motion'
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra',
                    '-Wno-unused-const-variable', '-Wno-unused-parameter',
                    str(translation), '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

    # Exercise actual register writes/readback and status-clear failures.
    profile = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define D2 2
#define D4 4
#define D5 5
#define SMART_STANDBY_SLICE_SECONDS_CONFIG 60U
#define DBG_PRINTF(...) ((void)0)
#define noInterrupts() ((void)0)
#define interrupts() ((void)0)
uint8_t registers[256];
bool mpuDataReadyPending, failClear;
void delay(int ms) { (void)ms; }
uint16_t smartSensitivityThresholdMg(void) { return 160; }
bool mpuWriteRegister(uint8_t reg, uint8_t value) {
  registers[reg] = value; return true;
}
bool readRegister(uint8_t reg, uint8_t *value) {
  *value = registers[reg]; return !(failClear && reg == 0x3A);
}
#define mpuReadRegister(reg, value) readRegister(reg, &(value))
'''
    profile += constants + '\n' + '\n'.join(function(name) for name in [
        'smartMotionThresholdRegister', 'mpuReadbackProfile',
        'clearMpuInterruptPending', 'configureMpuArmed', 'configureMpuVerification'])
    profile += r'''
int main(void) {
  assert(configureMpuArmed());
  assert(registers[MPU_REG_CONFIG] == 0);
  assert(registers[MPU_REG_MOT_DUR] == 1);
  assert(registers[MPU_REG_MOT_THR] == 5);
  assert(registers[MPU_REG_PWR_MGMT_1] == 0x28);
  assert(registers[MPU_REG_PWR_MGMT_2] == 0x47);
  assert(registers[MPU_REG_INT_ENABLE] == 0x40);
  assert(configureMpuVerification());
  assert(registers[MPU_REG_CONFIG] == 3);
  assert(registers[MPU_REG_SMPLRT_DIV] == 49);
  assert(registers[MPU_REG_INT_ENABLE] == 1);
  failClear = true;
  assert(!configureMpuArmed());
  assert(!configureMpuVerification());
  puts("PASS MPU profile writes/readback and interrupt-clear failures");
}
'''
    translation.write_text(profile)
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra',
                    '-Wno-unused-const-variable', str(translation),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
