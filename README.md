# XIAO GPS Tracker Smart Motion v2.0

**A compact, private, low-cost GPS tracker built from accessible maker hardware.**

The tracker logs positions locally without a SIM card, subscription, or cloud
account, then transfers them over bonded Bluetooth to an Android phone for
viewing or export.

![Render of the printable XIAO GPS Tracker enclosure](enclosure/xiao-gps-tracker-enclosure.png)

The assembled v2 hardware uses a Seeed Studio XIAO nRF52840, an ATGM336H GPS
receiver, a GY-521/MPU6050 motion sensor, a 1000 mAh 1S LiPo, and the native
Android companion app.

## Why this tracker?

- **No monthly fee** — no SIM card and no data plan.
- **Private by design** — tracks stay on the device and your phone.
- **Two recording modes** — Interval keeps scheduled logging; Smart Motion
  waits for sustained movement before acquiring a location.
- **Battery-conscious operation** — the firmware reduces receiver, UART, BLE,
  MCU, and flash activity where the assembled hardware permits.
- **Large offline history** — more than 104,000 positions fit in the onboard
  circular log.
- **Easy phone sync** — connect only when needed; logging continues without the
  phone.
- **Useful exports** — save a journey as GPX or CSV.
- **Open maps** — the app uses MapLibre and OpenFreeMap, with light and dark
  themes.
- **Pocket-size hardware** — an enclosure is included as STL, 3MF, and Fusion
  360 source.

## Smart Motion and Interval modes

Interval mode preserves the six saved schedules: 1, 15, or 30 minutes, or 1,
2, or 3 hours. Smart mode retains that saved interval but does not use the
interval scheduler while selected. A motion interrupt starts a short
accelerometer verification; sustained dynamic acceleration then starts GPS
acquisition, with a fresh valid location and UTC fix required before logging.
After a fix, a cooldown prevents duplicate wakes. Continued movement can start
another acquisition; when movement stops, the tracker returns to its armed
state. Smart defaults to Balanced sensitivity. The detailed state machine,
BLE packet layout, and migration rules are in the [firmware notes](firmware/README.md).

The legacy interval command explicitly selects Interval mode, so older apps
continue to configure scheduled logging. Smart controls require the v2 Android
app and firmware.

## Actual v2 power and wiring model

The assembled hardware connects both the GPS receiver and the MPU6050 to the
XIAO `3V3` rail. It has a common ground, a 1000 mAh 1S LiPo connected to the
XIAO battery input, and no boost converter, load switch, GPS ON/OFF lead, or
physical GPS power gate. GPS supply is therefore electrically present whenever
the XIAO 3V3 rail is active.

Firmware reports these concepts separately:

- **Physical supply:** the receiver supply on this build; always connected
  while the XIAO rail is active.
- **Receiver activity:** whether the receiver is actively producing navigation
  data or is in a verified receiver-side standby state.
- **UART/parser activity:** whether the firmware has opened the GPS UART and is
  accepting NMEA data.

The firmware may probe the receiver's CASIC timed-standby capability after NMEA
has been established. It uses the optimization only after two complete
standby/resumption checks with valid navigation data; otherwise it keeps the
receiver running as required and reports that standby is unavailable. This is
an optimization, not a Smart-mode dependency. Current draw, standby timing,
and battery runtime still require physical measurement on the assembled unit.

## What you need

AliExpress changes listings and sellers frequently, so these are non-affiliate
search links rather than endorsements of a particular shop.

| Component | What to look for | AliExpress |
| --- | --- | --- |
| Seeed Studio XIAO nRF52840 | Standard nRF52840 board; Sense is not required | [Search](https://www.aliexpress.com/w/wholesale-xiao-nrf52840.html) |
| ATGM336H GPS receiver | 3.3 V-compatible module with `VCC`, `RX`, `TX`, and `GND` | [Search](https://www.aliexpress.com/w/wholesale-atgm336h-gps-module.html) |
| GY-521 / MPU6050 | Breakout with `VCC`, `GND`, `SDA`, `SCL`, and `INT` | [Search](https://www.aliexpress.com/w/wholesale-mpu6050-gy-521.html) |
| 1000 mAh 1S LiPo | Cell compatible with the XIAO battery input and enclosure | [Search](https://www.aliexpress.com/w/wholesale-1000mah-3.7v-lipo-battery.html) |

> [!IMPORTANT]
> Check dimensions, pin labels, logic levels, polarity, and the cell's charger
> requirements before ordering. Similar-looking marketplace modules are not
> always electrically equivalent.

## How it works

1. In Interval mode, the GPS obtains a position at the saved schedule and the
   XIAO saves it to onboard flash.
2. In Smart mode, the MPU6050 identifies a motion candidate, firmware verifies
   sustained movement, and the GPS obtains a fresh fix before a point is
   stored.
3. The Android app connects over bonded BLE to sync, browse, and export the
   route. Logging does not require the phone to remain connected.

The first bonded phone becomes the owner. A hardware recovery procedure can
change the owner without erasing the route history or saved wake settings.

## Android companion app

The v2 Android app (`2.0.0`, versionCode `20`) shows tracker status, the
Interval/Smart selector, Smart sensitivity, map, and time-based route timeline.
It syncs the local log over BLE and exports tracks as GPX or CSV without sending
them to a cloud service. It also reports runtime fallback and receiver standby
capability when the firmware provides those statuses.

<p align="center">
  <img src="docs/images/xiao-tracker-android-app.jpg" alt="XIAO Tracker Android app showing tracker status, wake mode, map, and timeline" width="360">
</p>

## Included in this repository

```text
firmware/   Arduino firmware, setup notes, and detailed wiring
android/    Native Android companion app
enclosure/  STL, 3MF, Fusion 360 source, render, and render script
docs/       README images and project media
```

### Start here

- [Firmware setup and technical details](firmware/README.md)
- [1000 mAh LiPo wiring guide](firmware/WIRING_1000MAH.md)
- [Android project](android/XiaoGpsTrackerApp)
- [Printable enclosure](enclosure/Seeed_GPS_Tracker.stl)
- [Release notes](RELEASE_NOTES.md)

For the firmware, use the non-mbed **Seeed nRF52 Boards** Arduino core and
change the example BLE pairing PIN before flashing. The Android project targets
JDK 17.

## Storage capacity

The tracker holds 104,244 GPS points before its circular log overwrites the
oldest entries. The table below applies to scheduled Interval mode; Smart mode
is event-driven, so its history length depends on movement and fix frequency.

| Interval mode setting | On-device history |
| --- | ---: |
| 1 minute | 72 days |
| 15 minutes | 3.0 years |
| 30 minutes | 5.9 years |
| 1 hour | 11.9 years |
| 2 hours | 23.8 years |
| 3 hours | 35.7 years |

Points already synced to Android remain in the phone archive even after the
tracker eventually overwrites its oldest records.

## Validation status

The software release defines the v2 state machine, BLE extension, metadata
migration, and runtime capability reporting. Physical validation is still
pending: sensor identity/address/interrupt checks, GPS 3.3 V operation and
NMEA checks, CAS12 standby cycles, current measurements, and sensitivity
calibration on the assembled hardware. No battery-runtime or power-saving
measurement is claimed yet. See [VALIDATION.txt](VALIDATION.txt) for the
release checklist.

This is a personal/offline logger, not a live anti-theft or emergency tracker:
it has no cellular connection and does not continuously report its location to
a remote service.

## Future development

The current BLE tracker is the foundation for planned variants built around the
same compact, local-first platform. These ideas are a roadmap and are **not
implemented in the current release**.

### RF version

- Add a **LoRa radio** for long-range, low-power position and status
  communication beyond normal Bluetooth range.
- Explore **Meshtastic integration** so compatible nodes can relay tracker data
  through an off-grid LoRa mesh.
- Retain local flash logging when a radio link or gateway is unavailable.

### SIM version

- Integrate a **SIM800L GSM/GPRS module** for remote position updates where
  compatible 2G service is still available.
- Add store-and-forward behavior so the tracker can upload queued positions
  after coverage returns.
- Redesign the power stage for the SIM800L's transmission-current peaks.

> [!NOTE]
> SIM800L depends on 2G service, which has already been retired in some
> countries and networks. Regional network compatibility must be checked before
> developing or buying parts for this variant.

### Long-autonomy version

- Replace the pouch cell with a protected, quality **18650 Li-ion cell** for
  substantially more stored energy.
- Develop a larger enclosure with safe cell retention and service access.
- Profile every always-on load and tune the GPS, BLE, LoRa, or Meshtastic duty
  cycle for extended unattended operation.

Future autonomy targets will be published only after complete hardware builds
have been measured under representative conditions.

## License

See [LICENSE](LICENSE).
