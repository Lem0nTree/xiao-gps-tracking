# 300 mAh LiPo wiring

The reference GPS breakout exposes **5V / RX / TX / GND / PPS** and does not expose VBAT.

## Recommended wiring

```text
300 mAh 1S LiPo
   +------------------------------> XIAO BAT
   |
   +--> low-Iq 3.7 V to 5 V boost/load switch
                           |
                           +-- 5 V OUT --> GPS 5V
                           +-- EN <------- XIAO D1

GPS TX  --------------------------------> XIAO D7 / RX
GPS RX  <--------------------------------- XIAO D6 / TX
GPS GND ---------------------------------> XIAO GND / boost GND
GPS PPS ---------------------------------> optional / unused
```

## Important checks

- D1 drives only the boost/load-switch **enable input**.
- Never connect GPS VCC directly to D1.
- The boost EN input must accept 3.3 V logic.
- All grounds must be common.
- Do not feed the boost converter's 5 V output into XIAO VBUS.
- The firmware assumes EN is active-high. If the converter is active-low, change `GPS_POWER_ACTIVE_HIGH`.
- Confirm LiPo polarity at the XIAO before connecting the battery; small JST leads are not universally wired the same way.

## Why the boost is switched

The five-pin carrier has no exposed backup-battery input, so the firmware cannot reliably retain GNSS state while removing main power.

Firmware uses two strategies:

- **1-minute interval:** keep the GPS powered after the first fix to preserve satellite lock.
- **15 minutes to 3 hours:** remove GPS main power, then wake it 60 seconds before the next point is due.

Choose a boost converter with low shutdown and quiescent current. At longer tracking intervals, the converter's idle current can become a significant part of the total battery budget.

## LiPo charging

The firmware selects the XIAO's lower 50 mA charge-current setting for the 300 mAh profile. Verify the cell manufacturer's permitted charge rate before use. Do not charge a damaged, swollen, or unprotected LiPo cell.
