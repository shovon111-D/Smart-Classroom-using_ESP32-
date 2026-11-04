Here is your updated ESP32 Smart Classroom System pin configuration, including the RFID RST and flame sensor changes.

## Complete pin configuration

| Component         | Pin / Signal        | ESP32 GPIO | Connection    |
| ----------------- | ------------------- | ---------- | ------------- |
| LCD 1602 I2C      | SDA                 | GPIO 21    | ESP32 GPIO 21 |
|                   | SCL                 | GPIO 22    | ESP32 GPIO 22 |
| RFID RC522        | SDA / SS            | GPIO 5     | ESP32 GPIO 5  |
|                   | SCK                 | GPIO 18    | ESP32 GPIO 18 |
|                   | MOSI                | GPIO 23    | ESP32 GPIO 23 |
|                   | MISO                | GPIO 19    | ESP32 GPIO 19 |
|                   | RST                 | —          | 3.3V directly |
|                   | VCC                 | —          | 3.3V          |
|                   | GND                 | —          | GND           |
| Servo Motor       | Signal              | GPIO 16    | ESP32 GPIO 16 |
| Buzzer            | Positive / Signal   | GPIO 17    | ESP32 GPIO 17 |
| MQ-2 Smoke Sensor | A0 (Analog Output)  | GPIO 35    | ESP32 GPIO 35 |
|                   | D0 (Digital Output) | GPIO 34    | ESP32 GPIO 34 |
| Flame Sensor      | D0 (Digital Output) | GPIO 4     | ESP32 GPIO 4  |
| 4×4 Keypad        | R1                  | GPIO 13    | ESP32 GPIO 13 |
|                   | R2                  | GPIO 14    | ESP32 GPIO 14 |
|                   | R3                  | GPIO 27    | ESP32 GPIO 27 |
|                   | R4                  | GPIO 26    | ESP32 GPIO 26 |
|                   | C1                  | GPIO 25    | ESP32 GPIO 25 |
|                   | C2                  | GPIO 33    | ESP32 GPIO 33 |
|                   | C3                  | GPIO 32    | ESP32 GPIO 32 |
|                   | C4                  | GPIO 15    | ESP32 GPIO 15 |

## Power connections

| Component    | VCC                                            | GND        |
| ------------ | ---------------------------------------------- | ---------- |
| LCD 1602 I2C | 5V (as supported by module)                    | GND        |
| RFID RC522   | 3.3V                                           | GND        |
| Servo        | External regulated 5V recommended              | Common GND |
| Buzzer       | As supported by buzzer                         | GND        |
| MQ-2         | 5V                                             | GND        |
| Flame Sensor | As supported by module; ensure D0 is 3.3V-safe | GND        |

## Important notes

- RFID RST: Must be connected to 3.3V, not left floating.
- GPIO 4: Now exclusively assigned to the flame sensor D0.
- GPIO 34 and 35: Input-only pins, suitable for the MQ-2 outputs.
- GPIO 15: Used by keypad column C4; it is a boot-strapping pin, so avoid external circuitry that forces an unsuitable level during ESP32 startup.
- Common GND: All externally powered components must share a common ground with the ESP32.
- Voltage safety: Never feed a 5V digital output directly into an ESP32 GPIO.

This is the consolidated pin map based on your original configuration and the agreed RFID/flame sensor changes.