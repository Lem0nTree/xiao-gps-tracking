# 1000 mAh LiPo wiring

This guide matches the assembled Smart Motion v2 hardware: a 1S 1000 mAh LiPo,
an XIAO nRF52840, a GY-521/MPU6050, and an ATGM336H GPS receiver. The receiver
and motion sensor share the XIAO `3V3` rail and ground.

## Connections

```text
1000 mAh 1S LiPo
   +------------------------------> XIAO BAT
   +------------------------------> XIAO GND (LiPo negative)

MPU6050 / GY-521                  XIAO
VCC -----------------------------> 3V3
GND -----------------------------> GND
SDA -----------------------------> D4 / SDA
SCL -----------------------------> D5 / SCL
INT -----------------------------> D2

ATGM336H GPS                      XIAO
VCC -----------------------------> 3V3
GND -----------------------------> GND
TX  -----------------------------> D7 / RX
RX  <----------------------------- D6 / TX
PPS -----------------------------> not connected

Owner-reset test point: D0 -> GND only when performing the boot reset procedure
D1: not connected (unused/reserved)
```

The GY-521 default I²C address is `0x68`; leave `AD0`, `XDA`, and `XCL`
unconnected. D3 and D8–D10 are free in this assembly.

## Power and signal checks

- Verify the LiPo connector polarity before connecting it to the XIAO battery
  input. Do not connect the raw cell to the sensor or GPS signal rail.
- Confirm that the GPS and MPU VCC pins are on XIAO `3V3`, with one common
  ground.
- GPS serial communication is 9600 baud. Cross TX and RX as shown above.
- D1 is unused. Do not wire it to a supply, receiver control input, or another
  power circuit.
- This assembly has no boost converter, load switch, GPS ON/OFF connection, or
  physical GPS power gate. The GPS supply is electrically present whenever the
  XIAO 3V3 rail is active.
- Check the cell manufacturer's permitted charge profile and the charger's
  suitability before charging. Do not charge a damaged, swollen, or unprotected
  LiPo cell.

Firmware can reduce UART/parser and receiver activity and may use CASIC timed
standby only after a runtime capability probe. Those logical states are not a
physical removal of GPS supply.

## Validation status

The wiring is the documented target assembly; physical checks remain pending.
Before relying on the tracker, verify MPU identity/address/register read-back
and interrupt operation, GPS 3V3 operation and NMEA/navigation, and the two
CAS12 standby/resumption cycles. Measure whole-device current in both wake
modes and calibrate Smart thresholds on representative stationary, walking,
city, and smooth-driving runs. No current, runtime, or calibration result is
claimed by this wiring guide.
