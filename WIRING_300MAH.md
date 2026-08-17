# 300 mAh LiPo wiring — XIAO GPS Tracker 1.5

The GPS breakout shown in the supplied photos exposes **5V / RX / TX / GND / PPS** and does not expose VBAT.

## Recommended wiring

```text
300 mAh 1S LiPo
   ├──────────────────────────> XIAO BAT
   │
   └──> low-Iq 3.7 V → 5 V boost/load switch
                       │
                       ├── 5 V OUT ──> GPS 5V
                       └── EN <─────── XIAO D1

GPS TX  ─────────────────────────────> XIAO D7 / RX
GPS RX  <───────────────────────────── XIAO D6 / TX
GPS GND ────────────────────────────── XIAO GND / boost GND
GPS PPS ────────────────────────────── optional / unused
```

## Important

- D1 only drives the boost/load-switch **enable input**.
- Never connect the GPS VCC directly to D1.
- The boost EN input must accept 3.3 V logic.
- All grounds must be common.
- Do not feed the boost 5 V output back into XIAO VBUS.
- The firmware assumes EN is active-high. If your converter is active-low, change `GPS_POWER_ACTIVE_HIGH`.

## Why the boost is switched

The carrier has no exposed VBAT pin, so the firmware cannot reliably retain GNSS backup state while removing main power.

Firmware 1.5 therefore uses two strategies:

- **1-minute interval:** keep GPS powered after first fix to preserve satellite lock.
- **15 minutes to 3 hours:** remove GPS main power, then wake it 60 seconds before the next timestamp is due.

Choose a boost converter with low shutdown/quiescent current; at long tracking intervals its idle current can become a meaningful part of the battery budget.

## LiPo charging

Firmware selects the XIAO's lower 50 mA charge-current setting for the 300 mAh profile. Verify your battery manufacturer's allowed charge rate before use.
