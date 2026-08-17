# Release 1.5.0

## Firmware

- Firmware version `1.5.0`.
- Default GPS interval changed to 30 minutes.
- Added persisted phone-configurable GPS intervals:
  - 1 min
  - 15 min
  - 30 min
  - 1 h
  - 2 h
  - 3 h
- Added BLE command `CMD_SET_INTERVAL (0x05)` with a 32-bit seconds payload.
- Added validation so unsupported intervals are rejected.
- Added metadata format v1.5 combining:
  - owner phone identity
  - GPS interval
- Added automatic migration from the firmware-1.0 owner metadata record.
- Owner reset preserves the configured interval.
- For the 1-minute profile, GPS stays powered after first fix to avoid repeated cold starts.
- For intervals >=15 minutes, GPS is power-gated and wakes 60 seconds before the next due timestamp.
- Receiver stays powered once a pre-wake fix is obtained so it can keep satellite lock until the due timestamp.
- Acquisition timeout is 120 seconds, followed by a 60-second retry rest.
- Slow BLE advertising remains available continuously; connected BLE is not required for logging.
- Existing QSPI deep-power-down, 0 dBm BLE TX, USB-only debug logging, LED-off profile, 50 mA LiPo charge setting, bonded owner security, and circular logging are retained.

## Android

- App version `1.5.0`, versionCode 15.
- Added GPS wake interval selector backed by firmware persistence.
- Live Status card shows the selected interval and battery-impact description.
- Bottom controls redesigned to:
  - Connect
  - Sync track
  - More
- GPX, CSV, and Clear History moved into a popup More menu.
- More menu enables/disables actions based on local track availability and BLE connection state.
- Added BLE idle auto-disconnect to reduce connected-radio time while keeping timeline/map usable offline.
- Added public BLE disconnect handling with an OEM-stack fallback.
- Fixed duplicate legacy CCCD descriptor write.
- Existing dark mode, timeline navigation, MapLibre/OpenFreeMap map, bonded pairing, incremental downloads, exports, and system-bar inset handling are retained.

## Protocol

New owner-only command:

```text
CMD_SET_INTERVAL = 0x05
payload = uint32 little-endian seconds
```

Allowed seconds:

```text
60
900
1800
3600
7200
10800
```

INFO remains 65 bytes and now reports firmware version 1.5.0 plus the persisted interval.

## 1.5.1 interval-setting reliability patch

- Android now verifies that the connected XIAO firmware is 1.5+ before enabling interval changes.
- Added a 3.5-second interval command timeout and read-back verification.
- Android logs received protocol packet types under `XiaoGpsProtocol`.
- Protocol errors now have explicit messages for unsupported command, flash save failure, and bad interval.
- Firmware sends only one ACK for `CMD_SET_INTERVAL`; Android requests INFO afterward.
- Firmware restores the previous runtime interval if QSPI metadata persistence fails.
