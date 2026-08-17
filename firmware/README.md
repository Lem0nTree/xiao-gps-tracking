# XIAO GPS Tracker firmware

Technical setup notes for the Seeed Studio XIAO nRF52840 firmware. For the project overview, parts list, enclosure, and Android app, return to the [main README](../README.md).

## Arduino setup

Use the non-mbed **Seeed nRF52 Boards** Arduino core and select:

```text
Tools -> Board -> Seeed nRF52 Boards -> Seeed XIAO nRF52840
```

Install these Arduino libraries:

1. TinyGPSPlus
2. Adafruit SPIFlash
3. Adafruit TinyUSB Library

Open [XiaoGpsTracker/XiaoGpsTracker.ino](XiaoGpsTracker/XiaoGpsTracker.ino). Arduino Serial Monitor uses **115200 baud** and the GPS UART uses **9600 baud**.

Before deployment, change the example six-digit BLE pairing PIN:

```cpp
static const char BLE_PAIRING_PIN[] = "482731";
```

## Wiring

The reference GPS carrier exposes `5V`, `RX`, `TX`, `GND`, and `PPS`, but no backup-battery pin. It therefore needs a switched 3.7 V to 5 V boost converter when powered from a 1S LiPo.

See the full [300 mAh LiPo wiring guide](WIRING_300MAH.md) before connecting power.

```text
GPS TX  -> XIAO D7 / RX
GPS RX  -> XIAO D6 / TX
GPS GND -> XIAO GND / boost GND
GPS PPS -> optional / unused
XIAO D1 -> boost-converter EN
```

> [!CAUTION]
> D1 is an enable signal only. Never power the GPS directly from D1, and never feed the boost converter's 5 V output into XIAO VBUS.

## Configurable recording interval

The Android app can select one of six persisted intervals over the encrypted, bonded BLE connection:

```text
1 minute
15 minutes
30 minutes (default)
1 hour
2 hours
3 hours
```

At one minute, the GPS remains powered after its first valid fix to avoid repeated cold starts. At 15 minutes or longer, the GPS is power-gated and wakes 60 seconds before the next point is due. If it cannot obtain an eligible fix within 120 seconds, it sleeps for 60 seconds before retrying.

## Storage

The XIAO's 2 MiB QSPI flash contains a circular log with 104,244 records. Each 20-byte record contains:

- sequence number;
- UTC Unix timestamp;
- latitude and longitude multiplied by 10^7;
- HDOP multiplied by 100;
- satellite count;
- CRC-8.

Sector 0 stores tracker metadata, including the owner identity and configured interval. The remaining sectors store the circular GPS log.

## BLE behavior and security

The tracker advertises at a slow interval so the phone can find it without a wake button. It does not need to stay connected while logging.

The first successfully bonded phone becomes the owner. Tracker commands require an encrypted, bonded owner connection. The Android app connects to read status, change settings, or sync records, then disconnects automatically after roughly one minute of BLE inactivity.

To replace the owner phone without deleting GPS history or the configured interval:

1. Power off the tracker.
2. Connect **D0 to GND**.
3. Power or reset it and hold D0 low for at least five seconds.
4. Release D0.
5. Pair the replacement phone.

## BLE interval command

```text
0x05 CMD_SET_INTERVAL
payload: uint32 little-endian seconds
```

Accepted values are `60`, `900`, `1800`, `3600`, `7200`, and `10800`. The existing 65-byte INFO packet reports the saved `logIntervalSeconds` value.

When diagnosing an interval change, Android Logcat should show a `0x05` NUS write followed by the `0x84` ACK and a fresh INFO request. The Android app reports distinct errors for unsupported firmware, flash persistence failure, and an unsupported interval.
