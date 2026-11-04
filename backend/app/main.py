from __future__ import annotations

import asyncio
import logging
import re
import smtplib
from contextlib import asynccontextmanager, suppress
from datetime import date, datetime, time, timedelta, timezone
from email.message import EmailMessage
from typing import Any, Literal
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

from fastapi import BackgroundTasks, FastAPI, HTTPException, Query
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from pydantic import BaseModel, Field, field_validator, model_validator
from supabase import Client, create_client

from .config import settings

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("smart_classroom")

_db: Client | None = None


def db() -> Client:
    global _db
    if _db is None:
        if not settings.supabase_url or not settings.supabase_service_key:
            raise HTTPException(status_code=503, detail="Supabase is not configured")
        _db = create_client(settings.supabase_url, settings.supabase_service_key)
    return _db


def rows(response: Any) -> list[dict[str, Any]]:
    return response.data or []


def one(response: Any) -> dict[str, Any] | None:
    found = rows(response)
    return found[0] if found else None


def normalize_uid(value: str) -> str:
    uid = re.sub(r"[:\-\s]", "", value)
    if not uid or len(uid) > 20 or re.fullmatch(r"[0-9A-Fa-f]+", uid) is None or len(uid) % 2:
        raise HTTPException(status_code=422, detail="UID must contain 1 to 10 hexadecimal bytes")
    return uid.upper()


def audit(action: str, details: dict[str, Any] | None = None) -> None:
    db().table("audit_log").insert({"action": action, "details": details or {}}).execute()


def active_registration(state: dict[str, Any] | None, clear_expired: bool = True) -> bool:
    if not state or not state.get("pending_registration"):
        return False
    expires = state.get("registration_expires")
    if expires and datetime.fromisoformat(expires.replace("Z", "+00:00")) > datetime.now(timezone.utc):
        return True
    if clear_expired:
        db().table("device_state").update(
            {"pending_registration": None, "registration_expires": None}
        ).eq("id", 1).execute()
    return False


def get_state() -> dict[str, Any]:
    state = one(db().table("device_state").select("*").eq("id", 1).limit(1).execute())
    if not state:
        raise HTTPException(status_code=503, detail="Device state has not been initialized")
    return state


def map_db_error(exc: Exception) -> HTTPException:
    message = str(exc)
    lowered = message.lower()
    if "duplicate key" in lowered or "unique constraint" in lowered or "already exists" in lowered:
        return HTTPException(status_code=409, detail="A user with that UID or student ID already exists")
    if "user not found" in lowered:
        return HTTPException(status_code=404, detail="User not found")
    logger.exception("Database operation failed")
    return HTTPException(status_code=500, detail="Database operation failed")


def send_alert_email(alert_id: int, severity: str, value: int) -> None:
    required = [settings.smtp_host, settings.smtp_port, settings.smtp_user,
                settings.smtp_app_password, settings.alert_email_from, settings.alert_email_to]
    if not all(required):
        logger.warning("Fire alert email skipped because SMTP settings are incomplete")
        return
    message = EmailMessage()
    message["Subject"] = f"Smart classroom fire alert: {severity}"
    message["From"] = settings.alert_email_from
    message["To"] = settings.alert_email_to
    message.set_content(
        f"A fire/smoke alert started in the smart classroom.\nSeverity: {severity}\nPeak sensor value: {value}"
    )
    try:
        with smtplib.SMTP(settings.smtp_host, settings.smtp_port, timeout=10) as smtp:
            smtp.starttls()
            smtp.login(settings.smtp_user, settings.smtp_app_password)
            smtp.send_message(message)
        db().table("alerts").update({"email_sent": True}).eq("id", alert_id).execute()
    except Exception:
        logger.exception("Unable to send fire alert email")


def schedule_alert_email(background_tasks: BackgroundTasks, result: dict[str, Any] | None) -> None:
    if result and result.get("started_now"):
        background_tasks.add_task(
            send_alert_email, result["id"], result["severity"], result["peak_value"]
        )


def manage_alert(event: str, value: int) -> dict[str, Any] | None:
    result = db().rpc("manage_fire_alert", {
        "p_event": event,
        "p_value": value,
        "p_medium_min": settings.severity_medium_min,
        "p_high_min": settings.severity_high_min,
    }).execute()
    return result.data if isinstance(result.data, dict) else None


class RFIDScan(BaseModel):
    uid: str = Field(min_length=1, max_length=80)


class Heartbeat(BaseModel):
    rssi: int = Field(ge=-127, le=0)
    door_state: Literal["LOCKED", "UNLOCKED_TIMED", "UNLOCKED_LATCHED", "FIRE_UNLOCKED"]
    fire_active: bool
    mq2_value: int = Field(ge=0, le=4095)


class AlertEvent(BaseModel):
    event: Literal["start", "update", "clear"]
    mq2_value: int = Field(ge=0, le=4095)


class KeypadEvent(BaseModel):
    type: Literal["success", "wrong_code"]
    action: Literal["lock", "unlock"] | None = None


class DoorRequest(BaseModel):
    passkey: str = Field(min_length=1, max_length=128)
    action: Literal["lock", "unlock"]


class PersonInput(BaseModel):
    name: str = Field(min_length=1, max_length=120)
    role: Literal["teacher", "student"]
    student_id: str | None = Field(default=None, max_length=80)

    @field_validator("name")
    @classmethod
    def clean_name(cls, value: str) -> str:
        value = value.strip()
        if not value:
            raise ValueError("Name must contain non-whitespace characters")
        return value

    @field_validator("student_id")
    @classmethod
    def clean_student_id(cls, value: str | None) -> str | None:
        value = value.strip() if value else None
        return value or None


class UserUpdate(BaseModel):
    name: str | None = Field(default=None, min_length=1, max_length=120)
    role: Literal["teacher", "student"] | None = None
    student_id: str | None = Field(default=None, max_length=80)

    @field_validator("name")
    @classmethod
    def clean_name(cls, value: str | None) -> str | None:
        if value is None:
            return None
        value = value.strip()
        if not value:
            raise ValueError("Name must contain non-whitespace characters")
        return value

    @field_validator("student_id")
    @classmethod
    def clean_student_id(cls, value: str | None) -> str | None:
        value = value.strip() if value else None
        return value or None


class OfflineEvent(BaseModel):
    seq: int = Field(ge=1)
    kind: Literal["rfid", "keypad"]
    uid: str | None = None
    type: Literal["success", "wrong_code"] | None = None
    action: Literal["lock", "unlock"] | None = None
    boot: int = Field(ge=0)
    t_ms: int = Field(ge=0)

    @model_validator(mode="after")
    def validate_event_shape(self) -> "OfflineEvent":
        if self.kind == "rfid":
            if not self.uid or self.type is not None or self.action is not None:
                raise ValueError("RFID events require only a UID")
        elif self.uid is not None:
            raise ValueError("Keypad events must not include a UID")
        elif self.type == "success" and self.action is None:
            raise ValueError("Successful keypad events require an action")
        elif self.type == "wrong_code" and self.action is not None:
            raise ValueError("Wrong-code events must not include an action")
        elif self.type is None:
            raise ValueError("Keypad events require a type")
        return self


class OfflineSync(BaseModel):
    boot: int = Field(ge=0)
    now_ms: int = Field(ge=0)
    events: list[OfflineEvent] = Field(max_length=50)


async def prune_sensor_readings() -> None:
    while True:
        await asyncio.sleep(3600)
        try:
            cutoff = datetime.now(timezone.utc) - timedelta(hours=settings.sensor_retention_hours)
            db().table("sensor_readings").delete().lt("recorded_at", cutoff.isoformat()).execute()
        except Exception:
            logger.exception("Sensor reading retention cleanup failed")


@asynccontextmanager
async def lifespan(_: FastAPI):
    task = asyncio.create_task(prune_sensor_readings())
    yield
    task.cancel()
    with suppress(asyncio.CancelledError):
        await task


app = FastAPI(title="Smart Classroom API", version="1.0.0", lifespan=lifespan)
app.add_middleware(
    CORSMiddleware,
    allow_origins=settings.allowed_origins,
    allow_credentials=False,
    allow_methods=["GET", "POST", "PUT", "DELETE", "OPTIONS"],
    allow_headers=["Content-Type", "apikey", "Authorization"],
)

@app.get("/api/status")
def status() -> dict[str, Any]:
    state = get_state()
    registration = active_registration(state)
    last_seen = state.get("last_seen")
    online = False
    if last_seen:
        seen = datetime.fromisoformat(last_seen.replace("Z", "+00:00"))
        online = (datetime.now(timezone.utc) - seen).total_seconds() < settings.offline_after_s
    return {
        "id": state["id"], "last_seen": last_seen, "wifi_rssi": state.get("wifi_rssi"),
        "door_state": state["door_state"], "fire_active": state["fire_active"],
        "mq2_value": state.get("mq2_value"), "occupancy": state["occupancy"],
        "pending_command": state.get("pending_command"),
        "pending_command_at": state.get("pending_command_at"),
        "users_version": state["users_version"], "last_offline_seq": state["last_offline_seq"],
        "online": online, "registration_active": registration,
        "registration_expires": state.get("registration_expires") if registration else None,
    }


@app.get("/api/health")
def connectivity_health() -> dict[str, Any]:
    try:
        state = one(db().table("device_state").select("id,last_seen").eq("id", 1)
                    .limit(1).execute())
    except Exception:
        logger.exception("Connectivity check to Supabase failed")
        return {
            "backend": "connected",
            "supabase": "disconnected",
            "esp32": "unknown",
            "last_seen": None,
        }

    last_seen = state.get("last_seen") if state else None
    esp32_online = False
    if last_seen:
        seen = datetime.fromisoformat(last_seen.replace("Z", "+00:00"))
        esp32_online = (datetime.now(timezone.utc) - seen).total_seconds() < settings.offline_after_s

    return {
        "backend": "connected",
        "supabase": "connected",
        "esp32": "connected" if esp32_online else "disconnected",
        "last_seen": last_seen,
    }


@app.post("/api/rfid/scan")
def rfid_scan(payload: RFIDScan) -> Any:
    uid = normalize_uid(payload.uid)
    state = get_state()
    if active_registration(state):
        try:
            result = db().rpc("register_pending_user", {"p_uid": uid}).execute().data
        except Exception as exc:
            raise map_db_error(exc) from exc
        if isinstance(result, dict) and result.get("already_registered"):
            return JSONResponse(status_code=409, content={"result": "already_registered"})
        if isinstance(result, dict) and result.get("registered"):
            return {"result": "registered", "name": result["name"]}
    user = one(db().table("users").select("id,name,role").eq("rfid_uid", uid)
               .is_("deleted_at", "null").limit(1).execute())
    if not user:
        try:
            db().rpc("record_unknown", {"p_uid": uid, "p_at": datetime.now(timezone.utc).isoformat()}).execute()
        except Exception as exc:
            raise map_db_error(exc) from exc
        return JSONResponse(status_code=404, content={"result": "denied"})
    try:
        direction = db().rpc("apply_tap", {
            "p_user_id": user["id"], "p_uid": uid, "p_source": "online",
            "p_time_approx": False, "p_logged_at": datetime.now(timezone.utc).isoformat(),
        }).execute().data
    except Exception as exc:
        raise map_db_error(exc) from exc
    return {"result": "granted", "direction": direction, "name": user["name"], "role": user["role"]}


@app.get("/api/uids")
def list_uids() -> dict[str, Any]:
    state = get_state()
    users = rows(db().table("users").select("rfid_uid").is_("deleted_at", "null").execute())
    return {"version": state["users_version"], "uids": [row["rfid_uid"] for row in users]}


@app.post("/api/heartbeat")
def heartbeat(payload: Heartbeat, background_tasks: BackgroundTasks) -> dict[str, Any]:
    result = db().rpc("record_heartbeat", {
        "p_rssi": payload.rssi, "p_door_state": payload.door_state,
        "p_fire_active": payload.fire_active, "p_mq2_value": payload.mq2_value,
        "p_command_ttl_s": settings.command_ttl_s,
        "p_medium_min": settings.severity_medium_min,
        "p_high_min": settings.severity_high_min,
    }).execute().data
    if isinstance(result, dict):
        schedule_alert_email(background_tasks, result.get("alert"))
        return {
            "command": result.get("command"),
            "users_version": result.get("users_version", 0),
            "registration_active": result.get("registration_active", False),
        }
    raise HTTPException(status_code=500, detail="Heartbeat response was invalid")


@app.post("/api/alert")
def alert(payload: AlertEvent, background_tasks: BackgroundTasks) -> dict[str, bool]:
    schedule_alert_email(background_tasks, manage_alert(payload.event, payload.mq2_value))
    return {"ok": True}


@app.post("/api/keypad-event")
def keypad_event(payload: KeypadEvent) -> dict[str, bool]:
    if payload.type == "success" and payload.action is None:
        raise HTTPException(status_code=422, detail="Successful keypad events require an action")
    event_type = "wrong_code"
    if payload.type == "success":
        event_type = "master_unlock" if payload.action == "unlock" else "master_lock"
    db().table("access_events").insert({"event_type": event_type, "source": "keypad"}).execute()
    return {"ok": True}


@app.post("/api/offline-sync")
def offline_sync(payload: OfflineSync) -> dict[str, int]:
    state = get_state()
    last_seq = int(state["last_offline_seq"])
    now = datetime.now(timezone.utc)
    event_rows = sorted(payload.events, key=lambda event: event.seq)
    sequences = [event.seq for event in event_rows]
    if len(sequences) != len(set(sequences)):
        raise HTTPException(status_code=422, detail="Offline event sequences must be unique")

    prepared_events: list[tuple[OfflineEvent, datetime, bool]] = []
    for event in event_rows:
        if event.seq <= last_seq:
            continue
        if event.boot > payload.boot or (event.boot == payload.boot and event.t_ms > payload.now_ms):
            raise HTTPException(status_code=422, detail=f"Offline event {event.seq} is from the future")
        if event.kind == "rfid":
            try:
                normalize_uid(event.uid or "")
            except HTTPException as exc:
                raise HTTPException(status_code=422, detail=f"Offline event {event.seq} has an invalid UID") from exc
        approximate = event.boot != payload.boot
        event_at = now - timedelta(milliseconds=payload.now_ms) if approximate else (
            now - timedelta(milliseconds=payload.now_ms - event.t_ms)
        )
        prepared_events.append((event, event_at, approximate))

    for event, event_at, approximate in prepared_events:
        try:
            response = db().rpc("record_offline_event", {
                "p_seq": event.seq,
                "p_boot": event.boot,
                "p_kind": event.kind,
                "p_uid": normalize_uid(event.uid) if event.uid else None,
                "p_event_type": event.type,
                "p_action": event.action,
                "p_time_approx": approximate,
                "p_logged_at": event_at.isoformat(),
            }).execute()
        except Exception as exc:
            raise map_db_error(exc) from exc
        last_seq = int(response.data or last_seq)
    return {"last_seq": last_seq}


@app.post("/api/door")
def door(payload: DoorRequest) -> dict[str, bool]:
    expected = settings.door_unlock_passkey if payload.action == "unlock" else settings.door_lock_passkey
    if not expected or payload.passkey != expected:
        db().table("access_events").insert({"event_type": "wrong_passkey", "source": "dashboard"}).execute()
        raise HTTPException(status_code=403, detail="Incorrect passkey for this action")
    now = datetime.now(timezone.utc).isoformat()
    db().table("device_state").update({
        "pending_command": payload.action, "pending_command_at": now
    }).eq("id", 1).execute()
    event = "remote_unlock" if payload.action == "unlock" else "remote_lock"
    db().table("access_events").insert({"event_type": event, "source": "dashboard"}).execute()
    audit(f"door_{payload.action}")
    return {"ok": True}


@app.post("/api/registration")
def start_registration(payload: PersonInput) -> dict[str, Any]:
    student_id = payload.student_id.strip() if payload.student_id else None
    if student_id:
        duplicate = one(db().table("users").select("id").eq("student_id", student_id)
                        .is_("deleted_at", "null").limit(1).execute())
        if duplicate:
            raise HTTPException(status_code=409, detail="Student ID is already in use")
    pending = {"name": payload.name.strip(), "role": payload.role, "student_id": student_id}
    expires = datetime.now(timezone.utc) + timedelta(seconds=settings.registration_timeout_s)
    db().table("device_state").update({
        "pending_registration": pending, "registration_expires": expires.isoformat()
    }).eq("id", 1).execute()
    audit("registration_start", pending)
    return {"ok": True, "registration_expires": expires.isoformat()}


@app.delete("/api/registration")
def cancel_registration() -> dict[str, bool]:
    db().table("device_state").update({
        "pending_registration": None, "registration_expires": None
    }).eq("id", 1).execute()
    audit("registration_cancel")
    return {"ok": True}


@app.get("/api/users")
def list_users() -> list[dict[str, Any]]:
    return rows(db().table("users").select("id,name,role,student_id,rfid_uid,created_at,updated_at")
                .is_("deleted_at", "null").order("name").execute())


@app.put("/api/users/{user_id}")
def update_user(user_id: str, payload: UserUpdate) -> dict[str, Any]:
    changes = payload.model_dump(exclude_unset=True)
    if not changes:
        raise HTTPException(status_code=422, detail="At least one field is required")
    if changes.get("name") is not None:
        changes["name"] = changes["name"].strip()
    if "student_id" in changes and changes["student_id"]:
        changes["student_id"] = changes["student_id"].strip()
    exists = one(db().table("users").select("id").eq("id", user_id)
                 .is_("deleted_at", "null").limit(1).execute())
    if not exists:
        raise HTTPException(status_code=404, detail="User not found")
    try:
        updated = one(db().table("users").update(changes).eq("id", user_id)
                      .is_("deleted_at", "null").execute())
    except Exception as exc:
        raise map_db_error(exc) from exc
    audit("user_update", {"user_id": user_id, "fields": list(changes)})
    return updated or {"ok": True}


@app.delete("/api/users/{user_id}")
def delete_user(user_id: str) -> dict[str, bool]:
    try:
        db().rpc("soft_delete_user", {"p_user_id": user_id}).execute()
    except Exception as exc:
        raise map_db_error(exc) from exc
    audit("user_delete", {"user_id": user_id})
    return {"ok": True}


@app.get("/api/unknown")
def list_unknown() -> list[dict[str, Any]]:
    return rows(db().table("unknown_cards").select("id,rfid_uid,scan_count,scanned_at")
                .is_("resolved_at", "null").order("scanned_at", desc=True).execute())


@app.post("/api/unknown/{unknown_id}/assign")
def assign_unknown(unknown_id: int, payload: PersonInput) -> dict[str, Any]:
    try:
        result = db().rpc("assign_unknown_card", {
            "p_unknown_id": unknown_id, "p_name": payload.name.strip(),
            "p_role": payload.role,
            "p_student_id": payload.student_id.strip() if payload.student_id else None,
        }).execute().data
    except Exception as exc:
        raise map_db_error(exc) from exc
    if not isinstance(result, dict) or not result.get("id"):
        raise HTTPException(status_code=404, detail="Unknown card not found or already resolved")
    return result


@app.post("/api/occupancy/reset")
def reset_occupancy() -> dict[str, bool]:
    db().rpc("reset_occupancy").execute()
    audit("occupancy_reset")
    return {"ok": True}


@app.get("/api/attendance")
def attendance(limit: int = Query(default=50, ge=1, le=500)) -> list[dict[str, Any]]:
    return rows(db().table("attendance_logs").select(
        "id,user_id,rfid_uid,direction,source,time_approx,logged_at,users(name,role,deleted_at)"
    ).order("id", desc=True).limit(limit).execute())


@app.get("/api/analytics/time-inside")
def time_inside(day: date | None = Query(default=None, alias="date")) -> list[dict[str, Any]]:
    try:
        local_zone = ZoneInfo(settings.local_tz)
    except ZoneInfoNotFoundError as exc:
        raise HTTPException(status_code=500, detail="LOCAL_TZ is not a valid IANA time zone") from exc
    selected = day or datetime.now(local_zone).date()
    start_local = datetime.combine(selected, time.min, local_zone)
    end_local = datetime.combine(selected + timedelta(days=1), time.min, local_zone)
    window_start = start_local.astimezone(timezone.utc)
    window_end = end_local.astimezone(timezone.utc)
    now = datetime.now(timezone.utc)
    people = rows(db().table("users").select("id,name,role,deleted_at").execute())
    logs = rows(db().table("attendance_logs").select(
        "id,user_id,direction,time_approx,logged_at"
    ).order("id").execute())
    by_user: dict[str, list[dict[str, Any]]] = {}
    for log in logs:
        by_user.setdefault(log["user_id"], []).append(log)
    output = []
    for person in people:
        total = 0.0
        approximate = False
        current = False
        opened: dict[str, Any] | None = None
        for log in by_user.get(person["id"], []):
            logged = datetime.fromisoformat(log["logged_at"].replace("Z", "+00:00"))
            if log["direction"] == "IN":
                opened = {"at": logged, "approx": log["time_approx"]}
            elif opened:
                end = logged
                clipped_start = max(opened["at"], window_start)
                clipped_end = min(end, window_end)
                if clipped_end > clipped_start:
                    total += (clipped_end - clipped_start).total_seconds()
                    approximate = approximate or opened["approx"] or log["time_approx"]
                opened = None
        if opened:
            current = True
            clipped_start = max(opened["at"], window_start)
            clipped_end = min(now, window_end)
            if clipped_end > clipped_start:
                total += (clipped_end - clipped_start).total_seconds()
                approximate = approximate or opened["approx"]
        if total > 0 or current:
            output.append({
                "user_id": person["id"],
                "name": person["name"] + (" (deleted)" if person["deleted_at"] else ""),
                "role": person["role"], "seconds": round(total),
                "approx": approximate, "currently_inside": current,
            })
    return sorted(output, key=lambda item: item["name"].lower())


@app.get("/api/alerts")
def list_alerts(limit: int = Query(default=100, ge=1, le=500)) -> list[dict[str, Any]]:
    return rows(db().table("alerts").select("*").order("started_at", desc=True).limit(limit).execute())


@app.get("/api/sensor-readings")
def sensor_readings(minutes: int = Query(default=60, ge=1, le=1440)) -> list[dict[str, Any]]:
    since = datetime.now(timezone.utc) - timedelta(minutes=minutes)
    return rows(db().table("sensor_readings").select("id,mq2_value,recorded_at")
                .gte("recorded_at", since.isoformat()).order("recorded_at").execute())


@app.get("/api/security-log")
def security_log(limit: int = Query(default=100, ge=1, le=500)) -> list[dict[str, Any]]:
    unknown = rows(db().table("unknown_cards").select("id,rfid_uid,scan_count,scanned_at,resolved_at")
                   .order("scanned_at", desc=True).limit(limit).execute())
    access = rows(db().table("access_events").select("id,event_type,source,created_at")
                  .in_("event_type", ["wrong_code", "master_unlock", "master_lock", "wrong_passkey"])
                  .order("created_at", desc=True).limit(limit).execute())
    merged = [{"id": f"unknown-{item['id']}", "event_type": "unknown_card", "source": "rfid",
               "created_at": item["scanned_at"], "details": item} for item in unknown]
    merged.extend({"id": f"access-{item['id']}", "event_type": item["event_type"],
                   "source": item["source"], "created_at": item["created_at"], "details": {}}
                  for item in access)
    return sorted(merged, key=lambda item: item["created_at"], reverse=True)[:limit]


@app.get("/api/audit-log")
def audit_log(limit: int = Query(default=100, ge=1, le=500)) -> list[dict[str, Any]]:
    return rows(db().table("audit_log").select("id,action,details,created_at")
                .order("id", desc=True).limit(limit).execute())


@app.get("/health")
def health() -> dict[str, str]:
    return {"status": "ok"}
