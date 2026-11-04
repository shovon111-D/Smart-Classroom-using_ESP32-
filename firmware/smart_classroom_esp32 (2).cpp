/*
  Smart Classroom - ESP32 (MERGED: RFID + Keypad + Flame + MQ-2 smoke graph)

  Components:
  1. I2C LCD 1602
  2. Buzzer
  3. Servo Motor (door lock)
  4. MFRC522 RFID
  5. 4x4 Keypad
  6. MQ-2 Gas / Smoke Sensor
  7. Flame Sensor

  Serial Monitor: 115200 baud

  What this program does:
    - RFID card -> asks the backend if the card is allowed -> opens door for 5 sec
    - No internet? -> uses the saved (cached) card list and stores the event
      in a file. The events are sent to the backend later.
    - Keypad: type UNLOCK_CODE to open the door, LOCK_CODE to lock it
        *  = clear what you typed
    - Flame or smoke detected for 1 sec -> ALARM
        buzzer ON, door unlocks (so people can escape), backend is told
    - Alarm gone for 5 sec -> alarm stops, door locks again
    - Every 5 sec the ESP32 sends a "heartbeat" to the backend. The backend
      can answer with: lock / unlock command, registration mode, new card list
    - OTA (wireless) upload if OTA_PASSWORD is set

  What was fixed in this merged version:
    - RFID card check ALWAYS asks the backend first when WiFi is connected.
      (Before, it was blocked while the offline queue was not empty or until
       the first heartbeat worked, so every card went to the offline path.)
    - RFID request timeout is 5 sec (Supabase can be slow; 2 sec was too short).
    - Card list is downloaded even if the backend sends no "users_version".
    - RC522 health check: prints the chip version and restarts the reader
      if it stops answering (wiring / power glitch).
    - MQ-2 value is averaged (8 readings) and sent in every heartbeat,
      so the dashboard graph is smooth and never stops.
    - Heartbeat is sent as soon as WiFi connects (graph starts quickly).
    - OTA starts after WiFi is connected. LittleFS formats itself if needed.

  Libraries to install (Arduino Library Manager):
    MFRC522, Keypad, ESP32Servo, LiquidCrystal_I2C, ArduinoJson (version 7)
  (WiFi, HTTPClient, ArduinoOTA, LittleFS, Preferences come with the ESP32 core)

  IMPORTANT:
    1. Fill in the "YOUR SETTINGS" section below (WiFi, backend address, codes).
    2. Calibrate GAS_ALERT using your real sensor (watch the Serial Monitor).
    3. MQ-2 needs a proper warm-up / burn-in. The 30 sec delay here is only
       a start-up delay, it does not replace the manufacturer's instructions.
    4. Never connect a 5V sensor analog output directly to an ESP32 pin.
       Use a voltage divider so AO never goes above 3.3V.
    5. This is a student prototype, NOT a certified fire/life-safety system.

  RFID connections:
    SDA  = GPIO 5
    SCK  = GPIO 18
    MOSI = GPIO 23
    MISO = GPIO 19
    RST  = 3.3V (no GPIO connection)
*/

// ---------- Libraries ----------

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <vector>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <MFRC522.h>
#include <ESP32Servo.h>

// ---------- YOUR SETTINGS (this replaces secrets.h) ----------

#define WIFI_SSID        "Jahangir"
#define WIFI_PASSWORD    "JAHANGIR##"
#define API_BASE_URL     "http://192.168.0.129:8000"
#define UNLOCK_CODE      "1234"   // must be exactly 4 digits
#define LOCK_CODE        "4321"   // must be exactly 4 digits and different
#define OTA_PASSWORD     ""       // leave empty to turn OTA off

// ---------- Pins (this replaces config.h) ----------

#define LCD_SDA     21
#define LCD_SCL     22

#define SPI_SCK     18
#define SPI_MISO    19
#define SPI_MOSI    23

#define RFID_SS     5

#define SERVO_PIN   16
#define BUZZER_PIN  17
#define FLAME_DO    4

// NOTE: your old config.h used these two pins. The Wokwi demo had them
// the other way around (AO = 34, DO = 35). Swap them if your wiring differs.
#define GAS_DO      35
#define GAS_AO      34

// ---------- Settings ----------

#define SERVO_LOCKED      0       // servo angle when door is locked
#define SERVO_UNLOCKED    90      // servo angle when door is open

#define GAS_ALERT         1800    // analog value that means "smoke"
#define GAS_HYSTERESIS    150     // alarm clears only below (GAS_ALERT - this)

#define FLAME_WARMUP_MS   2000
#define GAS_WARMUP_MS     30000

#define SENSOR_INTERVAL_MS  100   // read sensors every 0.1 sec
#define ALARM_CONFIRM_MS    1000  // danger must last 1 sec to start alarm
#define ALARM_CLEAR_MS      5000  // must be safe for 5 sec to stop alarm
#define ALERT_UPDATE_MS     5000  // min time between "update" alerts

#define DOOR_RELOCK_MS      5000  // door re-locks 5 sec after a card
#define HEARTBEAT_MS        5000
#define WIFI_RETRY_MS       10000
#define HTTP_CONNECT_MS     1500  // give up quickly when the backend is unreachable
#define HTTP_TIMEOUT_MS     3000  // normal requests (heartbeat, alerts, ...)
#define HTTP_SCAN_TIMEOUT_MS 5000 // RFID check (backend asks Supabase, can be slow)

#define BEEP_MS             200
#define MESSAGE_MS          2000  // how long LCD messages stay
#define LCD_REFRESH_MS      5000  // redraw LCD at least this often

#define CODE_LENGTH         4
#define KEYPAD_TIMEOUT_MS   5000

#define QUEUE_MAX           200   // max offline events stored
#define SYNC_BATCH          50    // events sent to backend at once

#define RFID_POLL_MS        50    // look for a card every 50 ms
#define RFID_HEALTH_MS      10000 // check that the RC522 still answers
#define GAS_SAMPLES         8     // average of 8 readings = smooth graph
#define GAS_USE_DIGITAL     1     // 1 = also use MQ-2 DO pin, 0 = analog (AO) only

// ---------- Objects ----------

// The LCD is created in setupLcd() because we look for its address first
LiquidCrystal_I2C *lcd = nullptr;

// RFID RST connected directly to 3.3V (-1 = no reset pin, same as the working RFID code)
MFRC522 rfid(RFID_SS, -1);

Servo servo;

Preferences preferences;   // small permanent storage (boot count, event number)

// ---------- Keypad ----------

const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

byte rowPins[ROWS] = {13, 14, 27, 26};
byte colPins[COLS] = {25, 33, 32, 15};

Keypad keypad = Keypad(
  makeKeymap(keys),
  rowPins,
  colPins,
  ROWS,
  COLS
);

// ---------- Door states ----------

#define DOOR_LOCKED   0   // locked
#define DOOR_TIMED    1   // opened by a card, will re-lock by itself
#define DOOR_LATCHED  2   // opened by keypad / remote, stays open
#define DOOR_FIRE     3   // opened because of an alarm

int doorState = DOOR_LOCKED;
unsigned long relockAt = 0;          // when a TIMED door should lock again

// ---------- Variables ----------

unsigned long bootTime = 0;

// sensors
bool flameDetected = false;
int gasValue = 0;
int peakGas = 0;
int lastSentPeak = 0;
unsigned long lastSensorTime = 0;
unsigned long lastSensorLog = 0;

// alarm
bool alarmActive = false;
unsigned long alarmStartedAt = 0;    // 0 = no danger seen right now
unsigned long clearSince = 0;        // 0 = not clear right now
unsigned long lastAlertUpdate = 0;

// LCD message + buzzer
String messageLine1 = "";
String messageLine2 = "";
unsigned long messageUntil = 0;
unsigned long buzzerUntil = 0;
String lastLine1 = "";
String lastLine2 = "";
unsigned long lastLcdDraw = 0;

// keypad
String keypadBuffer = "";
unsigned long lastKeyTime = 0;
bool keypadCodesReady = false;

// network / backend
bool backendOk = false;              // true if the last request worked
bool registrationActive = false;     // backend says "register new cards"
bool otaEnabled = false;
bool otaStarted = false;
bool wifiWasConnected = false;
bool heartbeatDue = true;            // send a heartbeat as soon as WiFi is up
unsigned long lastRfidPoll = 0;
unsigned long lastRfidHealth = 0;
unsigned long lastWifiAttempt = 0;
unsigned long lastHeartbeat = 0;

// alert that could not be sent (tried again on next heartbeat)
String pendingAlertEvent = "";
int pendingAlertGas = 0;

// storage (card cache + offline events)
bool filesystemReady = false;
bool cacheFileAvailable = false;
uint32_t cachedUsersVersion = UINT32_MAX;
std::vector<String> cachedUids;      // list of allowed card UIDs
uint64_t bootCount = 0;
uint64_t nextSequence = 0;

// ---------- Door Helpers ----------

const char* doorStateName() {

  switch (doorState) {
    case DOOR_TIMED:   return "UNLOCKED_TIMED";
    case DOOR_LATCHED: return "UNLOCKED_LATCHED";
    case DOOR_FIRE:    return "FIRE_UNLOCKED";
    default:           return "LOCKED";
  }
}

void setDoor(int newState) {

  doorState = newState;

  if (newState == DOOR_LOCKED) {

    servo.write(SERVO_LOCKED);
    relockAt = 0;

  } else {

    servo.write(SERVO_UNLOCKED);

    if (newState != DOOR_TIMED) {
      relockAt = 0;
    }
  }
}

// ---------- Sensor Ready Helpers ----------

bool flameReady() {
  return millis() - bootTime >= FLAME_WARMUP_MS;
}

bool gasReady() {
  return millis() - bootTime >= GAS_WARMUP_MS;
}

bool sensorsReady() {
  return flameReady() && gasReady();
}

// ---------- LCD Helpers ----------

// Print one line on the LCD (always exactly 16 characters)
void lcdLine(uint8_t row, String text) {

  while (text.length() < 16) {
    text += ' ';
  }

  lcd->setCursor(0, row);
  lcd->print(text.substring(0, 16));
}

// Show a message for 2 seconds (it replaces the normal screen)
void showMessage(String line1, String line2) {

  messageLine1 = line1;
  messageLine2 = line2;
  messageUntil = millis() + MESSAGE_MS;
}

// Decide what the LCD should show right now and draw it
void updateLcd() {

  String line1;
  String line2;

  if (alarmActive) {

    line1 = "FIRE/SMOKE ALERT";
    line2 = "EVACUATE NOW";

  } else if (millis() < messageUntil) {

    line1 = messageLine1;
    line2 = messageLine2;

  } else if (!sensorsReady()) {

    line1 = "Gas sensor warm";
    line2 = "RFID is ready";

  } else if (registrationActive) {

    line1 = "Registration ON";
    line2 = "Tap new card";

  } else if (!backendOk) {

    line1 = "RFID Ready";
    line2 = String(doorStateName()) + " OFFLINE";

  } else {

    line1 = "RFID Ready";
    line2 = doorStateName();
  }

  // make both lines exactly 16 characters
  while (line1.length() < 16) line1 += ' ';
  while (line2.length() < 16) line2 += ' ';
  line1 = line1.substring(0, 16);
  line2 = line2.substring(0, 16);

  // only touch the LCD if the text changed (or every 5 sec to be safe)
  bool changed = (line1 != lastLine1) || (line2 != lastLine2);
  bool refresh = (millis() - lastLcdDraw >= LCD_REFRESH_MS);

  if (changed || refresh) {

    lcdLine(0, line1);
    lcdLine(1, line2);

    lastLine1 = line1;
    lastLine2 = line2;
    lastLcdDraw = millis();
  }
}

// ---------- Buzzer Helpers ----------

void shortBeep() {
  buzzerUntil = millis() + BEEP_MS;
}

// Buzzer is ON during an alarm, or for a short time after shortBeep()
void updateBuzzer() {

  if (alarmActive || millis() < buzzerUntil) {
    digitalWrite(BUZZER_PIN, HIGH);
  } else {
    digitalWrite(BUZZER_PIN, LOW);
  }
}

// ---------- Network Helpers ----------

String apiUrl(String path) {

  String base = API_BASE_URL;

  while (base.endsWith("/")) {
    base.remove(base.length() - 1);
  }

  return base + path;
}

// Send one HTTP request. Returns the HTTP status code (or -1 on failure)
int sendHttp(String method, String path, String body, String &response, uint16_t timeoutMs) {

  response = "";

  if (WiFi.status() != WL_CONNECTED) {
    backendOk = false;
    return -1;
  }

  HTTPClient http;
  http.setConnectTimeout(HTTP_CONNECT_MS);
  http.setTimeout(timeoutMs);

  if (!http.begin(apiUrl(path))) {
    backendOk = false;
    return -1;
  }

  http.addHeader("Content-Type", "application/json");

  int status;

  if (method == "GET") {
    status = http.GET();
  } else {
    status = http.POST(body);
  }

  backendOk = (status > 0 && status < 500);

  if (status > 0) {
    response = http.getString();
  }

  http.end();

  Serial.printf("[HTTP] %s %s -> %d\n", method.c_str(), path.c_str(), status);

  return status;
}

// Send JSON to the backend. Returns true if the backend said OK (2xx).
// If responseDoc is not nullptr, the answer is stored there.
bool postJson(String path, JsonDocument &payload, JsonDocument *responseDoc) {

  String body;
  serializeJson(payload, body);

  String response;
  int status = sendHttp("POST", path, body, response, HTTP_TIMEOUT_MS);

  if (status < 200 || status >= 300) {
    return false;
  }

  if (responseDoc != nullptr && response.length() > 0) {
    return deserializeJson(*responseDoc, response) == DeserializationError::Ok;
  }

  return true;
}

// Ask the backend for JSON. Returns true if it worked.
bool getJson(String path, JsonDocument &responseDoc) {

  String response;
  int status = sendHttp("GET", path, "", response, HTTP_TIMEOUT_MS);

  if (status < 200 || status >= 300) {
    return false;
  }

  return deserializeJson(responseDoc, response) == DeserializationError::Ok;
}

// ---------- Saved Card List (works without internet) ----------

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
      cachedUids.push_back(String(item.as<const char*>()));
    }

    cacheFileAvailable = true;
  }

  file.close();
}

void saveUidCache(uint32_t version, JsonArrayConst uidArray) {

  if (!filesystemReady) return;

  std::vector<String> newList;
  JsonDocument document;

  document["version"] = version;
  JsonArray values = document["uids"].to<JsonArray>();

  for (JsonVariantConst item : uidArray) {

    const char* uid = item.as<const char*>();

    if (!uid) continue;

    values.add(uid);
    newList.push_back(String(uid));
  }

  // nothing changed -> do not wear out the flash
  if (cacheFileAvailable && version == cachedUsersVersion && newList == cachedUids) return;

  File file = LittleFS.open("/uids.json", FILE_WRITE);

  if (!file) {
    Serial.println("Unable to create RFID cache file");
    return;
  }

  size_t written = serializeJson(document, file);
  file.close();

  if (written > 0) {
    cachedUids = newList;
    cachedUsersVersion = version;
    cacheFileAvailable = true;
  }
}

bool uidIsCached(String uid) {

  for (size_t i = 0; i < cachedUids.size(); i++) {
    if (cachedUids[i] == uid) return true;
  }

  return false;
}

// Download the card list from the backend and keep a copy for offline use
void refreshUidCache(uint32_t requestedVersion) {

  JsonDocument response;

  if (!getJson("/api/uids", response)) return;

  // never replace a good list with a broken answer
  if (!response["uids"].is<JsonArray>()) return;

  uint32_t version = response["version"] | requestedVersion;
  saveUidCache(version, response["uids"].as<JsonArrayConst>());
}

// ---------- Offline Event Queue ----------
// When the backend can't be reached, events are saved one per line
// in the file /queue.jsonl and sent later.

std::vector<String> readQueue() {

  std::vector<String> lines;

  if (!filesystemReady || !LittleFS.exists("/queue.jsonl")) return lines;

  File file = LittleFS.open("/queue.jsonl", FILE_READ);

  if (!file) return lines;

  while (file.available()) {

    String line = file.readStringUntil('\n');
    line.trim();

    if (line.length() > 0) {
      lines.push_back(line);
    }
  }

  file.close();

  return lines;
}

bool writeQueue(std::vector<String> &lines) {

  if (!filesystemReady) return false;

  File file = LittleFS.open("/queue.jsonl", FILE_WRITE);

  if (!file) return false;

  for (size_t i = 0; i < lines.size(); i++) {
    file.print(lines[i]);
    file.print('\n');
  }

  file.close();

  return true;
}

bool queueIsEmpty() {

  if (!filesystemReady || !LittleFS.exists("/queue.jsonl")) return true;

  File file = LittleFS.open("/queue.jsonl", FILE_READ);

  if (!file) return true;

  bool empty = (file.size() == 0);
  file.close();

  return empty;
}

// Save one event to the queue. Returns true if saved.
bool saveOfflineEvent(JsonDocument &event) {

  if (!filesystemReady) return false;

  std::vector<String> lines = readQueue();

  // queue full -> throw away the oldest events
  bool overflow = false;

  while (lines.size() >= QUEUE_MAX) {
    lines.erase(lines.begin());
    overflow = true;
  }

  nextSequence++;

  event["seq"] = nextSequence;      // event number (backend uses it to avoid duplicates)
  event["boot"] = bootCount;        // which start-up this event belongs to
  event["t_ms"] = millis();         // time since start-up

  String text;
  serializeJson(event, text);
  lines.push_back(text);

  if (writeQueue(lines)) {

    preferences.putULong64("next_seq", nextSequence);

    if (overflow) {
      showMessage("Offline queue", "Oldest event lost");
    }

    return true;
  }

  nextSequence--;
  Serial.println("Unable to save offline event");

  return false;
}

// Send saved events to the backend. Returns true when the queue is empty.
bool syncOfflineQueue() {

  if (!filesystemReady) return false;

  while (true) {

    std::vector<String> lines = readQueue();

    if (lines.empty()) return true;

    JsonDocument payload;
    payload["boot"] = bootCount;
    payload["now_ms"] = millis();
    JsonArray events = payload["events"].to<JsonArray>();

    // take up to 50 events
    size_t batchSize = lines.size();
    if (batchSize > SYNC_BATCH) batchSize = SYNC_BATCH;

    uint64_t lastSeqInBatch = 0;

    for (size_t i = 0; i < batchSize; i++) {

      JsonDocument event;

      if (deserializeJson(event, lines[i]) != DeserializationError::Ok) {
        Serial.println("Offline queue has invalid JSON; sync paused");
        return false;
      }

      uint64_t seq = event["seq"].as<uint64_t>();
      if (seq > lastSeqInBatch) lastSeqInBatch = seq;

      events.add(event);
    }

    JsonDocument response;

    if (!postJson("/api/offline-sync", payload, &response)) return false;

    // the backend tells us the last event number it received
    uint64_t acknowledged = response["last_seq"] | 0ULL;

    if (acknowledged == 0 || acknowledged < lastSeqInBatch) return false;

    // keep only the events the backend has NOT received yet
    std::vector<String> remaining;

    for (size_t i = 0; i < lines.size(); i++) {

      JsonDocument event;

      if (deserializeJson(event, lines[i]) != DeserializationError::Ok) {
        Serial.println("Offline queue has invalid JSON; events kept");
        return false;
      }

      if (event["seq"].as<uint64_t>() > acknowledged) {
        remaining.push_back(lines[i]);
      }
    }

    if (!writeQueue(remaining)) return false;
  }
}

// ---------- Backend: Alarm Report ----------

// eventName = "start", "update" or "clear"
void reportAlarm(String eventName, int gasReading) {

  JsonDocument payload;
  payload["event"] = eventName;
  payload["mq2_value"] = gasReading;
  payload["flame_detected"] = flameDetected;

  if (postJson("/api/alert", payload, nullptr)) {

    pendingAlertEvent = "";

  } else {

    // remember it, we will try again on the next heartbeat
    pendingAlertEvent = eventName;
    pendingAlertGas = gasReading;
  }
}

// ---------- RFID Handling ----------

// Open the door for a few seconds after a good card
void unlockFromCard(String name, String direction, bool fromCache) {

  // during an alarm the door is already open
  if (alarmActive || doorState == DOOR_FIRE) return;

  shortBeep();

  if (doorState == DOOR_LOCKED || doorState == DOOR_TIMED) {
    setDoor(DOOR_TIMED);
    relockAt = millis() + DOOR_RELOCK_MS;
  }

  if (fromCache) {

    showMessage("Access Granted", "OFFLINE CACHE");

  } else {

    String welcome = "Welcome " + name;
    showMessage(welcome, direction);
  }
}

// Do the right thing for each answer
void handleRfidResult(String result, String name, String direction) {

  if (result == "granted") {

    unlockFromCard(name, direction, false);

  } else if (result == "cached") {

    unlockFromCard("", "", true);

  } else if (result == "registered") {

    showMessage("Registered: " + name, "Welcome");

  } else if (result == "already_registered") {

    showMessage("Already", "registered");

  } else if (result == "offline_no_cache") {

    shortBeep();
    showMessage("Offline", "No access cache");

  } else if (result == "backend_error") {

    showMessage("Backend error", "Card not checked");

  } else {

    shortBeep();
    showMessage("Access denied", "Unknown card");
  }
}

// A card was scanned: ask the backend first (always, when WiFi is connected).
// The saved card list is used only when the backend cannot be reached.
void handleCard(String uid) {

  Serial.println("[RFID] Card UID: " + uid);

  // 1. ONLINE: ask the backend
  if (WiFi.status() == WL_CONNECTED) {

    JsonDocument payload;
    payload["uid"] = uid;

    String body;
    serializeJson(payload, body);

    String responseText;
    int status = sendHttp("POST", "/api/rfid/scan", body, responseText, HTTP_SCAN_TIMEOUT_MS);

    if (status > 0 && status < 500) {

      JsonDocument response;

      if (deserializeJson(response, responseText) == DeserializationError::Ok &&
          response["result"].is<const char*>()) {

        handleRfidResult(
          response["result"] | "denied",
          response["name"] | "Student",
          response["direction"] | ""
        );

        return;
      }

      Serial.println("[RFID] Backend answer not understood, using saved card list");
    }
  }

  // 2. OFFLINE: cards cannot be registered without the backend
  if (registrationActive) {

    shortBeep();
    showMessage("Registration", "Wi-Fi required");
    return;
  }

  // 3. OFFLINE: save the event and use the saved card list
  JsonDocument event;
  event["kind"] = "rfid";
  event["uid"] = uid;

  if (!saveOfflineEvent(event)) {
    Serial.println("[RFID] Offline event not stored");
  }

  String result;

  if (!cacheFileAvailable) {
    result = "offline_no_cache";
  } else if (uidIsCached(uid)) {
    result = "cached";
  } else {
    result = "denied";
  }

  handleRfidResult(result, "", "");
}

// The RC522 answers 0x00 or 0xFF when it is not wired / powered correctly
bool rfidReaderAlive() {

  byte version = rfid.PCD_ReadRegister(rfid.VersionReg);

  return (version != 0x00 && version != 0xFF);
}

void initRfid() {

  rfid.PCD_Init();
  delay(50);

  byte version = rfid.PCD_ReadRegister(rfid.VersionReg);

  Serial.printf(
    "[RFID] RC522 version register: 0x%02X %s\n",
    version,
    rfidReaderAlive() ? "(OK)" : "(NOT FOUND - check wiring and 3.3V)"
  );
}

// If the reader stops answering, restart it
void checkRfidHealth() {

  unsigned long now = millis();

  if (now - lastRfidHealth < RFID_HEALTH_MS) return;

  lastRfidHealth = now;

  if (!rfidReaderAlive()) {

    Serial.println("[RFID] RC522 not answering - restarting reader");
    showMessage("RFID error", "Check wiring");
    rfid.PCD_Init();
  }
}

void checkRfid() {

  unsigned long now = millis();

  if (now - lastRfidPoll < RFID_POLL_MS) return;

  lastRfidPoll = now;

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  // turn the card number into text like "A1B2C3D4"
  String uid = "";

  for (byte i = 0; i < rfid.uid.size; i++) {

    if (rfid.uid.uidByte[i] < 0x10) {
      uid += "0";
    }

    uid += String(rfid.uid.uidByte[i], HEX);
  }

  uid.toUpperCase();

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  handleCard(uid);
}

// ---------- Keypad Handling ----------

// Tell the backend about a keypad event (or save it if offline)
// type = "success" or "wrong_code"
void sendKeypadEvent(String type, String action) {

  if (WiFi.status() == WL_CONNECTED) {

    JsonDocument payload;
    payload["type"] = type;

    if (type == "success") {
      payload["action"] = action;
    }

    if (postJson("/api/keypad-event", payload, nullptr)) return;
  }

  JsonDocument event;
  event["kind"] = "keypad";
  event["type"] = type;

  if (type == "success") {
    event["action"] = action;
  }

  if (!saveOfflineEvent(event)) {
    showMessage("Queue save failed", "Event not stored");
  }
}

// 4 digits were typed: check the code
void handleKeypadCode() {

  bool unlock = (keypadBuffer == UNLOCK_CODE);
  bool lock = (keypadBuffer == LOCK_CODE);

  keypadBuffer = "";

  if (!unlock && !lock) {

    shortBeep();
    showMessage("Access denied", "Wrong keypad code");
    sendKeypadEvent("wrong_code", "");
    return;
  }

  String action = unlock ? "unlock" : "lock";

  if (alarmActive) {

    // the exit must stay open during an alarm
    showMessage("Fire alarm", "Exit stays open");

  } else if (unlock) {

    setDoor(DOOR_LATCHED);
    showMessage("Keypad unlock", "DOOR OPEN");

  } else {

    setDoor(DOOR_LOCKED);
    showMessage("Keypad lock", "LOCKED");
  }

  // the door already moved, now tell the backend
  sendKeypadEvent("success", action);
}

void checkKeypad() {

  if (!keypadCodesReady) return;

  // forget a half-typed code after 5 sec
  if (keypadBuffer.length() > 0 && millis() - lastKeyTime >= KEYPAD_TIMEOUT_MS) {

    keypadBuffer = "";
    showMessage("Keypad timeout", "Entry cleared");
  }

  char key = keypad.getKey();

  if (!key) return;

  lastKeyTime = millis();

  if (key == '*') {

    keypadBuffer = "";
    showMessage("Code cleared", "Enter 4 digits");

  } else if (key >= '0' && key <= '9' && keypadBuffer.length() < CODE_LENGTH) {

    keypadBuffer += key;

    if (keypadBuffer.length() == CODE_LENGTH) {

      handleKeypadCode();

    } else {

      showMessage("Enter access code", String(keypadBuffer.length()) + "/4 digits");
    }
  }
}

// ---------- Flame + Gas Sensors / Alarm ----------

// Average several readings so the graph is smooth
int readGasAverage() {

  long total = 0;

  for (int i = 0; i < GAS_SAMPLES; i++) {
    total += analogRead(GAS_AO);
  }

  return (int)(total / GAS_SAMPLES);
}

void checkSensors() {

  unsigned long now = millis();

  if (now - lastSensorTime < SENSOR_INTERVAL_MS) return;

  lastSensorTime = now;

  // 1. READ SENSORS

  flameDetected = (digitalRead(FLAME_DO) == LOW);   // LOW = flame
  gasValue = readGasAverage();
  bool gasDigital = (GAS_USE_DIGITAL == 1) && (digitalRead(GAS_DO) == LOW);   // LOW = gas / smoke

  // print values once per second (helps you calibrate GAS_ALERT)
  if (now - lastSensorLog >= 1000) {

    lastSensorLog = now;

    Serial.printf(
      "Flame: %s | Gas raw: %d | Gas DO: %s | Alarm: %s | Backend: %s\n",
      flameDetected ? "DETECTED" : "clear",
      gasValue,
      gasDigital ? "GAS" : "ok",
      alarmActive ? "ON" : "OFF",
      backendOk ? "OK" : "OFFLINE"
    );
  }

  // sensors only count after their warm-up time
  bool flameDanger = flameReady() && flameDetected;
  bool smokeDanger = gasReady() && (gasValue >= GAS_ALERT || gasDigital);

  // 2. ALARM IS OFF: look for danger

  if (!alarmActive) {

    if (flameDanger || smokeDanger) {

      if (alarmStartedAt == 0) {
        alarmStartedAt = now;
      }

      // danger lasted long enough -> START ALARM
      if (now - alarmStartedAt >= ALARM_CONFIRM_MS) {

        alarmActive = true;
        peakGas = gasValue;
        lastSentPeak = gasValue;
        lastAlertUpdate = now;

        setDoor(DOOR_FIRE);               // unlock the exit
        showMessage("FIRE/SMOKE ALERT", "EVACUATE NOW");

        // buzzer and LCD first, because the network call can be slow
        updateBuzzer();
        updateLcd();

        Serial.println("!!! ALARM STARTED !!!");

        reportAlarm("start", gasValue);
      }

    } else {

      alarmStartedAt = 0;
    }

    return;
  }

  // 3. ALARM IS ON

  // remember the highest gas value
  if (gasValue > peakGas) {
    peakGas = gasValue;
  }

  // tell the backend if we reached a new peak (but not too often)
  if (peakGas > lastSentPeak && now - lastAlertUpdate >= ALERT_UPDATE_MS) {

    reportAlarm("update", gasValue);

    lastSentPeak = peakGas;
    lastAlertUpdate = now;
  }

  // gas must drop a little BELOW the alarm level before we call it "clear"
  int clearLevel = GAS_ALERT - GAS_HYSTERESIS;

  if (clearLevel < 0) clearLevel = 0;

  bool allClear = flameReady() && gasReady() &&
                  !flameDetected &&
                  gasValue < clearLevel &&
                  !gasDigital;

  if (allClear) {

    if (clearSince == 0) {
      clearSince = now;
    }

    // safe for long enough -> STOP ALARM
    if (now - clearSince >= ALARM_CLEAR_MS) {

      alarmActive = false;
      peakGas = 0;
      alarmStartedAt = 0;
      clearSince = 0;

      setDoor(DOOR_LOCKED);
      showMessage("Alarm cleared", "DOOR LOCKED");

      updateBuzzer();
      updateLcd();

      Serial.println("Alarm cleared.");

      reportAlarm("clear", gasValue);
    }

  } else {

    clearSince = 0;
  }
}

// ---------- Door Timer ----------

// Lock the door again when a card-opened door times out
void checkDoorTimer() {

  if (doorState == DOOR_TIMED && relockAt > 0 && millis() >= relockAt) {

    setDoor(DOOR_LOCKED);
    showMessage("Door relocked", "LOCKED");
  }
}

// ---------- Backend Heartbeat ----------

// Do what the backend asks (lock / unlock)
void handleRemoteCommand(String command) {

  // remote commands can never lock the exit during an alarm
  if (alarmActive) return;

  if (command == "unlock") {

    setDoor(DOOR_LATCHED);
    showMessage("Remote unlock", "DOOR OPEN");

  } else if (command == "lock") {

    setDoor(DOOR_LOCKED);
    showMessage("Remote lock", "LOCKED");
  }
}

void sendHeartbeat() {

  JsonDocument payload;
  payload["rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -127;
  payload["door_state"] = doorStateName();
  payload["fire_active"] = alarmActive;
  payload["mq2_value"] = gasValue;          // this feeds the smoke graph
  payload["flame_detected"] = flameDetected;

  JsonDocument response;

  if (!postJson("/api/heartbeat", payload, &response)) return;

  // 1. lock / unlock command from the backend
  String command = response["command"] | "";
  handleRemoteCommand(command);

  // 2. is "register new cards" mode on?
  registrationActive = response["registration_active"] | false;

  // 3. new card list available? (also download it when we have no copy yet)
  bool versionGiven = !response["users_version"].isNull();
  uint32_t serverVersion = response["users_version"] | 0;

  if (!cacheFileAvailable || !versionGiven || serverVersion != cachedUsersVersion) {
    refreshUidCache(serverVersion);
  }

  // 4. send events that were saved while offline
  syncOfflineQueue();

  // 5. send an alarm report that failed earlier
  if (pendingAlertEvent.length() > 0) {
    reportAlarm(pendingAlertEvent, pendingAlertGas);
  }
}

void checkHeartbeat() {

  if (WiFi.status() != WL_CONNECTED) {
    heartbeatDue = true;      // send one as soon as WiFi is back
    return;
  }

  if (heartbeatDue || millis() - lastHeartbeat >= HEARTBEAT_MS) {

    heartbeatDue = false;
    sendHeartbeat();
    lastHeartbeat = millis();   // count 5 sec from the END of the request
  }
}

// ---------- WiFi + OTA ----------

void checkWifi() {

  if (WiFi.status() != WL_CONNECTED) {

    backendOk = false;
    wifiWasConnected = false;

    // try to reconnect every 10 sec
    if (millis() - lastWifiAttempt >= WIFI_RETRY_MS) {

      lastWifiAttempt = millis();

      Serial.println("[WIFI] Reconnecting...");

      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }

    return;
  }

  // WiFi is connected
  if (!wifiWasConnected) {

    wifiWasConnected = true;

    Serial.print("[WIFI] Connected, IP: ");
    Serial.println(WiFi.localIP());
  }

  // OTA must start AFTER WiFi is connected
  if (otaEnabled && !otaStarted) {

    ArduinoOTA.setHostname("smart-classroom");
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.begin();
    otaStarted = true;

    Serial.println("[OTA] Ready for wireless updates");
  }

  if (otaStarted) {
    ArduinoOTA.handle();
  }
}

// ---------- Setup Helpers ----------

void setupLcd() {

  Wire.begin(LCD_SDA, LCD_SCL);
  Wire.setClock(100000);

  delay(100);

  // look for the LCD at the common I2C addresses
  byte candidates[] = {0x27, 0x3F, 0x20, 0x38};

  byte address = 0x27;
  bool found = false;

  for (int i = 0; i < 4; i++) {

    Wire.beginTransmission(candidates[i]);

    if (Wire.endTransmission() == 0) {
      address = candidates[i];
      found = true;
      break;
    }
  }

  Serial.printf(
    "LCD %s at 0x%02X\n",
    found ? "found" : "not detected, trying",
    address
  );

  lcd = new LiquidCrystal_I2C(address, 16, 2);

  lcd->init();
  lcd->backlight();
  lcd->clear();

  lcdLine(0, "Starting system");
  lcdLine(1, "Sensors warming");
}

void setupStorage() {

  // Preferences: remembers the boot number and the last event number
  preferences.begin("classroom", false);

  bootCount = preferences.getULong64("boot", 0) + 1;
  nextSequence = preferences.getULong64("next_seq", 0);

  preferences.putULong64("boot", bootCount);

  // LittleFS: file system for the card list and offline events
  filesystemReady = LittleFS.begin(true);   // true = format on first use

  if (!filesystemReady) {
    Serial.println("LittleFS unavailable; offline queue/cache disabled");
  }

  loadUidCache();
}

void setupWifi() {

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);            // faster answers from the backend
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  otaEnabled = (strlen(OTA_PASSWORD) > 0);

  if (!otaEnabled) {
    Serial.println("OTA disabled because OTA_PASSWORD is empty");
  }

  lastWifiAttempt = millis();
}

// ---------- Setup ----------

void setup() {

  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== Smart Classroom starting ===");

  bootTime = millis();

  // Gas sensor (analog 0-4095, safe up to 3.3V)

  analogReadResolution(12);
  analogSetPinAttenuation(GAS_AO, ADC_11db);

  pinMode(GAS_AO, INPUT);
  pinMode(GAS_DO, INPUT);

  // Flame sensor

  pinMode(FLAME_DO, INPUT);

  // Buzzer

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // Keypad codes must be 4 digits and different from each other

  keypadCodesReady =
    strlen(UNLOCK_CODE) == CODE_LENGTH &&
    strlen(LOCK_CODE) == CODE_LENGTH &&
    strcmp(UNLOCK_CODE, LOCK_CODE) != 0;

  keypad.setDebounceTime(50);

  // LCD

  setupLcd();

  if (!keypadCodesReady) {
    Serial.println("Keypad disabled; set two different 4-digit codes");
    showMessage("Set keypad codes", "in the settings");
  }

  // Servo (starts locked)

  ESP32PWM::allocateTimer(0);

  servo.setPeriodHertz(50);

  servo.attach(SERVO_PIN, 500, 2400);

  servo.write(SERVO_LOCKED);

  // RFID
  // RST is physically connected to 3.3V

  SPI.begin(
    SPI_SCK,
    SPI_MISO,
    SPI_MOSI,
    RFID_SS
  );

  initRfid();

  // Storage + WiFi

  setupStorage();

  setupWifi();
}

// ---------- Main Loop ----------

void loop() {

  checkSensors();      // 1. flame + gas, alarm logic
  checkRfid();         // 2. RFID cards
  checkKeypad();       // 3. keypad codes
  checkDoorTimer();    // 4. re-lock door after a card
  checkWifi();         // 5. WiFi reconnect + OTA
  checkHeartbeat();    // 6. talk to the backend every 5 sec
  updateBuzzer();      // 7. buzzer on/off
  updateLcd();         // 8. LCD text
  checkRfidHealth();   // 9. make sure the RC522 still answers

  delay(2);            // lets the ESP32 background tasks run
}

