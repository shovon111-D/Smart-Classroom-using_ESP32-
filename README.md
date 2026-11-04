# Smart Classroom

A single-room ESP32, FastAPI, Supabase, and desktop dashboard implementation based on [Agent.md](Agent.md).

## Configure Supabase

1. Create or select a Supabase project.
2. Run [`supabase/schema.sql`](supabase/schema.sql) in the Supabase SQL editor. To add the clearly labeled demo rows, run [`supabase/seed.sql`](supabase/seed.sql); it is safe to reapply.
3. Copy `backend/.env.example` to `backend/.env` and fill in the project URL, **service-role** key, door passkeys, dashboard origin, and local IANA time zone. Add SMTP settings if fire-alert email is required. The service-role key belongs only in this backend file.
4. Copy `frontend/.env.example` to `frontend/.env` and set the API URL, Supabase project URL, and **anon/public** key. Never put the service-role key in the frontend. For access over your LAN, use the PC's private IPv4 address in `VITE_API_BASE_URL`.
5. Set `CORS_ORIGINS` to the exact dashboard origin(s), including the PC's LAN address when opening the dashboard from another device.
6. Configure `SEVERITY_MEDIUM_MIN` and `SEVERITY_HIGH_MIN` above the calibrated ESP32 `MQ2_ALARM_THRESHOLD`.

## Run the Backend (Windows PowerShell)

Find the PC's private IPv4 address with `ipconfig`. Put that address in the firmware API URL and, if needed, in the frontend API URL and `CORS_ORIGINS` (for example, `http://192.168.1.10:5173`). The ESP32, PC, and browser must be able to reach each other on the same LAN; allow inbound TCP port 8000 through Windows Firewall, and port 5173 if opening the dashboard from another device. Keep these ports on a trusted private network; do not forward them to the public internet.

From the workspace root, bind the backend to all PC interfaces so the ESP32 can connect (do not use `127.0.0.1` for the device):

```powershell
.\.venv\Scripts\Activate.ps1
python -m pip install -r backend\requirements.txt
Set-Location backend
uvicorn app.main:app --reload --host 0.0.0.0 --port 8000
```

On the PC, the API docs are at `http://127.0.0.1:8000/docs`; other LAN devices use `http://<PC-IPv4>:8000/docs`.

## Run the Dashboard

In a second PowerShell terminal from the workspace root:

```powershell
Set-Location frontend
npm install
npm run dev
```

Open the URL printed by Vite, normally `http://localhost:5173`. Vite is bound to `0.0.0.0` so it can also be opened from the LAN. The dashboard's Live System panel checks backend-to-Supabase connectivity and whether the ESP32 has sent a recent heartbeat (`GET /api/health`). Supabase Realtime must be enabled for the tables added to the `supabase_realtime` publication by the schema. Dashboard writes go to FastAPI; its Supabase client uses only the public anon key.

## ESP32 Source

The firmware is [`firmware/smart_classroom_esp32_simple.cpp`](firmware/smart_classroom_esp32_simple.cpp); use [`firmware/Updated Pin Layouts.md`](firmware/Updated%20Pin%20Layouts.md) for wiring. Tunables and the blank-secret template are in [`firmware/include/config.h`](firmware/include/config.h) and [`firmware/include/secrets.example.h`](firmware/include/secrets.example.h). Copy the template to `firmware/include/secrets.h` and set the Wi-Fi credentials, PC IPv4 API URL, and keypad codes. That local secrets file is ignored by Git. Build it with Arduino IDE or PlatformIO after installing MFRC522, Keypad, ESP32Servo, LiquidCrystal_I2C, and ArduinoJson v7; this workspace does not include an Arduino build manifest/toolchain.

The RC522 reset line is wired directly to 3.3 V, so firmware uses the library's unused-reset-pin sentinel instead of claiming another GPIO. Flame D0 is read as active-low; verify the installed module's comparator polarity. Keep all sensor outputs ESP32-safe, level-shift 5 V LCD I2C pull-ups, power the servo from a separate regulated supply, and join all grounds. The project uses no status LEDs.

## Credential Hygiene

Do not commit real passwords, Supabase keys, connection strings, or SMTP app passwords. Credentials were removed from the project specification; rotate any credentials that were previously stored or shared. The generated `.env.example` files contain no credentials.
