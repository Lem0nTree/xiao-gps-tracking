# 2.1.1 — Bluetooth startup and scan recovery

- Valid older tracker settings now load without a mandatory flash erase/write
  before Bluetooth starts. The next successful settings or owner save writes
  metadata v3. Owner identity, saved interval, and sensitivity are preserved.
- Android scan sessions now cancel their own timeout and ignore stale callbacks.
  A connection/pairing failure or scan error is no longer overwritten by
  “XIAO-GPS not found,” and a previous scan cannot stop a subsequent scan.
- Firmware checks Bluetooth stack, UART service, and advertising startup results.
  Fatal startup messages repeat for a Serial Monitor opened after boot; periodic
  diagnostics report actual advertising and connection state. Pairing PINs are
  no longer included in serial output.

# 2.1.0 — Point-to-point tracking

- Smart Motion now offers Continuous and Point-to-point profiles in the app,
  replacing the motion-sensitivity picker. Existing sensitivity is preserved.
- Point-to-point captures after confirmed departure and after ten minutes with
  no acceleration. New acceleration resets the stop timer or cancels an arrival
  acquisition. There are no periodic points during a journey.
- Point-to-point waits up to five minutes for fresh GPS with at least six
  satellites and HDOP at most 1.50. Failed attempts are bounded; stationary
  points are not repeatedly attempted. Accuracy remains reception-dependent.
- Metadata v3 preserves owner, interval, and calibration while migrating older
  settings to Continuous. Smart protocol v3 appends the profile; the app also
  supports v2 firmware, with Continuous available until a firmware update.
- Firmware host checks cover stop timing, rollover, GPS quality, persistence,
  and legacy Continuous regressions. Physical board validation remains pending.

# Release 2.0.0 — Smart Motion

Smart Motion v2.0 adds motion-triggered logging while retaining the existing
offline GPS log, bonded BLE ownership, synchronization, exports, and Interval
mode.

## Firmware

- Firmware version `2.0.0`.
- Added persisted wake modes: `Interval (0)` and `Smart (1)`.
- Interval mode retains the six existing schedules: 60, 900, 1800, 3600,
  7200, and 10800 seconds.
- Smart mode preserves the saved interval but ignores its scheduler while
  selected. MPU6050 motion verification starts GPS acquisition; a fresh valid
  location and UTC are required before a point is stored.
- Smart defaults to Balanced sensitivity. High, Balanced, and Low starting
  thresholds are 80, 160, and 300 mg respectively; these values still require
  calibration on the assembled hardware.
- A missing MPU, failed identity/configuration check, or failed interrupt
  causes a runtime Interval fallback for that boot while preserving the saved
  Smart selection.
- Upgraded the 32-byte metadata record to version 2 by consuming two reserved
  bytes for `wakeMode` and `smartSensitivity`. Version-1 metadata migrates with
  owner and interval preserved and defaults new fields to Interval/Balanced.
- Owner reset clears only ownership and preserves route history, interval, wake
  mode, and Smart sensitivity. Clear-log also preserves wake settings.
- Updated GPS state reporting to distinguish physical supply, receiver
  activity, UART/parser activity, and receiver-side timed standby. The actual
  build powers the GPS from XIAO `3V3` continuously and has no physical power
  gate.
- Added a guarded CASIC `$PCAS12` timed-standby capability probe. It is used only
  after two complete standby/resumption checks with valid navigation data and
  is an optimization, not a Smart-mode dependency.

## Android

- App version `2.0.0`, versionCode `20`.
- Added Interval/Smart mode selection and Smart sensitivity controls.
- Added Smart status/capability display, including runtime Interval fallback and
  receiver standby status where reported by firmware.
- Retained legacy interval configuration, incremental downloads, GPX/CSV
  exports, bonded pairing, local maps, dark mode, and BLE idle disconnect.
- Legacy apps remain able to bond, read the 65-byte INFO packet, synchronize and
  clear logs, and set Interval schedules; they do not expose Smart controls.

## Protocol

The existing framed protocol and 65-byte INFO response remain compatible. The
v2 extension adds:

```text
CMD_GET_SMART_INFO   = 0x06   payload: empty
CMD_SET_SMART_CONFIG = 0x07   payload: mode:u8, sensitivity:u8 (exactly 2 bytes;
                                  sensitivity 0=High, 1=Balanced, 2=Low)
RSP_SMART_INFO       = 0x85   payload: exactly 12 bytes
```

The `RSP_SMART_INFO` payload fields are, in order:

```text
protocolVersion:u8
mode:u8
sensitivity:u8
motionState:u8
confirmationSeconds:u8
standbySliceSeconds:u8
fixCooldownSeconds:u16 little-endian
cooldownRemainingSeconds:u16 little-endian
flags:u8
lastWakeReason:u8
```

Defined flags are `MPU_PRESENT`, `MPU_INTERRUPT_ARMED`, `CAS12_VERIFIED`,
`CAS12_ACTIVE`, `RUNTIME_INTERVAL_FALLBACK`, and `GPS_RECEIVER_ACTIVE`. There
is no physical-GPS-power-off flag for the v2 assembly. `CMD_SET_INTERVAL (0x05)`
continues to set the persisted interval and explicitly selects Interval mode.
The normal ACK plus Smart Info read-back verifies Smart configuration
persistence.

## Validation and hardware status

The documented assembly is a 1000 mAh 1S LiPo, XIAO nRF52840, GY-521/MPU6050
(`3V3`, `D4` SDA, `D5` SCL, `D2` INT, address `0x68`), and GPS (`3V3`, TX to
`D7`, RX from `D6`). D1 is unused. There is no boost converter, load switch,
GPS ON/OFF connection, or physical GPS power gate.

Software source and protocol checks are separate from field validation. MPU
identity/configuration/interrupt, GPS UART and navigation, CAS12 two-cycle
runtime verification, whole-device current, battery runtime, and Smart
sensitivity calibration remain pending until measured on the physical
assembly. This release makes no fabricated current, runtime, or calibration
claim.
