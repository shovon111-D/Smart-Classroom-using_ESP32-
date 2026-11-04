#pragma once

#include <Arduino.h>

constexpr uint8_t RFID_SS_PIN = 5;
constexpr uint8_t RFID_RST_PIN = 0xFF;
constexpr uint8_t RFID_SCK_PIN = 18;
constexpr uint8_t RFID_MISO_PIN = 19;
constexpr uint8_t RFID_MOSI_PIN = 23;
constexpr uint8_t SERVO_PIN = 16;
constexpr uint8_t BUZZER_PIN = 17;
constexpr uint8_t FLAME_D0_PIN = 4;
constexpr uint8_t MQ2_D0_PIN = 34;
constexpr uint8_t MQ2_AO_PIN = 35;
constexpr uint8_t LCD_SDA_PIN = 21;
constexpr uint8_t LCD_SCL_PIN = 22;
constexpr uint8_t LCD_ADDRESS = 0x27;
constexpr uint8_t LCD_COLUMNS = 16;
constexpr uint8_t LCD_ROWS = 2;

constexpr uint8_t KEYPAD_ROWS = 4;
constexpr uint8_t KEYPAD_COLS = 4;
constexpr uint8_t KEYPAD_ROW_PINS[KEYPAD_ROWS] = {13, 14, 27, 26};
constexpr uint8_t KEYPAD_COL_PINS[KEYPAD_COLS] = {25, 33, 32, 15};
constexpr uint8_t KEYPAD_CODE_LENGTH = 4;
constexpr uint32_t KEYPAD_TIMEOUT_MS = 5000;

constexpr uint8_t SERVO_LOCKED_DEGREES = 0;
constexpr uint8_t SERVO_UNLOCKED_DEGREES = 90;
constexpr uint32_t ADC_MAX_VALUE = 4095;
constexpr uint8_t ADC_RESOLUTION_BITS = 12;
constexpr uint16_t MQ2_ALARM_THRESHOLD = 1800;
constexpr uint16_t MQ2_HYSTERESIS = 150;
constexpr uint32_t FLAME_WARMUP_MS = 2000;
constexpr uint32_t MQ2_WARMUP_MS = 30000;
constexpr uint32_t SENSOR_SAMPLE_INTERVAL_MS = 100;
constexpr uint32_t ALARM_CONFIRM_MS = 1000;
constexpr uint32_t ALARM_CLEAR_MS = 5000;
constexpr uint32_t ALERT_UPDATE_INTERVAL_MS = 5000;
constexpr uint32_t DOOR_RELOCK_DELAY_MS = 5000;
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 5000;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 10000;
constexpr uint32_t HTTP_TIMEOUT_MS = 2000;
constexpr uint32_t BUZZER_BEEP_MS = 200;
constexpr uint32_t LCD_MESSAGE_MS = 2000;
constexpr uint32_t LCD_MIN_UPDATE_MS = 100;
constexpr uint32_t LCD_FORCE_REFRESH_MS = 5000;
constexpr size_t OFFLINE_QUEUE_MAX = 200;
constexpr size_t OFFLINE_SYNC_BATCH = 50;
constexpr uint16_t NETWORK_QUEUE_LENGTH = 24;
constexpr uint16_t RESULT_QUEUE_LENGTH = 24;
constexpr uint16_t NETWORK_TASK_STACK = 12288;
constexpr UBaseType_t NETWORK_TASK_PRIORITY = 1;
constexpr BaseType_t NETWORK_TASK_CORE = 0;
