# XIAO GPS Tracker 1.5.0

Private GPS logger for the Seeed Studio XIAO nRF52840 + 5 V ATGM GPS breakout, with a native Android companion app.

## 1.5 highlights

- GPS wake interval is configurable from the Android phone and persisted on the XIAO.
- Presets: **1 min, 15 min, 30 min, 1 hour, 2 hours, 3 hours**.
- Default after a fresh install/migration is **30 minutes**.
- Battery profile redesigned for the 5-pin GPS carrier with no exposed VBAT.
- BLE remains slowly discoverable, but the phone connection automatically closes after idle time.
- Android action bar is simplified: Connect, Sync, and one **⋮ More** menu.
- GPX, CSV, and Clear History moved into **More**.
- Timeline navigation, dark mode, OpenFreeMap/MapLibre, local archive, bonded-owner security, and safe system-bar insets are retained.

## Hardware wiring

The GPS carrier shown in the supplied photos exposes:

```text
5V
RX
TX
GND
PPS
```

For a 300 mAh 1S LiPo:

```text
300 mAh LiPo
   ├────────────────────> XIAO BAT
   │
   └──> 3.7 V → 5 V boost/load switch
                    │
                    ├── 5 V OUT ───> GPS 5V
                    └── EN <──────── XIAO D1

GPS TX  ────────────────────────────> XIAO D7 / RX
GPS RX  <──────────────────────────── XIAO D6 / TX
GPS GND ───────────────────────────── XIAO GND / boost GND
GPS PPS ───────────────────────────── optional / unused
```

**D1 is an enable signal only. Do not power the GPS directly from D1.**

See `WIRING_300MAH.md` for the full wiring notes.

## Firmware setup

Use the non-mbed **Seeed nRF52 Boards** Arduino core and select:

```text
Tools → Board → Seeed nRF52 Boards → Seeed XIAO nRF52840
```

Install:

1. TinyGPSPlus
2. Adafruit SPIFlash
3. Adafruit TinyUSB Library

Open:

```text
firmware/XiaoGpsTracker/XiaoGpsTracker.ino
```

Change the example BLE pairing PIN before deployment:

```cpp
static const char BLE_PAIRING_PIN[] = "482731";
```

Arduino Serial Monitor is **115200 baud**. The GPS UART is **9600 baud**.

## Configurable GPS interval

The Android app sends the selected interval over the encrypted/bonded BLE channel.

Allowed values:

```text
1 minute
15 minutes
30 minutes
1 hour
2 hours
3 hours
```

The setting is saved in QSPI metadata and survives resets/power cycles.

Firmware 1.5 migrates the old firmware-1.0 owner metadata to the new combined metadata format so the owner identity can be preserved while adding the interval setting.

### Power behavior

For the **1-minute** setting:

- the GPS remains powered after the first valid fix;
- this avoids forcing a cold start every minute on a carrier with no exposed VBAT;
- this is the highest-power profile.

For **15 minutes and longer**:

1. a valid point is saved;
2. the GPS 5 V boost is disabled;
3. the XIAO keeps BLE advertising slowly;
4. the GPS wakes **60 seconds before** the next point is due;
5. once a satellite fix is obtained in that pre-wake window, the GPS stays powered so it keeps lock until the due timestamp;
6. the point is stored and GPS power is removed again.

If the receiver cannot get an eligible fix within 120 seconds of waking, it sleeps for 60 seconds before retrying.

This is designed around the reality that the 5-pin carrier does not expose VBAT: long intervals benefit from full power gating, while the one-minute interval does not.

## BLE power behavior

The XIAO advertises continuously at a slow interval so the phone can find it without a physical wake button.

It does **not** need to remain connected.

The Android app:

- connects when you press **Connect**;
- reads tracker state / changes settings / syncs records;
- automatically disconnects after roughly one minute of BLE inactivity;
- keeps the downloaded timeline fully usable after BLE disconnects.

The tracker resumes slow advertising after disconnect.

## Storage

Each GPS record is 20 bytes:

- sequence number
- UTC Unix timestamp
- latitude × 10^7
- longitude × 10^7
- HDOP × 100
- satellite count
- CRC-8

The circular log contains 104,244 records.

Approximate theoretical history before overwrite:

```text
1 min   ≈ 72 days
15 min  ≈ 3.0 years
30 min  ≈ 5.9 years
1 hour  ≈ 11.9 years
2 hours ≈ 23.8 years
3 hours ≈ 35.7 years
```

The phone archive is independent: already-synced points remain on Android even after the XIAO circular buffer eventually overwrites them.

## Android app 1.5.0

Open:

```text
android/XiaoGpsTrackerApp
```

The project uses JDK 17 and keeps the Kotlin compiler block outside `android {}`:

```kotlin
kotlin {
    compilerOptions {
        jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
    }
}
```

### Main controls

The bottom action row is intentionally minimal:

- **Connect**
- **Sync track**
- **⋮ More**

`More` contains:

- Export GPX
- Export CSV
- Clear history

The Live Status card also contains the current **GPS wake interval**. Tap it while connected to choose another preset.

### GPS interval selector

The phone shows:

```text
1 minute · high power
15 minutes
30 minutes · recommended
1 hour
2 hours
3 hours
```

After selection, firmware persists the new value and returns the updated tracker INFO packet.

### Timeline

The timeline is based on real UTC timestamps, not record number.

Scrubbing the slider:

- selects the nearest recorded timestamp;
- shows exact local date/time;
- moves the map marker;
- shows coordinates, satellites, HDOP, and elapsed time;
- highlights the travelled route and mutes the later route.

This remains useful even when the tracker is recording only once every 30 minutes or several hours.

### Dark mode

The app retains the DayNight theme and moon/sun control. The MapLibre basemap switches between OpenFreeMap light and dark styles.

## BLE protocol addition in 1.5

New command:

```text
0x05 CMD_SET_INTERVAL
payload: uint32 little-endian seconds
```

Accepted payload values are only:

```text
60, 900, 1800, 3600, 7200, 10800
```

The command requires the same encrypted, bonded owner connection as all other tracker commands.

The existing 65-byte INFO packet is retained; its `logIntervalSeconds` field now reports the persisted configured interval and the firmware version reports `1.5.0`.

## Owner-phone recovery

The first successfully bonded phone is the owner.

To replace it without deleting GPS history or the configured interval:

1. power off;
2. connect **D0 to GND**;
3. power/reset and hold D0 low for at least 5 seconds;
4. release D0;
5. pair the new phone.

The owner identity is cleared; the GPS log and configured wake interval are preserved.

## Interval troubleshooting in 1.5.1

When changing the GPS interval, Android Logcat should show:

```text
XiaoGpsBle: NUS RX write start type=0x5 ...
XiaoGpsBle: NUS RX write completed status=0 ...
XiaoGpsProtocol: RX type=0x84 payload=1
```

Then Android requests INFO and verifies that the value persisted.

If the app shows:

- `Firmware does not support this command` — the XIAO is still running firmware older than 1.5.
- `Could not save the setting to tracker flash` — QSPI metadata persistence failed.
- `Unsupported GPS interval` — the firmware rejected a value outside the six supported presets.
