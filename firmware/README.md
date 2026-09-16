# XIAO GPS Tracker firmware

Technical setup notes for the Seeed Studio XIAO nRF52840 firmware. For the
project overview and Android app, return to the [main README](../README.md).

## Arduino setup

Use the non-mbed **Seeed nRF52 Boards** Arduino core and select:

```text
Tools -> Board -> Seeed nRF52 Boards -> Seeed XIAO nRF52840
```

Install these Arduino libraries:

1. TinyGPSPlus
2. Adafruit SPIFlash
3. Adafruit TinyUSB Library

Open [XiaoGpsTracker/XiaoGpsTracker.ino](XiaoGpsTracker/XiaoGpsTracker.ino).
Arduino Serial Monitor uses **115200 baud** and the GPS UART uses **9600 baud**.

Before deployment, change the example six-digit BLE pairing PIN:

```cpp
static const char BLE_PAIRING_PIN[] = "482731";
```

## Actual v2 wiring

The assembled build uses a 1000 mAh 1S LiPo, a GY-521/MPU6050 at I²C address
`0x68`, and an ATGM336H receiver powered from the XIAO `3V3` rail. Grounds are
common. The GPS supply is continuously connected while the XIAO 3V3 rail is
active; this build has no boost converter, load switch, GPS ON/OFF lead, or
physical GPS power gate.

| XIAO pin or rail | Connect to | Notes |
| --- | --- | --- |
| `BAT` | LiPo positive; LiPo negative to `GND` | 1S cell; check polarity and charger suitability |
| `3V3` | MPU6050 `VCC` and GPS `VCC` | Use the documented 3V3 rail |
| `GND` | MPU6050 `GND` and GPS `GND` | One common ground |
| `D2` | MPU6050 `INT` | Motion interrupt |
| `D4` | MPU6050 `SDA` | I²C data |
| `D5` | MPU6050 `SCL` | I²C clock |
| `D6` | GPS `RX` | XIAO TX -> receiver RX |
| `D7` | GPS `TX` | Receiver TX -> XIAO RX |
| `D0` | Owner-reset test point | Hold to `GND` during boot for 5 seconds |
| `D1` | No connection | Unused/reserved for a future design |
| `D3`, `D8`–`D10` | No connection | Free in the assembled build |

Leave the GY-521 `AD0`, `XDA`, and `XCL` pins unconnected. The default `AD0`
state selects address `0x68`. Leave GPS `PPS` unconnected unless a future
firmware design explicitly uses it.

See the detailed [1000 mAh LiPo wiring guide](WIRING_1000MAH.md) before
connecting power. Do not connect a battery, sensor, or receiver through D1;
there is no external power-control circuit in this assembly.

## Recording modes

The persisted wake modes are:

```text
WAKE_MODE_INTERVAL = 0
WAKE_MODE_SMART    = 1
```

### Interval mode

Interval mode retains the existing six persisted schedules:

```text
1 minute
15 minutes
30 minutes (default)
1 hour
2 hours
3 hours
```

The GPS obtains a valid location for each scheduled point. The legacy
`CMD_SET_INTERVAL` command also selects Interval mode, preserving the expected
behavior of older Android apps.

### Smart Motion mode

Smart mode leaves the saved interval untouched but does not use the interval
scheduler while selected. An MPU6050 interrupt moves the tracker from `ARMED`
to `VERIFYING`. Verification samples dynamic acceleration (gravity removed) at
20 Hz for 5 seconds, or 100 samples. A candidate is accepted when at least 60
samples are active and no quiet gap exceeds 1 second. A rejected candidate
returns to `ARMED` without a GPS acquisition.

An accepted candidate moves to `ACQUIRING`. Firmware requires a fresh valid
location and UTC from the GPS within 90 seconds, then stores the point and
enters a 120-second cooldown. Recent accelerometer activity and reliable GPS
speed/displacement determine whether movement continues. Continued movement
can start another acquisition; when movement stops, the tracker returns to
`ARMED`. A failed acquisition is retried after the cooldown only when movement
is reconfirmed.

Smart sensitivity presets are starting calibration values, not measured final
thresholds:

| Preset | Wire value | Starting dynamic-acceleration threshold |
| --- | ---: | ---: |
| High | `0` | 80 mg |
| Balanced (default) | `1` | 160 mg |
| Low | `2` | 300 mg |

The firmware initializes the MPU by direct register access, checks identity and
critical register read-back, and arms its interrupt. If that initialization or
the interrupt fails, the saved Smart selection is preserved but that boot runs
Interval behavior and reports `runtimeIntervalFallback`.

### Motion rearm diagnostic build

The startup banner `FW diagnostic build: motion-rearm-1` identifies the
post-v2.0 debugging patch. Serial output now includes saved mode, actual runtime
mode, fallback, Smart state, the real Smart cooldown, D2 level, interrupt count,
motion-event count, and verification sample counts/rejection reasons. An
Interval `nextWake` countdown is printed only when Interval actually runs.
The legacy INFO wake countdown is zero while Smart runs; use Smart Info for
its cooldown. Android's displayed state is the last BLE read-back, not a
continuous live indication.

The patch recovers a latched-high MPU interrupt even when the rising edge was
missed, checks the Seeed core's interrupt attachment result, checks interrupt
clearing failures, and rearms the sensor on return to ARMED. Continued movement
at a successful fix's cooldown expiry starts the next acquisition directly.
Data-ready interrupts supply verification timing; software no longer discards
fresh samples just because readout jitter makes successive reads less than
50 ms apart.

The armed profile uses `DLPF_CFG=0`, `MOT_DUR=1` per the low-power motion
configuration flow in the [InvenSense product specification, section 8.1](https://hw101.tbs1.de/mpu6050/doc/mpu6050.pdf).
This replaces the initial plan's 100-count hardware-duration setting: that
setting is not validated as a 100 ms debounce in 5 Hz cycle mode. The
five-second software confirmation remains the sustained-movement filter.
The candidate threshold follows that flowchart's 32 mg/LSB scale, rounding
down to codes 2/5/9 for High/Balanced/Low. The previous Balanced code of 80
would correspond to 2560 mg at that scale. Documentation revisions disagree
on motion-threshold units; actual module calibration is still pending. The
software verifier retains the exact 80/160/300 mg presets regardless of the
hardware wake threshold.

Run the focused host regression checks from the repository root:

```sh
python3 firmware/tests/test_motion_runtime.py
```

These compile the actual C-compatible scheduler/verifier function bodies with
GCC and fake peripherals/clock. They cover repeated cooldowns, motion after
rearming, missed interrupt edges, jittered 20 Hz samples, quiet rejection,
failure retry, rollover, and Interval scheduling. They do not validate the
physical MPU, GPS, BLE stack, or flash writes.

## GPS activity and CAS12 standby

The firmware keeps these states distinct:

- **Physical supply:** electrically present on `3V3` whenever the XIAO rail is
  active; there is no physical GPS power-off control on this hardware.
- **Receiver activity:** whether the ATGM336H is navigating or in receiver-side
  timed standby.
- **UART/parser activity:** whether `Serial1` is open and NMEA is being
  accepted by the firmware.

After normal NMEA has been observed, firmware can probe the receiver's CASIC
`$PCAS12` timed-standby capability. It attempts two controlled standby cycles
and accepts the capability only if each cycle shows the expected NMEA silence,
resumption, fresh RMC/GGA sentences, and subsequent valid navigation. A failed
probe is marked unavailable for the boot; it is not continuously retried.
When unavailable, the receiver remains active as required and the status does
not claim a power saving that was not verified. CAS12 is an optimization only;
Smart motion and GPS acquisition do not depend on it.

Standby slices may be selected from the firmware's short/medium/long profiles
(30, 60, or 120 seconds). Thirty seconds is a compatibility workaround, not a
claim that it is optimal. Slice current and resume latency must be measured on
the actual receiver before choosing a production default.

## Storage and metadata

The XIAO's 2 MiB QSPI flash contains a circular log with 104,244 records. Each
20-byte record contains:

- sequence number;
- UTC Unix timestamp;
- latitude and longitude multiplied by 10^7;
- HDOP multiplied by 100;
- satellite count;
- CRC-8.

The 32-byte metadata record is version 2. It retains the owner identity and
interval and consumes two previously reserved bytes for `wakeMode` and
`smartSensitivity`. A version-1 record migrates in place conceptually while
preserving owner and interval, defaulting the new fields to Interval and
Balanced. The GPS records remain byte-for-byte compatible.

Owner reset (D0 held low during boot) clears only ownership. It preserves the
route history, interval, wake mode, and Smart sensitivity. Clearing the route
also preserves these settings; neither operation is a factory reset.

## BLE framing and v2 protocol

Packets use the existing frame:

```text
magic A5 5A | type:u8 | len:u16 little-endian | payload | CRC16-CCITT
```

The CRC covers `type`, `len`, and `payload`. Existing command and response
types remain available, including the 65-byte `INFO` response. The v2 extension
adds:

```text
CMD_GET_SMART_INFO   = 0x06   payload: empty
CMD_SET_SMART_CONFIG = 0x07   payload: mode:u8, sensitivity:u8 (exactly 2 bytes)
RSP_SMART_INFO       = 0x85   payload: exactly 12 bytes
```

`CMD_SET_SMART_CONFIG` returns the normal ACK and the client reads Smart Info
back to verify persisted settings. The 12-byte `RSP_SMART_INFO` payload is:

| Offset | Field | Encoding |
| ---: | --- | --- |
| 0 | `protocolVersion` | `u8` |
| 1 | `mode` | `u8` (`0` Interval, `1` Smart) |
| 2 | `sensitivity` | `u8` preset (`0` High, `1` Balanced, `2` Low) |
| 3 | `motionState` | `u8` state code |
| 4 | `confirmationSeconds` | `u8` |
| 5 | `standbySliceSeconds` | `u8` |
| 6–7 | `fixCooldownSeconds` | `u16` little-endian |
| 8–9 | `cooldownRemainingSeconds` | `u16` little-endian |
| 10 | `flags` | `u8` bit field |
| 11 | `lastWakeReason` | `u8` |

The defined flag bits are:

```text
0x01 MPU_PRESENT
0x02 MPU_INTERRUPT_ARMED
0x04 CAS12_VERIFIED
0x08 CAS12_ACTIVE
0x10 RUNTIME_INTERVAL_FALLBACK
0x20 GPS_RECEIVER_ACTIVE
```

There is deliberately no physical-GPS-power-off flag for this assembly. A
runtime fallback means the saved Smart setting remains available for a later
boot or hardware configuration; it does not silently rewrite the user's mode.

Older apps can still bond, read the legacy INFO packet, sync records, clear
logs, and set the six interval values. They cannot configure or display Smart
mode until upgraded to the v2 app.

## BLE behavior and security

The first successfully bonded phone becomes the owner. Tracker commands require
an encrypted, bonded owner connection. Unknown or unbonded phones are
disconnected. The app disconnects after BLE inactivity so the phone need not
stay connected while logging.

To replace the owner phone without deleting GPS history or wake settings:

1. Power off the tracker.
2. Connect **D0 to GND**.
3. Power or reset it and hold D0 low for at least five seconds.
4. Release D0.
5. Pair the replacement phone.

## Validation status

Source-level protocol, metadata, and state-machine checks can be performed
without a board. Physical validation remains pending for the assembled unit:
MPU `WHO_AM_I`, address, register read-back, and interrupt; GPS 3.3 V operation,
UART/NMEA, and valid navigation; two CAS12 standby/resumption cycles; current
draw in Interval and Smart states; and sensitivity calibration while stationary,
walking, in a city, and in smooth driving. See [VALIDATION.txt](../VALIDATION.txt)
for the release checklist. Do not use software defaults as measured hardware
results.
