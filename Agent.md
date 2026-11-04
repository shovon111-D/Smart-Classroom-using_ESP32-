# Deployment Credentials

Credentials that were previously included in this specification have been removed. Rotate any credentials that were shared or committed.

# 0. AI Agent Instructions (read first)

**Role:** You are implementing a university IoT lab project. Two independent modules (firmware, website) share one HTTP API contract (section 6).

**Rules**
1. Implement only what is in this file. If something is missing or ambiguous, put a `TODO(question)` comment and choose the simplest option. Do not add features.
2. **Out of scope. Never add:** user login/authentication (accounts, passwords, sessions, tokens for people), access levels/permissions (the `role` field on a user is only a teacher/student label), multiple classrooms/devices, CSV/PDF/any export, search boxes, mobile/responsive layouts, notifications other than the fire email.
3. The firmware must **never block** the main loop: no `delay()`, and no network call in the main loop. Use `millis()` timers, a state machine and a separate network task (section 3.0), so fire detection and the keypad stay responsive.
4. **Safety first:** fire logic runs locally on the ESP32 and must work with no network (section 3.2).
5. **Secrets and deployment settings** go in git-ignored files, never in code, in the frontend bundle (except the public anon key), or in section-by-section examples:
   - Firmware `secrets.h` (provide `secrets.example.h`): `WIFI_SSID`, `WIFI_PASSWORD`, `API_BASE_URL`, `KEYPAD_UNLOCK_CODE`, `KEYPAD_LOCK_CODE`, `OTA_PASSWORD`.
   - Backend `.env` (provide `.env.example`, all keys, empty values): `SUPABASE_URL`, `SUPABASE_SERVICE_KEY`, `SMTP_HOST` (default `smtp.gmail.com`), `SMTP_PORT` (default `587`), `SMTP_USER`, `SMTP_APP_PASSWORD`, `ALERT_EMAIL_FROM`, `ALERT_EMAIL_TO`, `DOOR_UNLOCK_PASSKEY`, `DOOR_LOCK_PASSKEY`, `CORS_ORIGINS` (the dashboard origin(s), comma-separated), `LOCAL_TZ` (IANA name, e.g. the classroom's local time zone).
   - Frontend `.env` (provide `.env.example`): `VITE_API_BASE_URL`, `VITE_SUPABASE_URL`, `VITE_SUPABASE_ANON_KEY`. Only the **anon** key is allowed in the frontend; the service-role key never.
   - The demo door codes `1234` / `4321` (section 2) are the documented **default values** of the `KEYPAD_*` / `DOOR_*` settings above. Code reads them from config and never contains them as literals.
6. All timestamps are stored in UTC (`timestamptz`) and displayed in local time on the dashboard (browser time zone). The backend uses `LOCAL_TZ` only to define "a day" for the time-inside report.
7. RFID UIDs are normalized everywhere to **uppercase hex, no separators** (e.g. `A1B2C3D4`). The firmware normalizes before sending and the backend normalizes again on input.
8. Keep all tunable numbers in one config file per module (`config.h`, `config.py`). No magic numbers. (Secrets and deployment settings live in section 0 rule 5 files instead.)
9. The JSON shapes in section 6 are exact. Do not rename fields.

**Suggested repo layout**
```
/firmware/        ESP32 C++ (PlatformIO or Arduino): src/, include/config.h, include/secrets.example.h
/backend/         FastAPI: app/main.py, routers/, services/, config.py, .env.example
/frontend/        Desktop web dashboard (default: React + Vite; framework may be changed), .env.example
/supabase/        schema.sql (section 7)
AGENTS.md         this file
```

**Suggested build order:** (1) `supabase/schema.sql` → (2) backend endpoints + tests with curl/Swagger → (3) firmware against the backend → (4) frontend dashboard → (5) OTA + polish.

**Definition of done for each endpoint/feature:** matches the contract in section 6, writes to the correct table, and appears on the dashboard in realtime.

---

## 1. Scope and Fixed Facts

- **One classroom, one ESP32.** There is no `classroom_id` or `device_id` anywhere.
- **No user login and no superuser.** Anyone with the dashboard URL can use it. The only protection is the **door passkey** (section 2).
- **Hardware (fixed):** ESP32, RC522 RFID reader + cards, 4×4 keypad, SG90 servo (door lock), MQ-2 smoke/gas sensor (analog output is required for severity levels), buzzer, flame sensor, and 16×2 I2C LCD. No status LEDs are used.
- **Hardware notes:** connect the MQ-2 analog output to an **ADC1** pin (GPIO 32–39), because ADC2 does not work while WiFi is on. The MQ-2 module's analog output can reach 5 V, so scale it to 3.3 V with a voltage divider. Power the servo from a separate 5 V supply with a common ground.
- **Stack:** ESP32 (WiFi, HTTP/JSON) → FastAPI → Supabase (Postgres + Realtime) → dashboard.

## 2. Door Codes and Servo

| Item | Value |
|---|---|
| Servo `0°` | Door **LOCKED** |
| Servo `90°` | Door **UNLOCKED** |
| Code / passkey `1234` (default) | **Unlock** → servo 90 |
| Code / passkey `4321` (default) | **Lock** → servo 0 |

- The same codes work on the **keypad** (4 digits, auto-submit; `*` clears) and the **dashboard** (passkey input).
- Keypad codes are checked **locally on the ESP32** (works offline) and reported to the backend for logging.
- Dashboard passkeys are checked **on the backend**. The passkey must **match the action**: `unlock` requires the unlock passkey, `lock` requires the lock passkey. Anything else is a wrong passkey. The frontend never contains the codes.

## 3. MODULE A: ESP32 Firmware (C++), hardware control only

**Responsibility:** read sensors, drive actuators, talk to the backend. The firmware does **not** decide IN/OUT or occupancy. The backend does.

### 3.0 Task model and non-blocking rules

- **Main loop:** sensor sampling, keypad, RFID reading, door state machine, servo, buzzer, LCD. It never waits for the network.
- **Network task (separate FreeRTOS task):** WiFi reconnect, every HTTP call, the heartbeat timer, and all LittleFS/NVS access for the cache and queue. The main loop sends it requests through a FreeRTOS queue and polls results without blocking.
- Every HTTP call uses `HTTP_TIMEOUT_MS` (default `2000`).
- **Backend unreachable** means: WiFi is down, or the request timed out, or the connection failed, or the response was HTTP 5xx. Any other HTTP response (including 404 and 409) is a valid answer and means the backend is reachable. The firmware keeps a flag `backend_ok`, set to true after any reachable response and to false after an unreachable one.
- Timers and counters (`millis` values, `t_ms`, `now_ms`) use a 64-bit millisecond counter (`esp_timer_get_time() / 1000`) so they never wrap.
- Short buzzer beeps and LCD messages are timed with `millis()` (`BUZZER_SHORT_MS`, `MSG_DISPLAY_MS`).
- The door state is not persisted. After a reboot or power loss the door starts `LOCKED`.

### 3.1 Door state machine

| State | Servo | Entered by | Left by |
|---|---|---|---|
| `LOCKED` | 0 | boot; relock timer; keypad 4321; lock command; fire cleared | any unlock |
| `UNLOCKED_TIMED` | 90 | granted RFID tap (backend `granted`, or cache hit offline) while `LOCKED` | relock timer → `LOCKED`; keypad 4321 or lock command → `LOCKED`; keypad 1234 or unlock command → `UNLOCKED_LATCHED` (timer cancelled); fire → `FIRE_UNLOCKED` (timer cancelled) |
| `UNLOCKED_LATCHED` | 90 | keypad 1234 or unlock command (from `LOCKED` or `UNLOCKED_TIMED`) | keypad 4321 or lock command → `LOCKED`; fire → `FIRE_UNLOCKED` |
| `FIRE_UNLOCKED` | 90 | fire confirmed (from any state) | fire cleared (after the `FIRE_CLEAR_MS` debounce) → `LOCKED` **immediately** |

**Rules**
- **Servo goes 0→90 immediately** on a valid tap. **90→0 happens only after a delay**: `DOOR_RELOCK_DELAY_MS` (default `5000`, configurable).
- The relock timer applies **only** to `UNLOCKED_TIMED`. If the door is already `UNLOCKED_LATCHED` or `FIRE_UNLOCKED`, a valid tap only logs attendance and does not start a relock timer.
- A valid tap while `UNLOCKED_TIMED` restarts the timer.
- **Priority:** `FIRE_UNLOCKED` > keypad/remote commands > RFID. While fire is active, lock commands and keypad codes have no effect on the door (keypad attempts are still reported), and RFID taps still log attendance but do not touch the servo.
- **Fire clear locks immediately.** `DOOR_RELOCK_DELAY_MS` applies **only** to the `UNLOCKED_TIMED` → `LOCKED` transition. When fire clears, the servo goes to 0 as soon as the `FIRE_CLEAR_MS` confirmation passes, with no extra delay, even if the door was latched open before the fire. A fire that starts while a relock timer is running cancels that timer.

### 3.2 Fire / smoke / gas (local, works offline)

- Read the MQ-2 analog value every `MQ2_SAMPLE_INTERVAL_MS` (default `200`). Fire detection is disabled for `MQ2_WARMUP_MS` after boot (LCD shows "Warming up"); heartbeats still report the value.
- `value >= SMOKE_ALARM_THRESHOLD` continuously for `FIRE_CONFIRM_MS` → **fire active**: buzzer ON, LCD "FIRE!", servo 90, state `FIRE_UNLOCKED`.
- Clear only when `value < SMOKE_ALARM_THRESHOLD - SMOKE_HYSTERESIS` continuously for `FIRE_CLEAR_MS` (avoids flapping). Then buzzer OFF, servo 0, state `LOCKED`.
- Thresholds are **placeholders to be calibrated** on the real sensor (12-bit ADC, 0–4095).
- Report to the backend (best effort): `POST /api/alert` with `event` = `start` / `update` / `clear`. Send `update` only when the peak value has increased, and at most once per `ALERT_UPDATE_MIN_INTERVAL_MS` (default `5000`). If a call fails, retry on the next heartbeat tick.
- Fire events are **not queued**. The heartbeat carries `fire_active` and the backend reconciles the alert from it (section 4.1), so a fire that starts or clears while offline is still recorded once the network returns.

### 3.3 RFID flow

1. RC522 reads a card → normalize UID.
2. **Path rule (ordering):** if `backend_ok` is true **and the offline queue is empty**, use the **online path**. Otherwise use the **offline path**. This guarantees that pending offline events are always applied before any newer tap, so IN/OUT order stays correct.
3. **Online path:** `POST /api/rfid/scan {uid}` and handle `result`:

| `result` | ESP32 action |
|---|---|
| `granted` | Servo 0→90 immediately (state per 3.1), LCD "Welcome <name>" / "IN" or "OUT", start relock timer (3.1) |
| `denied` (HTTP 404) | Buzzer ON (short), LCD "Access Denied", servo stays 0 |
| `registered` | LCD "Registered: <name>". **No buzzer. Door does not open.** |
| `already_registered` (HTTP 409) | LCD "Already registered". **No buzzer.** |

If the request itself turns out unreachable (timeout, 5xx), handle that same tap with the offline path.

4. **Offline path:**
   - Check the persistent UID cache (3.7). If the UID is cached: open as in `granted` (state per 3.1), LCD "Access Granted" / "(cached)". The cache holds UIDs only, so the name and IN/OUT are not shown.
   - If the UID is not cached, or there is no cache file: Buzzer ON (short), LCD "Access Denied" (or "Offline, no cache" when the cache file is missing). Servo stays 0.
   - In **both** cases append the tap to the persistent offline queue (3.7). The backend decides on sync whether it is attendance or an unknown card.
5. **UID cache:** load it from flash at boot (so it works even if the first boot is offline). Whenever the backend is reachable and `users_version` (from the heartbeat response) differs from the cached version, fetch `GET /api/uids` and overwrite the flash copy.

### 3.4 Keypad

- Digits `0`–`9` build the code. `*` clears. Other keys (`#`, `A`–`D`) are ignored. A partial entry is cleared after `KEYPAD_TIMEOUT_MS` (default `5000`) without a key press.
- Correct unlock code → `UNLOCKED_LATCHED`; correct lock code → `LOCKED`. LCD shows the result.
- Wrong code → LCD "Wrong code", short buzzer beep.
- Report every attempt. Use the same **path rule as RFID** (online path only if `backend_ok` and the queue is empty): `POST /api/keypad-event {type, action?}`. Otherwise (or if that call is unreachable) append it to the persistent offline queue.

### 3.5 Heartbeat and remote commands

The ESP32 can only call out (the backend cannot push to it), so **the heartbeat is also the command channel**.

- Every `HEARTBEAT_INTERVAL_MS` (default `5000`): `POST /api/heartbeat {rssi, door_state, fire_active, mq2_value}`. `door_state` is one of `LOCKED`, `UNLOCKED_TIMED`, `UNLOCKED_LATCHED`, `FIRE_UNLOCKED`.
- The response is `{command, users_version, registration_active}` where `command` is `"lock"`, `"unlock"` or `null`. Execute the command (`unlock` → `UNLOCKED_LATCHED`, `lock` → `LOCKED`, subject to the fire priority rule).
- `registration_active` true → LCD "Register: tap card" while idle (so registration mode is visible on the device, up to one heartbeat late).
- A successful heartbeat (HTTP 200) sets `backend_ok` and triggers the queue flush (3.7) if the queue is not empty.
- Remote command latency is up to one heartbeat interval. This is acceptable.

### 3.6 LCD, buzzer, OTA

- **LCD (16×2):** idle shows "Ready" and the door state. Also shows access granted/denied, "Registered", "FIRE!", "Warming up", and an offline indicator. There is no clock (no RTC/NTP).
- **Buzzer:** continuous during fire (overrides everything); a short beep (`BUZZER_SHORT_MS`) on denied and on wrong keypad code; silent otherwise.
- **OTA:** `ArduinoOTA` (password from `secrets.h`) or HTTP OTA. Optional, build last.
- WiFi auto-reconnect with non-blocking retry. NTP not required (the backend timestamps everything).

### 3.7 Persistent storage (survives reboot and power loss)

Use **LittleFS** for data files and **Preferences (NVS)** for small counters. No SD card is needed. Only the network task touches these files (guarded by a mutex).

| Item | Where | Details |
|---|---|---|
| UID cache | LittleFS `/uids.json` | `{version, uids:[...]}`. Rewritten only when `users_version` changes. Loaded at boot. |
| Offline event queue | LittleFS `/queue.jsonl` | Append-only, one JSON event per line. Written only on the offline path, so flash wear is low. |
| `boot_count` | NVS | Incremented on every boot. |
| `next_seq` | NVS | Monotonic event counter, incremented for every queued event, never reset. |

**Queue event format:** `{"seq":123,"kind":"rfid","uid":"A1B2C3D4","boot":7,"t_ms":845200}` or `{"seq":124,"kind":"keypad","type":"wrong_code","boot":7,"t_ms":850000}` or `{"seq":125,"kind":"keypad","type":"success","action":"unlock","boot":7,"t_ms":851000}`. `t_ms` is the millisecond counter at the time of the event and `boot` is the `boot_count` at that time.

**Flush procedure** (when a heartbeat succeeds and the queue is not empty):
1. Send up to `OFFLINE_SYNC_BATCH` (default 50) events, oldest first: `POST /api/offline-sync {boot, now_ms, events:[...]}`.
2. The response is `{last_seq}`. Remove from the file every event with `seq <= last_seq` (rewrite the file with the remainder).
3. Repeat until the queue is empty. If the response is lost, resending is safe because the backend ignores sequence numbers it has already applied.
4. While the queue is non-empty, new taps and keypad events keep being appended (path rule, 3.3), so the flush always delivers them after the older ones.

**Limits:** at most `OFFLINE_QUEUE_MAX` (default 200) events. When full, drop the oldest and show a short LCD warning.

**Time accuracy:** events from the *current* boot get an exact timestamp. The ESP32 has no RTC, so events recorded in an *earlier* boot have an unknown real time; the backend stamps them at the estimated start of the current boot, keeps their order, and marks them `time_approx = true` (section 4.1).

## 4. MODULE B: Website (FastAPI + Supabase + Dashboard)

### 4.1 Backend responsibilities (FastAPI)

- Owns all business logic: UID lookup, IN/OUT toggle, occupancy, registration mode, alert lifecycle, email, passkey check, audit log.
- Uses the Supabase **service-role key** for writes (server side only). Calls the SQL functions of section 7 through `rpc()` for every multi-step write.
- Allows CORS requests from the origins in `CORS_ORIGINS`.
- Swagger UI at `/docs` (FastAPI default).
- Normalizes every incoming UID (section 0 rule 7).

**RFID scan (`POST /api/rfid/scan`)**, in this order:
1. **Registration mode active?** (see below). If yes: if the UID belongs to an active user → 409 `already_registered` (registration stays active, so another card can be tapped); otherwise create the user from the pending registration, mark any unresolved `unknown_cards` row for that UID as resolved, **clear `pending_registration` and `registration_expires`**, write the audit entry, return `registered`. It is not treated as unknown, not logged to `unknown_cards`, and causes no buzzer.
2. Otherwise look up an **active** (not deleted) user by UID. Found → call `apply_tap` (section 7) with `source='online'` and return `granted` with `direction`, `name`, `role`. Not found → call `record_unknown` and return 404 `denied`.

**IN/OUT and occupancy:** the toggle is implemented inside the `apply_tap` SQL function: the user's last `attendance_logs` row **by `id`** decides the direction (none or `OUT` → `IN`, occupancy +1; `IN` → `OUT`, occupancy −1, never below 0). The function locks the `device_state` row so the attendance insert and the occupancy update happen in one transaction and cannot interleave with another tap, a deletion or an occupancy reset.

**Registration mode**
- `POST /api/registration {name, role, student_id?}` stores the pending registration in `device_state` with `registration_expires = now + REGISTRATION_TIMEOUT_S` (default 30). If `student_id` is already used by an active user → 409 `{detail}`. Starting a new registration replaces any pending one.
- Registration is **active** when `pending_registration` is set and `registration_expires > now`. An expired registration is treated as inactive and is **cleared** (both columns set to null) the first time the backend encounters it (scan, status read or heartbeat). `DELETE /api/registration` clears it immediately. A successful registration clears it (above).
- `student_id` is optional for both roles.

**Assign unknown card:** `POST /api/unknown/{id}/assign {name, role, student_id?}` creates the user from that stored UID (no scan needed), sets `resolved_at`. 404 if the record is missing or already resolved; 409 if the UID or `student_id` now belongs to an active user.

**Unknown cards:** there is **one record per unresolved UID**. A repeat scan updates `scanned_at` (last scan) and increments `scan_count` (`record_unknown`). `GET /api/unknown` returns only unresolved records (`resolved_at IS NULL`). Creating a user with a UID (by registration or assignment) resolves any unresolved record for that UID.

**Users:** `GET /api/users` lists active users. `PUT /api/users/{id}` may change `name`, `role`, `student_id` (not the RFID UID; to change a card, delete and re-register). Duplicate `student_id` → 409. Edits on deleted users → 404.

**User deletion is a soft delete.** `DELETE /api/users/{id}` calls `soft_delete_user` (section 7), which in one transaction: if the user is currently inside (last log = `IN`), inserts an `OUT` log and decrements occupancy; then sets `deleted_at = now()`. Rows are never removed, so attendance history and time-inside keep pointing at the right person. A soft-deleted user: is hidden from the user list; is excluded from `GET /api/uids` and from RFID lookup (their card now behaves as an unknown card); frees their `rfid_uid` and `student_id` for re-registration (the unique indexes are partial).

**Heartbeat (`POST /api/heartbeat`)**
- Update `device_state` (`last_seen`, `wifi_rssi`, `door_state`, `fire_active`, `mq2_value`) and insert a `sensor_readings` row.
- **Fire reconciliation:** if `fire_active` is true and no alert is open → run the `start` logic below. If `fire_active` is false and an alert is open → run the `clear` logic. This makes fire reporting self-healing when `/api/alert` calls were lost.
- Response `{command, users_version, registration_active}`. `command` is `device_state.pending_command` if it is not older than `COMMAND_TTL_S` (default 30), otherwise `null`. A delivered or expired command is cleared (`pending_command` and `pending_command_at` set to null). A new command overwrites an older pending one.

**Fire alert lifecycle (`POST /api/alert` and heartbeat reconciliation)**
- `start`: if an alert is already open (`cleared_at IS NULL`), treat it as `update` (no second row, no second email). Otherwise insert an `alerts` row, compute severity, and send the email once (background task).
- `update`: raise `peak_value` (and severity) if the new value is higher.
- `clear`: set `cleared_at` on the open alert (no-op if none).
- **Severity (from the MQ-2 peak value):** `low` / `medium` / `high` using `SEVERITY_MEDIUM_MIN` and `SEVERITY_HIGH_MIN` (placeholders; both must be above the firmware `SMOKE_ALARM_THRESHOLD`; `low` is anything between the alarm threshold and the medium minimum).
- **Email:** sent once per alert, over SMTP with STARTTLS, using the `.env` values from section 0 rule 5. It states the severity and reading at the time the alert started. If any value is missing, skip the email and log a warning (the alert itself must still be stored). Set `email_sent = true` only after a successful send. Never block the request.

**Remote door:** `POST /api/door {passkey, action}`. Wrong passkey (section 2 matching rule) → 403 `{detail}` + `access_events(wrong_passkey)`. Correct → set `device_state.pending_command` and `pending_command_at`, log `access_events(remote_lock/remote_unlock)` and `audit_log` (`door_lock` / `door_unlock`), return `{ok:true}`. If the device is offline the command simply expires after `COMMAND_TTL_S`.

**Keypad events (`POST /api/keypad-event`):** `type = wrong_code` → `access_events(wrong_code, keypad)`. `type = success` with `action = unlock` → `master_unlock`; `lock` → `master_lock` (source `keypad`).

**Online status:** `online = now() - last_seen < OFFLINE_AFTER_S` (default 15, computed on read).

**Sensor readings:** inserted from heartbeats; rows older than `SENSOR_RETENTION_HOURS` (default 24) are pruned by a scheduled task inside the FastAPI app (hourly).

**Occupancy reset:** `POST /api/occupancy/reset` calls `reset_occupancy` (section 7): in one transaction it inserts an `OUT` log for every user whose last log is `IN`, then sets occupancy to 0. Audit action `occupancy_reset`.

**Time spent inside** (`GET /api/analytics/time-inside?date=YYYY-MM-DD`):
- The date is a **local date in `LOCAL_TZ`**; the day window is `[00:00, next 00:00)` local time, converted to UTC.
- For each user, build sessions by pairing every `IN` with the next `OUT` in **`id` order**, then clip each session to the day window. A session that started before the window counts from the window start. An `IN` with no `OUT` is "currently inside" and runs until `min(now, window end)`.
- Deleted users who have logs in that window still appear, labelled "(deleted)". A session built from any row with `time_approx = true` is marked "approx." No stored table.

**Offline sync (`POST /api/offline-sync {boot, now_ms, events}`):**
- Process events in `seq` order and skip any `seq <= device_state.last_offline_seq`. `rfid` events never trigger registration mode.
- Timestamp rule: event `boot` equals the payload `boot` → `now − (now_ms − t_ms)` (exact). Earlier boot → the estimated start of the current boot, `now − now_ms`, with `time_approx = true` (this keeps the order and is never later than any exact event of the current boot).
- `rfid` event: active user found → `apply_tap` with `source='offline_sync'` and the computed timestamp. Not found (unknown, or user deleted since) → `record_unknown` with the computed timestamp.
- `keypad` event: `wrong_code` → `access_events(wrong_code)`; `success` + `action` → `master_unlock` / `master_lock` (source `keypad`), with the computed timestamp.
- A malformed event is skipped, logged as a warning, and counted as applied (so it cannot block the queue forever).
- Finally set `last_offline_seq` to the highest `seq` processed and return `{last_seq}`.

**Audit log actions** (written for every dashboard action): `registration_start`, `registration_cancel`, `register_user` (details: `source` = `registration` or `assign`), `user_update`, `user_delete`, `door_lock`, `door_unlock`, `occupancy_reset`.

**`updated_at`** on `users` is maintained by a Postgres trigger (section 7). The backend does not set it manually.

### 4.2 Dashboard (single desktop page)
- **Desktop only.** Layout for wide screens (≥1280 px). No mobile breakpoints, **no search**, **no export**. Dark mode toggle is included.
- Realtime updates through Supabase Realtime (anon key, read only). No manual refresh. Writes go through the FastAPI backend only.

Sections:
1. **Status bar:** online/offline, last seen, WiFi RSSI, door state, occupancy, fire status, and the **Occupancy reset** button (with a confirm dialog).
2. **Door control:** passkey input + Lock / Unlock buttons. Shows the command as pending until the door state changes or `COMMAND_TTL_S` passes.
3. **Live attendance:** latest taps in `id` order, newest first (name, role, IN/OUT, time; "approx." when `time_approx`; "(deleted)" for deleted users).
4. **Users:** *Register Teacher* and *Register Student* buttons (two separate options; each opens a form and starts registration mode, then shows "Waiting for card…" with a countdown and a Cancel button), a list with edit and delete (delete asks for confirmation and is a soft delete), and the time spent inside per user for a chosen date (date picker, default today).
5. **Unknown cards:** list of unresolved cards (UID, last scan, scan count) with an "Assign to new teacher/student" action.
6. **Fire alerts:** history with severity, start/clear time. **Live MQ-2 graph.**
7. **Security log:** unknown cards, wrong keypad codes, wrong dashboard passkeys, master code usage (`master_unlock` / `master_lock`) in one merged chronological view. (Remote lock/unlock appear in the audit log, not here.)
8. **Admin audit log:** every dashboard action.

## 5. Feature List (final, 30)

**Firmware (A)**
- A1 Local fire/smoke detection, buzzer, auto-unlock
- A2 Keypad master code (1234 unlock / 4321 lock), works offline
- A3 LCD status display
- A4 RFID read + persistent offline UID cache and persistent offline event queue
- A5 Servo: instant open, delayed relock
- A6 Heartbeat + remote command polling
- A7 OTA update (stretch)

**Backend (B)**
- B1 REST API + Swagger `/docs`
- B2 RFID validation, auto IN/OUT, occupancy
- B3 Unknown RFID handling (404 + `unknown_cards`)
- B4 Teacher/student registration via registration mode
- B5 Assign unknown card to a new user
- B6 Fire alert lifecycle, severity, email
- B7 Remote door lock/unlock with passkey
- B8 Occupancy reset
- B9 Device online/offline, last seen, RSSI
- B10 Keypad event, master code and unauthorized access logging
- B11 Audit log
- B12 Time spent inside per user

**Dashboard (C)**
- C1 Live occupancy and status bar
- C2 Realtime updates
- C3 User management (Register Teacher / Register Student, edit, delete)
- C4 Door control panel
- C5 Live attendance table
- C6 Alerts panel and fire history with severity
- C7 Live gas/smoke graph
- C8 Security log
- C9 Audit log view
- C10 Dark mode, desktop layout
- C11 Unknown cards list

**Not included:** login/auth, CSV export, PDF export, student export, search, mobile layout, per-classroom views.

## 6. API Contract (JSON)

### Device endpoints (called by the ESP32)
| Method + path | Request | Response |
|---|---|---|
| `POST /api/rfid/scan` | `{uid}` | 200 `{result:"granted", direction:"IN"/"OUT", name, role}` · 404 `{result:"denied"}` · 200 `{result:"registered", name}` · 409 `{result:"already_registered"}` |
| `POST /api/offline-sync` | `{boot, now_ms, events:[{seq, kind:"rfid"/"keypad", uid?, type?, action?, boot, t_ms}]}` (oldest first) | `{last_seq}` (see sections 3.7 and 4.1) |
| `GET /api/uids` | | `{version, uids:[...]}` |
| `POST /api/heartbeat` | `{rssi, door_state, fire_active, mq2_value}` | `{command:"lock"/"unlock"/null, users_version, registration_active}` |
| `POST /api/alert` | `{event:"start"/"update"/"clear", mq2_value}` | `{ok:true}` |
| `POST /api/keypad-event` | `{type:"success"/"wrong_code", action?:"lock"/"unlock"}` | `{ok:true}` |

### Dashboard endpoints
| Method + path | Purpose |
|---|---|
| `GET /api/health` | Live connection checks for the backend, Supabase database, and ESP32's most recent heartbeat |
| `GET /api/status` | One object: `device_state` fields (without `pending_registration` details) + computed `online` and `registration_active` |
| `POST /api/door` | `{passkey, action}`, remote lock/unlock → `{ok:true}` |
| `POST /api/registration` / `DELETE /api/registration` | Start `{name, role, student_id?}` / cancel registration mode |
| `GET/PUT/DELETE /api/users` (`/{id}`) | List active users, edit, **soft-delete** |
| `GET /api/unknown`, `POST /api/unknown/{id}/assign` | Unresolved unknown cards; assign one to a new user |
| `POST /api/occupancy/reset` | Close open sessions and set occupancy to 0 |
| `GET /api/attendance?limit=` | Recent attendance (joined with user name and role) |
| `GET /api/analytics/time-inside?date=` | Per-user time inside for a local date |
| `GET /api/alerts` | Fire alert history |
| `GET /api/sensor-readings?minutes=` | Graph data |
| `GET /api/security-log` | Unified unknown cards + wrong code/passkey + master code events |
| `GET /api/audit-log` | Audit entries |

**Error format:** `{ "detail": "message" }` (FastAPI default) for every error, **except** the two documented scan responses above (404 `{result:"denied"}` and 409 `{result:"already_registered"}`), which keep the `result` shape because the firmware depends on it.

## 7. Database (Supabase Postgres)

**Design notes:** `users` is the single permanent table (teacher + student UIDs). Users are **soft-deleted** (`deleted_at`), never hard-deleted, so attendance history and time-inside stay attached to the right person. Other tables are logs or state. Unknown-card scans are stored **only** in `unknown_cards` (one row per unresolved UID), not duplicated in `access_events`. Pending registration lives in `device_state`, so `users.rfid_uid` can stay `NOT NULL`. Multi-step writes (tap, delete, reset, unknown card) are SQL functions so each is one transaction; they are callable only with the service-role key.

```sql
-- PERMANENT TABLE
create table users (
  id          uuid primary key default gen_random_uuid(),
  name        text not null,
  role        text not null check (role in ('teacher','student')),
  student_id  text,
  rfid_uid    text not null,
  created_at  timestamptz not null default now(),
  updated_at  timestamptz not null default now(),
  deleted_at  timestamptz                          -- soft delete; NULL = active
);
-- uniqueness applies to ACTIVE users only, so a deleted user's card / student id can be reused
create unique index users_rfid_active_uniq    on users(rfid_uid)   where deleted_at is null;
create unique index users_student_active_uniq on users(student_id) where deleted_at is null and student_id is not null;

-- keep updated_at correct on every edit
create or replace function set_updated_at() returns trigger as $$
begin new.updated_at = now(); return new; end;
$$ language plpgsql;
create trigger users_set_updated_at before update on users
  for each row execute function set_updated_at();

-- LOG TABLES
create table attendance_logs (
  id         bigint generated always as identity primary key,   -- insertion order = true event order
  user_id    uuid not null references users(id) on delete restrict,   -- users are soft-deleted, never removed
  rfid_uid   text not null,                       -- UID at tap time (audit)
  direction  text not null check (direction in ('IN','OUT')),
  source     text not null default 'online' check (source in ('online','offline_sync')),
  time_approx boolean not null default false,     -- true if logged_at is only an estimate (tap recorded in an earlier boot)
  logged_at  timestamptz not null default now()
);
create index attendance_user_id   on attendance_logs(user_id, id desc);
create index attendance_time      on attendance_logs(logged_at desc);

create table unknown_cards (
  id          bigint generated always as identity primary key,
  rfid_uid    text not null,
  scan_count  int  not null default 1,
  scanned_at  timestamptz not null default now(),  -- time of the LAST scan
  resolved_at timestamptz                          -- set when assigned to a user
);
-- one unresolved record per UID
create unique index unknown_unresolved_uniq on unknown_cards(rfid_uid) where resolved_at is null;

create table alerts (
  id          bigint generated always as identity primary key,
  type        text not null default 'fire',
  severity    text not null check (severity in ('low','medium','high')),
  peak_value  int  not null,
  started_at  timestamptz not null default now(),
  cleared_at  timestamptz,
  email_sent  boolean not null default false
);
-- at most one open alert at a time
create unique index alerts_one_open on alerts(type) where cleared_at is null;

create table sensor_readings (
  id          bigint generated always as identity primary key,
  mq2_value   int not null,
  recorded_at timestamptz not null default now()
);
create index sensor_time on sensor_readings(recorded_at desc);   -- pruned by SENSOR_RETENTION_HOURS

create table access_events (
  id         bigint generated always as identity primary key,
  event_type text not null check (event_type in
             ('wrong_code','master_unlock','master_lock','remote_unlock','remote_lock','wrong_passkey')),
  source     text not null check (source in ('keypad','dashboard')),
  created_at timestamptz not null default now()
);

create table audit_log (
  id         bigint generated always as identity primary key,
  action     text not null,                        -- see section 4.1 for the list of actions
  details    jsonb,
  created_at timestamptz not null default now()
);

-- SINGLE-ROW DEVICE STATE (one ESP32)
create table device_state (
  id                   int primary key default 1 check (id = 1),
  last_seen            timestamptz,
  wifi_rssi            int,
  door_state           text not null default 'LOCKED'
                       check (door_state in ('LOCKED','UNLOCKED_TIMED','UNLOCKED_LATCHED','FIRE_UNLOCKED')),
  fire_active          boolean not null default false,
  mq2_value            int,
  occupancy            int not null default 0 check (occupancy >= 0),
  pending_command      text check (pending_command in ('lock','unlock')),
  pending_command_at   timestamptz,                -- command is dropped after COMMAND_TTL_S
  pending_registration jsonb,                      -- {name, role, student_id}
  registration_expires timestamptz,
  users_version        int not null default 0,
  last_offline_seq     bigint not null default 0   -- highest offline-queue seq already applied (dedupe)
);
insert into device_state (id) values (1);

-- bump users_version whenever users change (ESP32 re-syncs its UID cache)
create or replace function bump_users_version() returns trigger as $$
begin update device_state set users_version = users_version + 1 where id = 1; return null; end;
$$ language plpgsql;
create trigger users_changed after insert or update or delete on users
  for each statement execute function bump_users_version();

-- TRANSACTIONAL FUNCTIONS (called by the backend with the service-role key)

-- Record one tap: decide IN/OUT from the user's last log (by id), insert it, adjust occupancy. Returns the direction.
create or replace function apply_tap(p_user_id uuid, p_uid text, p_source text,
                                     p_time_approx boolean, p_logged_at timestamptz)
returns text as $$
declare v_last text; v_dir text;
begin
  perform 1 from device_state where id = 1 for update;      -- serialize with other taps/resets/deletes
  select direction into v_last from attendance_logs where user_id = p_user_id order by id desc limit 1;
  v_dir := case when v_last = 'IN' then 'OUT' else 'IN' end;
  insert into attendance_logs(user_id, rfid_uid, direction, source, time_approx, logged_at)
    values (p_user_id, p_uid, v_dir, p_source, p_time_approx, p_logged_at);
  update device_state
     set occupancy = case when v_dir = 'IN' then occupancy + 1 else greatest(occupancy - 1, 0) end
   where id = 1;
  return v_dir;
end;
$$ language plpgsql;

-- Soft-delete a user; if currently inside, log an OUT and decrement occupancy first.
create or replace function soft_delete_user(p_user_id uuid) returns void as $$
declare v_last text; v_uid text;
begin
  perform 1 from device_state where id = 1 for update;
  select rfid_uid into v_uid from users where id = p_user_id and deleted_at is null;
  if not found then raise exception 'user not found or already deleted'; end if;
  select direction into v_last from attendance_logs where user_id = p_user_id order by id desc limit 1;
  if v_last = 'IN' then
    insert into attendance_logs(user_id, rfid_uid, direction) values (p_user_id, v_uid, 'OUT');
    update device_state set occupancy = greatest(occupancy - 1, 0) where id = 1;
  end if;
  update users set deleted_at = now() where id = p_user_id;
end;
$$ language plpgsql;

-- Occupancy reset: close every open session with an OUT, then set occupancy to 0.
create or replace function reset_occupancy() returns void as $$
begin
  perform 1 from device_state where id = 1 for update;
  insert into attendance_logs(user_id, rfid_uid, direction)
    select user_id, rfid_uid, 'OUT'
      from (select distinct on (user_id) user_id, rfid_uid, direction
              from attendance_logs order by user_id, id desc) last_log
     where direction = 'IN';
  update device_state set occupancy = 0 where id = 1;
end;
$$ language plpgsql;

-- Unknown card: one unresolved row per UID; a repeat scan updates the last-scan time and count.
create or replace function record_unknown(p_uid text, p_at timestamptz default now()) returns void as $$
begin
  update unknown_cards set scanned_at = p_at, scan_count = scan_count + 1
   where rfid_uid = p_uid and resolved_at is null;
  if not found then insert into unknown_cards(rfid_uid, scanned_at) values (p_uid, p_at); end if;
end;
$$ language plpgsql;

-- Only the backend (service role) may call these; PostgREST would otherwise expose them to anon.
revoke all on function apply_tap(uuid, text, text, boolean, timestamptz) from public, anon, authenticated;
revoke all on function soft_delete_user(uuid)                            from public, anon, authenticated;
revoke all on function reset_occupancy()                                 from public, anon, authenticated;
revoke all on function record_unknown(text, timestamptz)                 from public, anon, authenticated;

-- REALTIME + RLS (no login: anon may READ; only the backend service key writes)
alter publication supabase_realtime add table
  attendance_logs, unknown_cards, alerts, sensor_readings, access_events, audit_log, device_state, users;

alter table users enable row level security;
alter table attendance_logs enable row level security;
alter table unknown_cards enable row level security;
alter table alerts enable row level security;
alter table sensor_readings enable row level security;
alter table access_events enable row level security;
alter table audit_log enable row level security;
alter table device_state enable row level security;

create policy anon_read on users            for select to anon using (true);
create policy anon_read on attendance_logs  for select to anon using (true);
create policy anon_read on unknown_cards    for select to anon using (true);
create policy anon_read on alerts           for select to anon using (true);
create policy anon_read on sensor_readings  for select to anon using (true);
create policy anon_read on access_events    for select to anon using (true);
create policy anon_read on audit_log        for select to anon using (true);
create policy anon_read on device_state     for select to anon using (true);
```

## 8. Config Values (placeholders to tune)

| Key | Default | Module |
|---|---|---|
| `DOOR_RELOCK_DELAY_MS` | 5000 | firmware |
| `HEARTBEAT_INTERVAL_MS` | 5000 | firmware |
| `HTTP_TIMEOUT_MS` | 2000 | firmware |
| `MQ2_WARMUP_MS` | 30000 | firmware |
| `MQ2_SAMPLE_INTERVAL_MS` | 200 | firmware |
| `SMOKE_ALARM_THRESHOLD` / `SMOKE_HYSTERESIS` | calibrate | firmware |
| `FIRE_CONFIRM_MS` / `FIRE_CLEAR_MS` | 1000 / 5000 | firmware |
| `ALERT_UPDATE_MIN_INTERVAL_MS` | 5000 | firmware |
| `OFFLINE_QUEUE_MAX` / `OFFLINE_SYNC_BATCH` | 200 / 50 | firmware |
| `MSG_DISPLAY_MS` / `BUZZER_SHORT_MS` | 2000 / 200 | firmware |
| `KEYPAD_TIMEOUT_MS` | 5000 | firmware |
| `OFFLINE_AFTER_S` | 15 | backend |
| `REGISTRATION_TIMEOUT_S` | 30 | backend |
| `COMMAND_TTL_S` | 30 | backend |
| `SEVERITY_MEDIUM_MIN` / `SEVERITY_HIGH_MIN` | calibrate (above `SMOKE_ALARM_THRESHOLD`) | backend |
| `SENSOR_RETENTION_HOURS` | 24 | backend |

## 9. Known Limitations (accepted for a lab project)
- Remote lock/unlock has up to ~5 s latency (heartbeat polling), and an unlock/lock command that is not delivered within `COMMAND_TTL_S` is dropped.
- No user login means anyone with the dashboard URL can view the data and attempt the passkey (4 digits, no lockout). The device endpoints are also open to anyone who can reach the backend; a shared device key header could be added later.
- Door codes default to `1234`/`4321`, so use them only in a demo environment.
- **Stale offline cache:** a user deleted on the dashboard can still open the door by RFID on an ESP32 that is offline and still holds the old UID cache, until the cache is refreshed.
- Taps recorded in an *earlier* boot while offline get an approximate time (start of the current boot, order preserved), because the ESP32 has no RTC.
- A tap whose response is lost (timeout after the backend already processed it) is queued and can be counted twice.
- Offline taps open the door with a generic "Access Granted (cached)" message because the cache holds UIDs only.
- A fire that starts and clears entirely while offline is reported late; its start time is the time the backend learned of it, and the email is sent then. The email states the severity at the time the alert started; later escalation does not send another email.
- If the ESP32's flash/NVS is erased, `next_seq` restarts at 0. Reset `device_state.last_offline_seq` to 0 in the database at the same time, or new offline events will be ignored.
- Occupancy is a counter and can drift. The admin reset exists for that reason (it closes all open sessions).
- Fire detection is disabled during the `MQ2_WARMUP_MS` after boot.
