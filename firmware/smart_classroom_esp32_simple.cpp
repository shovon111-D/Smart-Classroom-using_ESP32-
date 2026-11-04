/*
 * SMART CLASSROOM - SIMPLIFIED ESP32 FIRMWARE
 *
 * Included hardware:
 *   - MFRC522 RFID reader
 *   - Servo door lock
 *   - I2C LCD (16x2)
 *   - Buzzer
 *   - 4x4 keypad with local lock/unlock access codes
 *   - Flame sensor (digital D0)
 *   - MQ-2 smoke/gas sensor (analog AO and digital D0)
 *
 * Preserved software behavior from smart_classroom_esp32 (1).cpp:
 *   - RFID access check through the backend
 *   - Local cached RFID access when the internet is unavailable
 *   - Offline event queue and later synchronization
 *   - Timed door unlock after an accepted RFID card
 *   - Remote lock/unlock commands and registration mode from heartbeat
 *   - Flame/smoke alarm confirmation, alarm reporting, and automatic door unlock
 *   - Door relocks after the alarm has remained clear
 *   - LCD address detection and OTA support
 *
 * Required Arduino libraries:
 *   MFRC522, Keypad, ESP32Servo, LiquidCrystal_I2C, ArduinoJson (v7)
 *   ESP32 core libraries: WiFi, HTTPClient, ArduinoOTA, LittleFS
 *
 * IMPORTANT:
 *   1. Copy include/secrets.example.h to include/secrets.h and set Wi-Fi,
 *      API URL, keypad codes, and OTA password.
 *   2. Set the flame sensor's digital comparator and calibrate MQ2_ALARM_THRESHOLD.
 *   3. MQ-2 sensors need a proper initial burn-in and warm-up. The default
 *      warm-up delay below is only a startup delay, not a substitute for
 *      the manufacturer's conditioning/burn-in instructions.
 *   4. Do not connect a 5 V sensor analog output directly to an ESP32 pin.
 *      Use a voltage divider/level protection so AO never exceeds 3.3 V.
 *   5. This is a student prototype, not a certified fire or life-safety system.
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ESP32Servo.h>
#include <HTTPClient.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <LittleFS.h>
#include <MFRC522.h>
#include <Preferences.h>
#include <SPI.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_timer.h>
#include <vector>
#include "include/config.h"
#if defined(__has_include)
#if __has_include("include/secrets.h")
#include "include/secrets.h"
#endif
#endif

// ============================== SECRETS ==============================
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef API_BASE_URL
#define API_BASE_URL "http://YOUR_BACKEND_HOST:8000"
#endif
#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""
#endif
#ifndef KEYPAD_UNLOCK_CODE
#define KEYPAD_UNLOCK_CODE ""
#endif
#ifndef KEYPAD_LOCK_CODE
#define KEYPAD_LOCK_CODE ""
#endif

// MQ-2 AO normally rises as smoke/gas concentration rises.
// This is a raw ADC threshold. Calibrate it using your actual module and environment.
constexpr uint8_t RFID_UID_MAX_BYTES = 10;
constexpr uint8_t UID_HEX_LENGTH = RFID_UID_MAX_BYTES * 2;
constexpr uint8_t API_NAME_LENGTH = 48;
constexpr uint8_t API_DIRECTION_LENGTH = 8;

// ============================== OBJECTS / SHARED STATE ==============================
enum class DoorState : uint8_t { Locked, Timed, Latched, Fire };
enum class RequestKind : uint8_t { Rfid, Keypad, Alert };
enum class ResultKind : uint8_t { Rfid, RemoteCommand, Registration, QueueWarning, QueueError };

struct NetworkRequest {
  RequestKind kind;
  char uid[UID_HEX_LENGTH + 1];
  char keypadType[16];
  char action[8];
  char alertEvent[8];
  uint16_t mq2Value;
  uint64_t eventMs;
};

struct MainResult {
  ResultKind kind;
  char result[24];
  char name[API_NAME_LENGTH];
  char direction[API_DIRECTION_LENGTH];
  char action[8];
};

char keypadMap[KEYPAD_ROWS][KEYPAD_COLS] = {
  {'1', '2', '3', 'A'}, {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'}, {'*', '0', '#', 'D'}
};

MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
Keypad keypad = Keypad(makeKeymap(keypadMap),
                       const_cast<byte*>(KEYPAD_ROW_PINS), const_cast<byte*>(KEYPAD_COL_PINS),
                       KEYPAD_ROWS, KEYPAD_COLS);
Servo doorServo;
LiquidCrystal_I2C* lcd = nullptr;

QueueHandle_t networkQueue = nullptr;
QueueHandle_t resultQueue = nullptr;
SemaphoreHandle_t storageMutex = nullptr;

bool backendOk = false;
volatile bool registrationActive = false;
bool cacheFileAvailable = false;
bool filesystemReady = false;
bool keypadCodesReady = false;
bool otaEnabled = false;

DoorState doorState = DoorState::Locked;
bool alarmActive = false;
uint16_t flameValue = 0;
uint16_t mq2Value = 0;
uint16_t peakMq2Value = 0;
uint16_t lastSentMq2Peak = 0;
portMUX_TYPE sharedStateMux = portMUX_INITIALIZER_UNLOCKED;

uint64_t bootAtMs = 0;
uint64_t lastSensorSampleMs = 0;
uint64_t lastSensorLogMs = 0;
uint64_t alarmStartedMs = 0;
uint64_t sensorsClearSinceMs = 0;
uint64_t doorRelockAtMs = 0;
uint64_t buzzerUntilMs = 0;
uint64_t messageUntilMs = 0;
uint64_t lastAlertUpdateMs = 0;
uint64_t lastLcdDrawMs = 0;
uint64_t lastKeyAtMs = 0;
String keypadBuffer;

String displayMessage = "Ready";
String displayDetail = "LOCKED";
String lcdLastLine1;
String lcdLastLine2;

struct SharedDeviceState {
  DoorState doorState;
  bool alarmActive;
  uint16_t mq2Value;
};

// Network-task-only data.
Preferences preferences;
uint64_t bootCount = 0;
uint64_t nextSequence = 0;
uint64_t lastHeartbeatMs = 0;
uint64_t lastWifiAttemptMs = 0;
uint32_t cachedUsersVersion = UINT32_MAX;
String pendingAlertEvent;
uint16_t pendingAlertMq2 = 0;
std::vector<String> cachedUids;

// ============================== GENERAL HELPERS ==============================
uint64_t nowMs() {
  return static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
}

void setBackendOk(bool value) {
  portENTER_CRITICAL(&sharedStateMux);
  backendOk = value;
  portEXIT_CRITICAL(&sharedStateMux);
}

bool isBackendOk() {
  portENTER_CRITICAL(&sharedStateMux);
  const bool value = backendOk;
  portEXIT_CRITICAL(&sharedStateMux);
  return value;
}

SharedDeviceState sharedDeviceState() {
  portENTER_CRITICAL(&sharedStateMux);
  const SharedDeviceState state{doorState, alarmActive, mq2Value};
  portEXIT_CRITICAL(&sharedStateMux);
  return state;
}

const char* doorStateName(DoorState state) {
  switch (state) {
    case DoorState::Timed: return "UNLOCKED_TIMED";
    case DoorState::Latched: return "UNLOCKED_LATCHED";
    case DoorState::Fire: return "FIRE_UNLOCKED";
    default: return "LOCKED";
  }
}

void showMessage(const String& line1, const String& line2 = "", uint32_t durationMs = LCD_MESSAGE_MS) {
  displayMessage = line1;
  displayDetail = line2;
  messageUntilMs = nowMs() + durationMs;
}

void applyDoorState(DoorState nextState) {
  portENTER_CRITICAL(&sharedStateMux);
  doorState = nextState;
  portEXIT_CRITICAL(&sharedStateMux);
  if (nextState == DoorState::Locked) {
    doorServo.write(SERVO_LOCKED_DEGREES);
    doorRelockAtMs = 0;
  } else {
    doorServo.write(SERVO_UNLOCKED_DEGREES);
    if (nextState != DoorState::Timed) doorRelockAtMs = 0;
  }
}

void shortBeep() {
  buzzerUntilMs = nowMs() + BUZZER_BEEP_MS;
}

void unlockFromRfid(const char* personName, const char* direction, bool usedCache) {
  if (alarmActive || doorState == DoorState::Fire) return;

  if (doorState == DoorState::Locked || doorState == DoorState::Timed) {
    applyDoorState(DoorState::Timed);
    doorRelockAtMs = nowMs() + DOOR_RELOCK_DELAY_MS;
  }

  if (usedCache) {
    showMessage("Access Granted", "OFFLINE CACHE");
  } else {
    String welcome = String("Welcome ") + personName;
    if (welcome.length() > LCD_COLUMNS) welcome.remove(LCD_COLUMNS);
    showMessage(welcome, direction);
  }
}

void enqueueNetworkRequest(const NetworkRequest& request) {
  if (!networkQueue || xQueueSend(networkQueue, &request, 0) != pdTRUE) {
    showMessage("Network busy", "Please retry");
  }
}

// ============================== RFID ==============================
bool makeUid(char* output, size_t capacity) {
  const uint8_t uidSize = rfid.uid.size;
  if (uidSize == 0 || uidSize > RFID_UID_MAX_BYTES || capacity < uidSize * 2 + 1) return false;

  static const char hexDigits[] = "0123456789ABCDEF";
  for (uint8_t i = 0; i < uidSize; ++i) {
    const uint8_t value = rfid.uid.uidByte[i];
    output[i * 2] = hexDigits[value >> 4];
    output[i * 2 + 1] = hexDigits[value & 0x0F];
  }
  output[uidSize * 2] = '\0';
  return true;
}

void scanRfid() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  NetworkRequest request{};
  request.kind = RequestKind::Rfid;
  request.eventMs = nowMs();
  if (makeUid(request.uid, sizeof(request.uid))) enqueueNetworkRequest(request);

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

void queueKeypadEvent(const char* type, const char* action = "") {
  NetworkRequest request{};
  request.kind = RequestKind::Keypad;
  request.eventMs = nowMs();
  strlcpy(request.keypadType, type, sizeof(request.keypadType));
  strlcpy(request.action, action, sizeof(request.action));
  enqueueNetworkRequest(request);
}

void handleKeypadCode() {
  const bool unlock = keypadBuffer == KEYPAD_UNLOCK_CODE;
  const bool lock = keypadBuffer == KEYPAD_LOCK_CODE;
  keypadBuffer = "";

  if (!unlock && !lock) {
    queueKeypadEvent("wrong_code");
    shortBeep();
    showMessage("Access denied", "Wrong keypad code");
    return;
  }

  const char* action = unlock ? "unlock" : "lock";
  queueKeypadEvent("success", action);
  if (alarmActive) {
    showMessage("Fire alarm", "Exit stays open");
    return;
  }

  if (unlock) {
    applyDoorState(DoorState::Latched);
    showMessage("Keypad unlock", "DOOR OPEN");
  } else {
    applyDoorState(DoorState::Locked);
    showMessage("Keypad lock", "LOCKED");
  }
}

void scanKeypad() {
  if (!keypadCodesReady) return;
  const uint64_t current = nowMs();
  if (keypadBuffer.length() && current - lastKeyAtMs >= KEYPAD_TIMEOUT_MS) {
    keypadBuffer = "";
    showMessage("Keypad timeout", "Entry cleared");
  }

  const char key = keypad.getKey();
  if (!key) return;
  lastKeyAtMs = current;

  if (key == '*') {
    keypadBuffer = "";
    showMessage("Code cleared", "Enter 4 digits");
  } else if (key >= '0' && key <= '9' && keypadBuffer.length() < KEYPAD_CODE_LENGTH) {
    keypadBuffer += key;
    if (keypadBuffer.length() == KEYPAD_CODE_LENGTH) {
      handleKeypadCode();
    } else {
      showMessage("Enter access code", String(keypadBuffer.length()) + "/4 digits");
    }
  }
}

// ============================== FLAME + MQ-2 SENSORS ==============================
uint16_t readFlameLevel() {
  // Represent the active-low digital output as a binary 12-bit intensity.
  return digitalRead(FLAME_D0_PIN) == LOW ? ADC_MAX_VALUE : 0;
}

uint16_t readMq2Level() {
  return static_cast<uint16_t>(analogRead(MQ2_AO_PIN));
}

void queueAlarmEvent(const char* eventName) {
  NetworkRequest request{};
  request.kind = RequestKind::Alert;
  request.mq2Value = mq2Value;
  strlcpy(request.alertEvent, eventName, sizeof(request.alertEvent));
  enqueueNetworkRequest(request);
}

bool flameIsHigh() {
  return flameValue > 0;
}

bool smokeIsHigh() {
  return mq2Value >= MQ2_ALARM_THRESHOLD || digitalRead(MQ2_D0_PIN) == LOW;
}

bool sensorsAreReady(uint64_t currentMs) {
  const uint32_t warmup = max(FLAME_WARMUP_MS, MQ2_WARMUP_MS);
  return currentMs - bootAtMs >= warmup;
}

bool flameSensorReady(uint64_t currentMs) {
  return currentMs - bootAtMs >= FLAME_WARMUP_MS;
}

bool mq2SensorReady(uint64_t currentMs) {
  return currentMs - bootAtMs >= MQ2_WARMUP_MS;
}

void updateSafetySensors() {
  const uint64_t current = nowMs();
  if (current - lastSensorSampleMs < SENSOR_SAMPLE_INTERVAL_MS) return;
  lastSensorSampleMs = current;

  flameValue = readFlameLevel();
  portENTER_CRITICAL(&sharedStateMux);
  mq2Value = readMq2Level();
  portEXIT_CRITICAL(&sharedStateMux);

  // Print the flame comparator state and MQ-2 analog reading for calibration.
  if (current - lastSensorLogMs >= 1000) {
    lastSensorLogMs = current;
    Serial.printf("Flame D0: %s | MQ-2 raw: %u | Alarm: %s\n",
                  flameValue ? "DETECTED" : "clear", mq2Value,
                  alarmActive ? "ON" : "OFF");
  }

  // Either a confirmed flame reading or a confirmed MQ-2 reading starts the alarm.
  const bool flameHigh = flameSensorReady(current) && flameIsHigh();
  const bool smokeHigh = mq2SensorReady(current) && smokeIsHigh();
  if (!alarmActive) {
    if (flameHigh || smokeHigh) {
      if (alarmStartedMs == 0) alarmStartedMs = current;

      if (current - alarmStartedMs >= ALARM_CONFIRM_MS) {
        portENTER_CRITICAL(&sharedStateMux);
        alarmActive = true;
        portEXIT_CRITICAL(&sharedStateMux);
        peakMq2Value = mq2Value;
        lastSentMq2Peak = mq2Value;
        lastAlertUpdateMs = current;

        applyDoorState(DoorState::Fire);  // Keep the exit unlocked during an alarm.
        queueAlarmEvent("start");
        showMessage("FIRE/SMOKE ALERT", "EVACUATE NOW", LCD_MESSAGE_MS);
      }
    } else {
      alarmStartedMs = 0;
    }
    return;
  }

  // Track peak sensor readings while the alarm remains active.
  if (mq2Value > peakMq2Value) peakMq2Value = mq2Value;

  const bool newPeak = peakMq2Value > lastSentMq2Peak;
  if (newPeak && current - lastAlertUpdateMs >= ALERT_UPDATE_INTERVAL_MS) {
    queueAlarmEvent("update");
    lastSentMq2Peak = peakMq2Value;
    lastAlertUpdateMs = current;
  }

  const uint16_t mq2ClearLevel = MQ2_ALARM_THRESHOLD > MQ2_HYSTERESIS
    ? MQ2_ALARM_THRESHOLD - MQ2_HYSTERESIS : 0;

  // Clear only when BOTH sensors stay below their clear levels for the full period.
  if (flameSensorReady(current) && mq2SensorReady(current) && flameValue == 0 &&
      mq2Value < mq2ClearLevel && digitalRead(MQ2_D0_PIN) != LOW) {
    if (sensorsClearSinceMs == 0) sensorsClearSinceMs = current;

    if (current - sensorsClearSinceMs >= ALARM_CLEAR_MS) {
      portENTER_CRITICAL(&sharedStateMux);
      alarmActive = false;
      portEXIT_CRITICAL(&sharedStateMux);
      peakMq2Value = 0;
      alarmStartedMs = 0;
      sensorsClearSinceMs = 0;
      applyDoorState(DoorState::Locked);
      queueAlarmEvent("clear");
      showMessage("Alarm cleared", "DOOR LOCKED");
    }
  } else {
    sensorsClearSinceMs = 0;
  }
}

// ============================== DOOR / LCD / BUZZER ==============================
void updateDoorTimer() {
  if (doorState == DoorState::Timed && doorRelockAtMs > 0 && nowMs() >= doorRelockAtMs) {
    applyDoorState(DoorState::Locked);
    showMessage("Door relocked", "LOCKED");
  }
}

void renderOutputs() {
  const uint64_t current = nowMs();
  const bool messageVisible = current < messageUntilMs;
  String line1;
  String line2;

  if (alarmActive) {
    line1 = "FIRE/SMOKE ALERT";
    line2 = "EVACUATE NOW";
  } else if (messageVisible) {
    line1 = displayMessage;
    line2 = displayDetail;
  } else if (!sensorsAreReady(current)) {
    line1 = "Sensors warming";
    line2 = "Please wait";
  } else if (registrationActive) {
    line1 = "Registration ON";
    line2 = "Tap new card";
  } else if (!isBackendOk()) {
    line1 = "RFID Ready";
    line2 = String(doorStateName(doorState)) + " OFFLINE";
  } else {
    line1 = "RFID Ready";
    line2 = doorStateName(doorState);
  }

  if (line1.length() > LCD_COLUMNS) line1.remove(LCD_COLUMNS);
  if (line2.length() > LCD_COLUMNS) line2.remove(LCD_COLUMNS);
  while (line1.length() < LCD_COLUMNS) line1 += ' ';
  while (line2.length() < LCD_COLUMNS) line2 += ' ';

  const bool textChanged = line1 != lcdLastLine1 || line2 != lcdLastLine2;
  const bool refreshNeeded = current - lastLcdDrawMs >= LCD_FORCE_REFRESH_MS;
  if (lcd && ((textChanged && current - lastLcdDrawMs >= LCD_MIN_UPDATE_MS) || refreshNeeded)) {
    lcd->setCursor(0, 0);
    lcd->print(line1);
    lcd->setCursor(0, 1);
    lcd->print(line2);
    lcdLastLine1 = line1;
    lcdLastLine2 = line2;
    lastLcdDrawMs = current;
  }

  // Continuous buzzer during an alarm; short beep for an access denial.
  digitalWrite(BUZZER_PIN, alarmActive || current < buzzerUntilMs ? HIGH : LOW);
}

void receiveNetworkResults() {
  MainResult result{};
  while (resultQueue && xQueueReceive(resultQueue, &result, 0) == pdTRUE) {
    if (result.kind == ResultKind::Rfid) {
      if (strcmp(result.result, "granted") == 0) {
        unlockFromRfid(result.name, result.direction, false);
      } else if (strcmp(result.result, "cached") == 0) {
        unlockFromRfid("", "", true);
      } else if (strcmp(result.result, "registered") == 0) {
        showMessage(String("Registered: ") + result.name, "Welcome");
      } else if (strcmp(result.result, "already_registered") == 0) {
        showMessage("Already", "registered");
      } else if (strcmp(result.result, "offline_no_cache") == 0) {
        shortBeep();
        showMessage("Offline", "No access cache");
      } else if (strcmp(result.result, "backend_error") == 0) {
        showMessage("Backend error", "Card not checked");
      } else {
        shortBeep();
        showMessage("Access denied", "Unknown card");
      }
    } else if (result.kind == ResultKind::RemoteCommand) {
      if (alarmActive) continue;  // Remote commands cannot relock the exit during an alarm.
      if (strcmp(result.action, "unlock") == 0) {
        applyDoorState(DoorState::Latched);
        showMessage("Remote unlock", "DOOR OPEN");
      } else if (strcmp(result.action, "lock") == 0) {
        applyDoorState(DoorState::Locked);
        showMessage("Remote lock", "LOCKED");
      }
    } else if (result.kind == ResultKind::Registration) {
      registrationActive = strcmp(result.result, "active") == 0;
    } else if (result.kind == ResultKind::QueueWarning) {
      showMessage("Offline queue", "Oldest event lost");
    } else if (result.kind == ResultKind::QueueError) {
      showMessage("Queue save failed", "Event not stored");
    }
  }
}

// ============================== NETWORK / BACKEND HELPERS ==============================
String makeApiUrl(const char* path) {
  String base = API_BASE_URL;
  while (base.endsWith("/")) base.remove(base.length() - 1);
  return base + path;
}

int sendHttpRequest(const char* method, const char* path, const String& body, String& responseBody) {
  if (WiFi.status() != WL_CONNECTED) {
    setBackendOk(false);
    return -1;
  }

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(makeApiUrl(path))) {
    setBackendOk(false);
    return -1;
  }

  http.addHeader("Content-Type", "application/json");
  const int status = strcmp(method, "GET") == 0 ? http.GET() : http.POST(body);
  setBackendOk(status > 0 && status < 500);
  if (status > 0) responseBody = http.getString();
  http.end();
  return status;
}

bool getJson(const char* path, JsonDocument& responseDocument) {
  String response;
  const int status = sendHttpRequest("GET", path, "", response);
  if (status < 200 || status >= 300) return false;
  return deserializeJson(responseDocument, response) == DeserializationError::Ok;
}

bool replaceFileAtomically(const char* temporaryPath, const char* destinationPath,
                           const char* backupPath) {
  const bool hadDestination = LittleFS.exists(destinationPath);
  if (hadDestination) {
    LittleFS.remove(backupPath);
    if (!LittleFS.rename(destinationPath, backupPath)) return false;
  }
  if (!LittleFS.rename(temporaryPath, destinationPath)) {
    if (hadDestination) LittleFS.rename(backupPath, destinationPath);
    return false;
  }
  if (hadDestination) LittleFS.remove(backupPath);
  return true;
}

bool postJson(const char* path, JsonDocument& payload, JsonDocument* responseDocument = nullptr) {
  String body;
  serializeJson(payload, body);
  String response;
  const int status = sendHttpRequest("POST", path, body, response);
  if (status < 200 || status >= 300) return false;

  if (responseDocument && response.length() > 0) {
    return deserializeJson(*responseDocument, response) == DeserializationError::Ok;
  }
  return true;
}

bool uidIsCached(const char* uid) {
  for (const String& cachedUid : cachedUids) {
    if (cachedUid == uid) return true;
  }
  return false;
}

// ============================== LOCAL UID CACHE ==============================
void loadUidCache() {
  cachedUids.clear();
  cacheFileAvailable = false;
  if (!filesystemReady || !LittleFS.exists("/uids.json")) return;

  File file = LittleFS.open("/uids.json", FILE_READ);
  if (!file) return;

  JsonDocument document;
  if (deserializeJson(document, file) == DeserializationError::Ok) {
    cachedUsersVersion = document["version"] | UINT32_MAX;
    for (JsonVariantConst item : document["uids"].as<JsonArrayConst>()) {
      cachedUids.emplace_back(item.as<const char*>());
    }
    cacheFileAvailable = true;
  }
  file.close();
}

void saveUidCache(uint32_t version, JsonArrayConst uidArray) {
  if (!filesystemReady || xSemaphoreTake(storageMutex, pdMS_TO_TICKS(HTTP_TIMEOUT_MS)) != pdTRUE) return;

  std::vector<String> updatedUids;
  JsonDocument document;
  document["version"] = version;
  JsonArray values = document["uids"].to<JsonArray>();
  for (JsonVariantConst item : uidArray) {
    const char* uid = item.as<const char*>();
    if (!uid) continue;
    values.add(uid);
    updatedUids.emplace_back(uid);
  }

  File file = LittleFS.open("/uids.tmp", FILE_WRITE);
  if (file) {
    const size_t bytesWritten = serializeJson(document, file);
    file.close();
    if (bytesWritten > 0 &&
        replaceFileAtomically("/uids.tmp", "/uids.json", "/uids.bak")) {
      cachedUids.swap(updatedUids);
      cachedUsersVersion = version;
      cacheFileAvailable = true;
    } else {
      Serial.println("Unable to persist RFID cache; keeping previous cache");
    }
  } else {
    Serial.println("Unable to create RFID cache file");
  }
  xSemaphoreGive(storageMutex);
}

// ============================== OFFLINE EVENT QUEUE ==============================
std::vector<String> readOfflineQueue() {
  std::vector<String> lines;
  if (!filesystemReady || !LittleFS.exists("/queue.jsonl")) return lines;

  File file = LittleFS.open("/queue.jsonl", FILE_READ);
  if (!file) return lines;

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length()) lines.push_back(line);
  }
  file.close();
  return lines;
}

bool rewriteOfflineQueue(const std::vector<String>& lines) {
  if (!filesystemReady) return false;

  File file = LittleFS.open("/queue.tmp", FILE_WRITE);
  if (!file) return false;
  bool writeOk = true;
  for (const String& line : lines) {
    if (file.print(line) != line.length() || file.print('\n') != 1) {
      writeOk = false;
      break;
    }
  }
  file.flush();
  writeOk = writeOk && (lines.empty() ? file.size() == 0 : file.size() > 0);
  file.close();
  if (!writeOk) {
    LittleFS.remove("/queue.tmp");
    return false;
  }

  return replaceFileAtomically("/queue.tmp", "/queue.jsonl", "/queue.bak");
}

bool saveOfflineEvent(const NetworkRequest& request) {
  if (!filesystemReady || xSemaphoreTake(storageMutex, pdMS_TO_TICKS(HTTP_TIMEOUT_MS)) != pdTRUE) {
    return false;
  }

  std::vector<String> lines = readOfflineQueue();
  bool queueOverflow = false;
  while (lines.size() >= OFFLINE_QUEUE_MAX) {
    lines.erase(lines.begin());
    queueOverflow = true;
  }

  JsonDocument event;
  event["seq"] = ++nextSequence;
  if (request.kind == RequestKind::Rfid) {
    event["kind"] = "rfid";
    event["uid"] = request.uid;
  } else {
    event["kind"] = "keypad";
    event["type"] = request.keypadType;
    if (strcmp(request.keypadType, "success") == 0) event["action"] = request.action;
  }
  event["boot"] = bootCount;
  event["t_ms"] = request.eventMs;

  String serializedEvent;
  serializeJson(event, serializedEvent);
  lines.push_back(serializedEvent);
  const bool saved = rewriteOfflineQueue(lines);
  const bool sequenceSaved = saved && preferences.putULong64("next_seq", nextSequence) == sizeof(nextSequence);
  if (!sequenceSaved) {
    Serial.println("Unable to persist offline event or its sequence");
    if (!saved) --nextSequence;
  }

  if (sequenceSaved && queueOverflow) {
    MainResult warning{};
    warning.kind = ResultKind::QueueWarning;
    xQueueSend(resultQueue, &warning, 0);
  }
  xSemaphoreGive(storageMutex);
  return sequenceSaved;
}

bool syncOfflineQueue() {
  if (!filesystemReady) return false;

  while (true) {
    std::vector<String> lines = readOfflineQueue();
    if (lines.empty()) return true;

    JsonDocument payload;
    payload["boot"] = bootCount;
    payload["now_ms"] = nowMs();
    JsonArray events = payload["events"].to<JsonArray>();

    const size_t batchSize = min(static_cast<size_t>(OFFLINE_SYNC_BATCH), lines.size());
    uint64_t lastSequenceInBatch = 0;
    for (size_t i = 0; i < batchSize; ++i) {
      JsonDocument event;
      if (deserializeJson(event, lines[i]) != DeserializationError::Ok) {
        Serial.println("Offline queue contains invalid JSON; sync paused");
        return false;
      }
      lastSequenceInBatch = max(lastSequenceInBatch, event["seq"].as<uint64_t>());
      events.add(event);
    }

    JsonDocument response;
    if (!postJson("/api/offline-sync", payload, &response)) return false;
    const uint64_t acknowledgedSequence = response["last_seq"] | 0ULL;
    if (acknowledgedSequence == 0 || acknowledgedSequence < lastSequenceInBatch) return false;

    std::vector<String> remaining;
    for (const String& line : lines) {
      JsonDocument event;
      if (deserializeJson(event, line) != DeserializationError::Ok) {
        Serial.println("Offline queue contains invalid JSON; entries retained");
        return false;
      }
      if (event["seq"].as<uint64_t>() > acknowledgedSequence) {
        remaining.push_back(line);
      }
    }

    if (xSemaphoreTake(storageMutex, pdMS_TO_TICKS(HTTP_TIMEOUT_MS)) != pdTRUE) return false;
    const bool saved = rewriteOfflineQueue(remaining);
    xSemaphoreGive(storageMutex);
    if (!saved) return false;
  }
}

// ============================== BACKEND OPERATIONS ==============================
void reportAlarmToBackend(const char* eventName, uint16_t smokeReading) {
  JsonDocument payload;
  payload["event"] = eventName;
  payload["mq2_value"] = smokeReading;

  if (!postJson("/api/alert", payload)) {
    pendingAlertEvent = eventName;
    pendingAlertMq2 = smokeReading;
  } else {
    pendingAlertEvent = "";
  }
}

void processRfidRequest(const NetworkRequest& request) {
  const bool offlineQueueEmpty = readOfflineQueue().empty();

  if (isBackendOk() && offlineQueueEmpty && WiFi.status() == WL_CONNECTED) {
    JsonDocument payload;
    payload["uid"] = request.uid;
    String body;
    serializeJson(payload, body);
    String responseBody;
    const int status = sendHttpRequest("POST", "/api/rfid/scan", body, responseBody);

    if (status > 0 && status < 500) {
      JsonDocument response;
      MainResult result{};
      result.kind = ResultKind::Rfid;
      if (deserializeJson(response, responseBody) == DeserializationError::Ok &&
          response["result"].is<const char*>()) {
        strlcpy(result.name, response["name"] | "", sizeof(result.name));
        strlcpy(result.direction, response["direction"] | "", sizeof(result.direction));
        strlcpy(result.result, response["result"] | "denied", sizeof(result.result));
      } else {
        strlcpy(result.result, "backend_error", sizeof(result.result));
      }
      xQueueSend(resultQueue, &result, 0);
      return;
    }
  }

  if (!saveOfflineEvent(request)) {
    MainResult warning{};
    warning.kind = ResultKind::QueueError;
    xQueueSend(resultQueue, &warning, 0);
  }
  MainResult result{};
  result.kind = ResultKind::Rfid;
  const char* accessResult = !cacheFileAvailable ? "offline_no_cache" :
                             uidIsCached(request.uid) ? "cached" : "denied";
  strlcpy(result.result, accessResult, sizeof(result.result));
  xQueueSend(resultQueue, &result, 0);
}

void processKeypadRequest(const NetworkRequest& request) {
  const bool offlineQueueEmpty = readOfflineQueue().empty();
  if (isBackendOk() && offlineQueueEmpty && WiFi.status() == WL_CONNECTED) {
    JsonDocument payload;
    payload["type"] = request.keypadType;
    if (request.keypadType[0] == 's') payload["action"] = request.action;
    if (postJson("/api/keypad-event", payload)) return;
  }

  if (!saveOfflineEvent(request)) {
    MainResult warning{};
    warning.kind = ResultKind::QueueError;
    xQueueSend(resultQueue, &warning, 0);
  }
}

void refreshUidCache(uint32_t requestedVersion) {
  if (requestedVersion == cachedUsersVersion) return;

  JsonDocument response;
  if (getJson("/api/uids", response)) {
    const uint32_t version = response["version"] | requestedVersion;
    saveUidCache(version, response["uids"].as<JsonArrayConst>());
  }
}

void sendHeartbeat() {
  const SharedDeviceState state = sharedDeviceState();
  JsonDocument payload;
  payload["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -127;
  payload["door_state"] = doorStateName(state.doorState);
  payload["fire_active"] = state.alarmActive;
  payload["mq2_value"] = state.mq2Value;

  JsonDocument response;
  if (!postJson("/api/heartbeat", payload, &response)) return;

  const char* command = response["command"] | "";
  if (strcmp(command, "lock") == 0 || strcmp(command, "unlock") == 0) {
    MainResult commandResult{};
    commandResult.kind = ResultKind::RemoteCommand;
    strlcpy(commandResult.action, command, sizeof(commandResult.action));
    xQueueSend(resultQueue, &commandResult, 0);
  }

  MainResult registrationResult{};
  registrationResult.kind = ResultKind::Registration;
  strlcpy(registrationResult.result,
          (response["registration_active"] | false) ? "active" : "inactive",
          sizeof(registrationResult.result));
  xQueueSend(resultQueue, &registrationResult, 0);

  refreshUidCache(response["users_version"] | cachedUsersVersion);
  syncOfflineQueue();

  if (pendingAlertEvent.length()) {
    reportAlarmToBackend(pendingAlertEvent.c_str(), pendingAlertMq2);
  }
}

void initializeNetworkStorage() {
  preferences.begin("classroom", false);
  bootCount = preferences.getULong64("boot", 0) + 1;
  nextSequence = preferences.getULong64("next_seq", 0);
  preferences.putULong64("boot", bootCount);

  filesystemReady = LittleFS.begin(false);
  if (!filesystemReady) {
    Serial.println("LittleFS unavailable; offline queue/cache disabled");
  } else {
    if (!LittleFS.exists("/uids.json") && LittleFS.exists("/uids.bak")) {
      LittleFS.rename("/uids.bak", "/uids.json");
    }
    if (!LittleFS.exists("/queue.jsonl") && LittleFS.exists("/queue.bak")) {
      LittleFS.rename("/queue.bak", "/queue.jsonl");
    }
  }
  loadUidCache();
}

void networkTask(void*) {
  initializeNetworkStorage();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  if (strlen(OTA_PASSWORD) > 0) {
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.begin();
    otaEnabled = true;
  } else {
    Serial.println("OTA disabled because OTA_PASSWORD is empty");
  }

  lastWifiAttemptMs = nowMs();
  lastHeartbeatMs = nowMs();
  NetworkRequest request{};

  while (true) {
    const uint64_t current = nowMs();

    if (WiFi.status() != WL_CONNECTED) {
      setBackendOk(false);
      if (current - lastWifiAttemptMs >= WIFI_RETRY_INTERVAL_MS) {
        lastWifiAttemptMs = current;
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      }
    } else if (otaEnabled) {
      ArduinoOTA.handle();
    }

    if (current - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
      lastHeartbeatMs = current;
      sendHeartbeat();
    }

    if (xQueueReceive(networkQueue, &request, pdMS_TO_TICKS(25)) == pdTRUE) {
      if (request.kind == RequestKind::Rfid) {
        processRfidRequest(request);
      } else if (request.kind == RequestKind::Keypad) {
        processKeypadRequest(request);
      } else {
        reportAlarmToBackend(request.alertEvent, request.mq2Value);
      }
    }
  }
}

// ============================== SETUP ==============================
void setupLcd() {
  Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
  Wire.setClock(100000);
  delay(100);

  const uint8_t candidates[] = {LCD_ADDRESS, 0x27, 0x3F, 0x20, 0x38};
  uint8_t detectedAddress = LCD_ADDRESS;
  bool found = false;

  for (uint8_t address : candidates) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      detectedAddress = address;
      found = true;
      break;
    }
  }

  Serial.printf("LCD %s at 0x%02X\n", found ? "found" : "not detected; trying", detectedAddress);
  lcd = new LiquidCrystal_I2C(detectedAddress, LCD_COLUMNS, LCD_ROWS);
  lcd->init();
  lcd->backlight();
  lcd->clear();
  lcd->setCursor(0, 0);
  lcd->print("Starting system");
  lcd->setCursor(0, 1);
  lcd->print("Sensors warming");
}

void setup() {
  Serial.begin(115200);
  bootAtMs = nowMs();

  analogReadResolution(ADC_RESOLUTION_BITS);
  analogSetPinAttenuation(MQ2_AO_PIN, ADC_11db);
  pinMode(FLAME_D0_PIN, INPUT);
  pinMode(MQ2_D0_PIN, INPUT);
  pinMode(MQ2_AO_PIN, INPUT);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  keypadCodesReady = strlen(KEYPAD_UNLOCK_CODE) == KEYPAD_CODE_LENGTH &&
                     strlen(KEYPAD_LOCK_CODE) == KEYPAD_CODE_LENGTH &&
                     strcmp(KEYPAD_UNLOCK_CODE, KEYPAD_LOCK_CODE) != 0;
  keypad.setDebounceTime(50);
  if (!keypadCodesReady) Serial.println("Keypad disabled; set distinct four-digit codes in secrets.h");

  setupLcd();
  if (!keypadCodesReady) showMessage("Set keypad codes", "in secrets.h");

  doorServo.setPeriodHertz(50);
  doorServo.attach(SERVO_PIN, 500, 2400);
  doorServo.write(SERVO_LOCKED_DEGREES);

  SPI.begin(RFID_SCK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN, RFID_SS_PIN);
  rfid.PCD_Init();

  networkQueue = xQueueCreate(NETWORK_QUEUE_LENGTH, sizeof(NetworkRequest));
  resultQueue = xQueueCreate(RESULT_QUEUE_LENGTH, sizeof(MainResult));
  storageMutex = xSemaphoreCreateMutex();

  if (networkQueue && resultQueue && storageMutex) {
    xTaskCreatePinnedToCore(networkTask, "networkTask", NETWORK_TASK_STACK, nullptr,
                            NETWORK_TASK_PRIORITY, nullptr, NETWORK_TASK_CORE);
  } else {
    Serial.println("Could not create network queues/mutex; backend features unavailable");
    showMessage("Network error", "Check restart");
  }
}

// ============================== MAIN LOOP ==============================
void loop() {
  updateSafetySensors();
  scanRfid();
  scanKeypad();
  updateDoorTimer();
  receiveNetworkResults();
  renderOutputs();
}