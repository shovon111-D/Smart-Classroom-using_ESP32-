import { type FormEvent, useEffect, useState } from 'react';
import { createClient, type SupabaseClient } from '@supabase/supabase-js';
import {
  AlertTriangle, ArrowDownLeft, ArrowUpRight, BadgeCheck, Bell, Bolt,
  Check, ChevronRight, Clock3, DoorClosed, DoorOpen, Eye, EyeOff, LockKeyhole, Moon,
  Plus, Radio, RefreshCw, ShieldAlert, Sun, X,
} from 'lucide-react';
import {
  CartesianGrid, Line, LineChart, ResponsiveContainer, Tooltip, XAxis, YAxis,
} from 'recharts';

type AnyRow = Record<string, any>;
type Dialog = { kind: 'register'; role: 'teacher' | 'student' } |
  { kind: 'assign'; id: number } | { kind: 'edit'; user: AnyRow } | null;
type PendingDoor = { action: 'lock' | 'unlock'; initialState: string; started: number };
type Connectivity = {
  backend: 'connected' | 'disconnected';
  supabase: 'connected' | 'disconnected' | 'unknown';
  esp32: 'connected' | 'disconnected' | 'unknown';
  last_seen: string | null;
};

const apiBase = (import.meta.env.VITE_API_BASE_URL || 'http://127.0.0.1:8000').replace(/\/$/, '');
const supabaseUrl = import.meta.env.VITE_SUPABASE_URL;
const anonKey = import.meta.env.VITE_SUPABASE_ANON_KEY;
const realtime: SupabaseClient | null = supabaseUrl && anonKey ? createClient(supabaseUrl, anonKey) : null;

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const response = await fetch(`${apiBase}${path}`, {
    ...init,
    headers: { 'Content-Type': 'application/json', ...init?.headers },
  });
  const body = await response.json().catch(() => ({}));
  if (!response.ok) {
    const detail = typeof body.detail === 'string' ? body.detail : JSON.stringify(body.detail || body);
    throw new Error(detail || `Request failed (${response.status})`);
  }
  return body as T;
}
function localTime(value?: string | null, dateOnly = false) {
  if (!value) return '—';
  const date = new Date(value);
  return new Intl.DateTimeFormat(undefined, dateOnly
    ? { dateStyle: 'medium' }
    : { dateStyle: 'medium', timeStyle: 'short' }).format(date);
}

function duration(seconds: number) {
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  return `${hours}h ${String(minutes).padStart(2, '0')}m`;
}

function initials(name: string) {
  return name.split(/\s+/).slice(0, 2).map(part => part[0]).join('').toUpperCase();
}

function localDateValue(date = new Date()) {
  const offset = date.getTimezoneOffset() * 60000;
  return new Date(date.getTime() - offset).toISOString().slice(0, 10);
}

function App() {
  const [dark, setDark] = useState(() => localStorage.getItem('classroom-theme') === 'dark');
  const [status, setStatus] = useState<AnyRow | null>(null);
  const [connectivity, setConnectivity] = useState<Connectivity>({
    backend: 'disconnected', supabase: 'unknown', esp32: 'unknown', last_seen: null,
  });
  const [users, setUsers] = useState<AnyRow[]>([]);
  const [attendance, setAttendance] = useState<AnyRow[]>([]);
  const [unknown, setUnknown] = useState<AnyRow[]>([]);
  const [alerts, setAlerts] = useState<AnyRow[]>([]);
  const [readings, setReadings] = useState<AnyRow[]>([]);
  const [security, setSecurity] = useState<AnyRow[]>([]);
  const [audit, setAudit] = useState<AnyRow[]>([]);
  const [inside, setInside] = useState<AnyRow[]>([]);
  const [selectedDate, setSelectedDate] = useState(() => localDateValue());
  const [dialog, setDialog] = useState<Dialog>(null);
  const [formName, setFormName] = useState('');
  const [studentId, setStudentId] = useState('');
  const [passkey, setPasskey] = useState('');
  const [passkeyVisible, setPasskeyVisible] = useState(false);
  const [pendingDoor, setPendingDoor] = useState<PendingDoor | null>(null);
  const [tick, setTick] = useState(Date.now());
  const [toast, setToast] = useState('');
  const [loading, setLoading] = useState(true);
  async function refreshDashboard() {
    const dateQuery = encodeURIComponent(selectedDate);
    const result = await Promise.all([
      request<AnyRow>('/api/status'), request<AnyRow[]>('/api/users'),
      request<AnyRow[]>('/api/attendance?limit=30'), request<AnyRow[]>('/api/unknown'),
      request<AnyRow[]>('/api/alerts'), request<AnyRow[]>('/api/sensor-readings?minutes=180'),
      request<AnyRow[]>('/api/security-log?limit=30'), request<AnyRow[]>('/api/audit-log?limit=30'),
      request<AnyRow[]>(`/api/analytics/time-inside?date=${dateQuery}`),
    ]);
    setStatus(result[0]); setUsers(result[1]); setAttendance(result[2]); setUnknown(result[3]);
    setAlerts(result[4]); setReadings(result[5]); setSecurity(result[6]); setAudit(result[7]);
    setInside(result[8]); setLoading(false);
  }

  useEffect(() => {
    let active = true;
    const load = async () => {
      try {
        const dateQuery = encodeURIComponent(selectedDate);
        const result = await Promise.all([
          request<AnyRow>('/api/status'), request<AnyRow[]>('/api/users'),
          request<AnyRow[]>('/api/attendance?limit=30'), request<AnyRow[]>('/api/unknown'),
          request<AnyRow[]>('/api/alerts'), request<AnyRow[]>('/api/sensor-readings?minutes=180'),
          request<AnyRow[]>('/api/security-log?limit=30'), request<AnyRow[]>('/api/audit-log?limit=30'),
          request<AnyRow[]>(`/api/analytics/time-inside?date=${dateQuery}`),
        ]);
        if (!active) return;
        setStatus(result[0]); setUsers(result[1]); setAttendance(result[2]); setUnknown(result[3]);
        setAlerts(result[4]); setReadings(result[5]); setSecurity(result[6]); setAudit(result[7]);
        setInside(result[8]); setLoading(false);
      } catch (error) {
        if (active) { setLoading(false); setToast(error instanceof Error ? error.message : 'Unable to load dashboard'); }
      }
    };
    void load();
    return () => { active = false; };
  }, [selectedDate]);

  useEffect(() => {
    if (!realtime) return;
    let refreshTimer: number | undefined;
    const channel = realtime.channel('classroom-live');
    ['device_state', 'users', 'attendance_logs', 'unknown_cards', 'alerts', 'sensor_readings', 'access_events', 'audit_log']
      .forEach(table => channel.on('postgres_changes', { event: '*', schema: 'public', table }, () => {
        window.clearTimeout(refreshTimer);
        refreshTimer = window.setTimeout(() => { void refreshDashboard().catch(showError); }, 180);
      }));
    channel.subscribe();
    return () => { window.clearTimeout(refreshTimer); void realtime.removeChannel(channel); };
  }, [selectedDate]);

  useEffect(() => {
    if (!toast) return;
    const timer = window.setTimeout(() => setToast(''), 4200);
    return () => window.clearTimeout(timer);
  }, [toast]);

  useEffect(() => {
    const timer = window.setInterval(() => setTick(Date.now()), 1000);
    return () => window.clearInterval(timer);
  }, []);

  useEffect(() => {
    let active = true;
    const checkConnectivity = async () => {
      try {
        const result = await request<Connectivity>('/api/health');
        if (active) setConnectivity(result);
      } catch {
        if (active) {
          setConnectivity({ backend: 'disconnected', supabase: 'unknown', esp32: 'unknown', last_seen: null });
        }
      }
    };
    void checkConnectivity();
    const timer = window.setInterval(() => void checkConnectivity(), 5000);
    return () => {
      active = false;
      window.clearInterval(timer);
    };
  }, []);

  useEffect(() => {
    if (!pendingDoor) return;
    const target = pendingDoor.action === 'unlock'
      ? ['UNLOCKED_TIMED', 'UNLOCKED_LATCHED', 'FIRE_UNLOCKED']
      : ['LOCKED'];
    if (status && target.includes(status.door_state) && status.door_state !== pendingDoor.initialState) {
      setPendingDoor(null);
    } else if (tick - pendingDoor.started >= 30000) {
      setPendingDoor(null);
    }
  }, [tick, pendingDoor, status]);

  function showError(error: unknown) {
    setToast(error instanceof Error ? error.message : 'Action failed');
  }

  async function act(path: string, method = 'POST', body?: AnyRow) {
    try {
      const result = await request<AnyRow>(path, {
        method, ...(body ? { body: JSON.stringify(body) } : {}),
      });
      await refreshDashboard();
      return result;
    } catch (error) { showError(error); throw error; }
  }

  async function sendDoor(action: 'lock' | 'unlock') {
    try {
      await act('/api/door', 'POST', { passkey, action });
      setPasskey('');
      setPasskeyVisible(false);
      setPendingDoor({ action, initialState: status?.door_state || 'LOCKED', started: Date.now() });
      setToast(`${action === 'lock' ? 'Lock' : 'Unlock'} command sent`);
    } catch { /* Error is shown by act. */ }
  }

  function openDialog(next: Dialog) {
    setFormName(next?.kind === 'edit' ? next.user.name : '');
    setStudentId(next?.kind === 'edit' ? next.user.student_id || '' : '');
    setDialog(next);
  }

  async function submitDialog(event: FormEvent) {
    event.preventDefault();
    if (!dialog) return;
    try {
      const role = dialog.kind === 'register' ? dialog.role
        : (document.querySelector('[name="person-role"]') as HTMLSelectElement)?.value;
      const person = { name: formName.trim(), role, student_id: studentId.trim() || null };
      if (dialog.kind === 'register') {
        await act('/api/registration', 'POST', person);
        setToast('Registration is active. Tap the new card at the reader.');
      } else if (dialog.kind === 'assign') {
        await act(`/api/unknown/${dialog.id}/assign`, 'POST', person);
        setToast('Card assigned to the new user');
      } else {
        await act(`/api/users/${dialog.user.id}`, 'PUT', person);
        setToast('User updated');
      }
      setDialog(null);
    } catch { /* Error is shown by act. */ }
  }

  async function deleteUser(user: AnyRow) {
    if (!window.confirm(`Soft-delete ${user.name}? Attendance history will remain.`)) return;
    try { await act(`/api/users/${user.id}`, 'DELETE'); setToast('User removed from the active list'); }
    catch { /* Error is shown by act. */ }
  }

  async function resetOccupancy() {
    if (!window.confirm('Reset occupancy to zero and close every open attendance session?')) return;
    try { await act('/api/occupancy/reset'); setToast('Occupancy reset'); }
    catch { /* Error is shown by act. */ }
  }

  async function cancelRegistration() {
    try { await act('/api/registration', 'DELETE'); setToast('Registration cancelled'); }
    catch { /* Error is shown by act. */ }
  }

  const expiresIn = status?.registration_expires
    ? Math.max(0, Math.ceil((new Date(status.registration_expires).getTime() - tick) / 1000)) : 0;
  const openAlert = alerts.find(alert => !alert.cleared_at);
  const maxReading = Math.max(1, ...readings.map(item => item.mq2_value || 0));
  const lastReading = readings.at(-1)?.mq2_value ?? status?.mq2_value;
  const toggleTheme = () => {
    const next = !dark;
    setDark(next);
    localStorage.setItem('classroom-theme', next ? 'dark' : 'light');
  };

  return (
    <div className={dark ? 'app-shell dark' : 'app-shell'}>
      <main className="workspace" id="top">
        <header className="topbar">
          <div className="crumb"><span>FACILITIES</span><ChevronRight size={14} /><strong>Smart classroom</strong></div>
          <div className="topbar-right">
            <span className="live-indicator"><i /> LIVE SYSTEM</span>
            <span className="topbar-time">{new Intl.DateTimeFormat(undefined, { dateStyle: 'full' }).format(new Date())}</span>
            <button className="theme-toggle" onClick={toggleTheme}
              title={dark ? 'Switch to light appearance' : 'Switch to dark appearance'}
              aria-label={dark ? 'Switch to light appearance' : 'Switch to dark appearance'}>
              {dark ? <Sun size={18} /> : <Moon size={18} />}
            </button>
          </div>
        </header>

        <section className="page-heading" id="overview">
          <div>
            <p className="eyebrow">CONTROL ROOM / 01</p>
            <h1>Classroom status</h1>
            <p className="subheading">Entry, occupancy and environmental signals in one view.</p>
          </div>
          <div className="heading-stamp"><span>DEVICE</span><b>ESP32 · 01</b><small>Single room</small></div>
        </section>

        {toast && <div className="toast" role="status"><AlertTriangle size={16} />{toast}<button onClick={() => setToast('')} title="Dismiss"><X size={15} /></button></div>}

        {!realtime && <div className="setup-callout"><Radio size={16} /> Realtime is not configured. Add the public Supabase URL and anon key to the frontend environment.</div>}
        {loading && <div className="loading-line"><RefreshCw size={15} className="spin" /> Connecting to classroom services…</div>}

        <section className="connectivity-panel" aria-label="Live system connectivity">
          <div className="connectivity-title">
            <span className="status-label">LIVE SYSTEM</span>
            <strong>Connectivity</strong>
          </div>
          <div className="connectivity-check">
            <span className={`signal-dot ${connectivity.backend === 'connected' && connectivity.supabase === 'connected'
              ? 'good' : connectivity.backend === 'disconnected' || connectivity.supabase === 'disconnected' ? 'bad' : ''}`} />
            <span className="connectivity-name">Backend + Supabase</span>
            <b>{connectivity.backend === 'disconnected' ? 'Backend unavailable'
              : connectivity.supabase === 'connected' ? 'Connected'
                : connectivity.supabase === 'disconnected' ? 'Supabase unavailable' : 'Checking'}</b>
          </div>
          <div className="connectivity-check">
            <span className={`signal-dot ${connectivity.backend === 'connected' && connectivity.esp32 === 'connected'
              ? 'good' : connectivity.backend === 'disconnected' || connectivity.esp32 === 'disconnected' ? 'bad' : ''}`} />
            <span className="connectivity-name">Backend + ESP32</span>
            <b>{connectivity.backend === 'disconnected' ? 'Backend unavailable'
              : connectivity.esp32 === 'connected' ? 'Connected'
                : connectivity.esp32 === 'disconnected' ? 'ESP32 offline' : 'Checking'}</b>
            <small>{connectivity.last_seen ? `Last heartbeat ${localTime(connectivity.last_seen)}` : 'No heartbeat received'}</small>
          </div>
        </section>

        <section className="status-ribbon" aria-label="Live device status">
          <div className="status-cell connection-cell">
            <span className="status-label">DEVICE LINK</span>
            <div className="status-value"><i className={status?.online ? 'signal-dot good' : 'signal-dot'} />{status?.online ? 'Online' : 'Offline'}</div>
            <small>{status?.last_seen ? `Seen ${localTime(status.last_seen)}` : 'No heartbeat received'}</small>
          </div>
          <div className="status-cell">
            <span className="status-label">OCCUPANCY</span>
            <div className="status-value occupancy-value">{status?.occupancy ?? '—'} <small>inside</small></div>
            <button className="text-action" onClick={() => void resetOccupancy()}>Reset occupancy</button>
          </div>
          <div className="status-cell">
            <span className="status-label">DOOR STATE</span>
            <div className="status-value"><span className={`door-glyph ${String(status?.door_state || '').includes('UNLOCKED') ? 'open' : ''}`}>
              {String(status?.door_state || '').includes('UNLOCKED') ? <DoorOpen size={17} /> : <DoorClosed size={17} />}
            </span>{status?.door_state?.replaceAll('_', ' ') || '—'}</div>
            <small>Wi-Fi {status?.wifi_rssi != null ? `${status.wifi_rssi} dBm` : '—'}</small>
          </div>
          <div className={`status-cell fire-cell ${status?.fire_active ? 'is-fire' : ''}`}>
            <span className="status-label">FIRE / SMOKE</span>
            <div className="status-value"><i className={status?.fire_active ? 'fire-dot active' : 'fire-dot'} />{status?.fire_active ? 'ALERT ACTIVE' : 'No alert'}</div>
            <small>MQ-2 ADC · {lastReading ?? '—'} / 4095</small>
          </div>
        </section>

        <div className="main-grid">
          <section className="section-panel door-panel" id="door-control">
            <div className="section-heading"><div><p className="eyebrow">REMOTE ACTUATION</p><h2>Door control</h2></div><LockKeyhole size={18} /></div>
            <p className="panel-note">The command is delivered on the next device heartbeat.</p>
            <label className="field-label" htmlFor="passkey">Action passkey</label>
            <div className="passkey-field">
              <input id="passkey" className="text-input passkey-input" type={passkeyVisible ? 'text' : 'password'}
                inputMode="numeric" autoComplete="off" value={passkey}
                onChange={event => setPasskey(event.target.value)} placeholder="Enter passkey" />
              <button type="button" className="passkey-toggle" onClick={() => setPasskeyVisible(value => !value)}
                aria-label={passkeyVisible ? 'Hide passkey' : 'Show passkey'}
                title={passkeyVisible ? 'Hide passkey' : 'Show passkey'}>
                {passkeyVisible ? <EyeOff size={16} /> : <Eye size={16} />}
              </button>
            </div>
            <div className="button-row">
              <button className="button button-quiet" disabled={!passkey} onClick={() => void sendDoor('lock')}><DoorClosed size={16} /> Lock</button>
              <button className="button button-primary" disabled={!passkey} onClick={() => void sendDoor('unlock')}><DoorOpen size={16} /> Unlock</button>
            </div>
            {pendingDoor && <div className="pending-command"><span className="pending-pulse" /> {pendingDoor.action === 'lock' ? 'Lock' : 'Unlock'} pending
              <small> · expires in {Math.max(0, 30 - Math.floor((tick - pendingDoor.started) / 1000))}s</small></div>}
          </section>

          <section className="section-panel sensor-panel" id="alerts">
            <div className="section-heading"><div><p className="eyebrow">ENVIRONMENT / LIVE</p><h2>Smoke &amp; gas</h2></div>
              <div className={`sensor-current ${status?.fire_active ? 'danger' : ''}`}><span>{lastReading ?? '—'}</span><small>ADC</small></div></div>
            {openAlert && <div className="alert-banner"><AlertTriangle size={15} /> Active {openAlert.severity} fire alert <span>Peak {openAlert.peak_value}</span></div>}
            <div className="chart-wrap"><ResponsiveContainer width="100%" height="100%">
              <LineChart data={readings} margin={{ top: 10, right: 5, bottom: 0, left: -18 }}>
                <CartesianGrid strokeDasharray="2 5" vertical={false} stroke="var(--line)" />
                <XAxis dataKey="recorded_at" tickFormatter={value => new Date(value).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}
                  tick={{ fill: 'var(--muted)', fontSize: 10 }} axisLine={false} tickLine={false} minTickGap={35} />
                <YAxis domain={[0, Math.max(maxReading, 1000)]} tick={{ fill: 'var(--muted)', fontSize: 10 }} axisLine={false} tickLine={false} />
                <Tooltip labelFormatter={value => localTime(String(value))} formatter={value => [`${value} ADC`, 'MQ-2']} contentStyle={{ background: 'var(--surface)', border: '1px solid var(--line)', borderRadius: 4, color: 'var(--ink)' }} />
                <Line type="monotone" dataKey="mq2_value" stroke="var(--chart)" strokeWidth={2} dot={false} activeDot={{ r: 3 }} />
              </LineChart>
            </ResponsiveContainer></div>
            <div className="chart-footer"><span><i className="legend-line" /> MQ-2 reading</span><span>Last 3 hours</span></div>
          </section>

          <section className="section-panel attendance-panel" id="attendance">
            <div className="section-heading"><div><p className="eyebrow">DOOR EVENTS</p><h2>Live attendance</h2></div><span className="count-label">{attendance.length} latest</span></div>
            <div className="table-scroll"><table><thead><tr><th>PERSON</th><th>ROLE</th><th>DIRECTION</th><th>TIME</th></tr></thead>
              <tbody>{attendance.map(log => {
                const person = log.users || {};
                return <tr key={log.id}><td><span className="person-cell"><span className="avatar">{initials(person.name || 'U')}</span><span>{person.name || 'Unknown user'}{person.deleted_at ? ' (deleted)' : ''}{log.time_approx && <small className="approx-tag">approx.</small>}</span></span></td>
                  <td><span className="role-label">{person.role || '—'}</span></td>
                  <td><span className={`direction ${log.direction === 'IN' ? 'in' : 'out'}`}>{log.direction === 'IN' ? <ArrowDownLeft size={13} /> : <ArrowUpRight size={13} />}{log.direction}</span></td>
                  <td className="time-cell">{localTime(log.logged_at)}</td></tr>;
              })}{attendance.length === 0 && <tr><td colSpan={4} className="empty-row">No attendance events recorded.</td></tr>}</tbody></table></div>
          </section>

          <div className="incident-stack">
            <section className="section-panel unknown-panel">
              <div className="section-heading"><div><p className="eyebrow">UNASSIGNED UIDS</p><h2>Unknown cards</h2></div><span className="count-label alert-count">{unknown.length} open</span></div>
              <div className="unknown-list">{unknown.map(card => <div className="unknown-row" key={card.id}>
                <div className="uid-block"><code>{card.rfid_uid}</code><small>{card.scan_count} scans · {localTime(card.scanned_at)}</small></div>
                <button className="icon-text-button" onClick={() => openDialog({ kind: 'assign', id: card.id })} title="Assign this card to a new user"><Plus size={15} /> Assign</button>
              </div>)}{unknown.length === 0 && <div className="empty-state"><BadgeCheck size={18} /> No unresolved cards</div>}</div>
            </section>

            <section className="section-panel alerts-history">
              <div className="section-heading"><div><p className="eyebrow">INCIDENT RECORD</p><h2>Fire alerts</h2></div><span className="count-label">{alerts.length} total</span></div>
              <div className="alert-list">{alerts.map(item => <div className="alert-row" key={item.id}>
                <span className={`severity-mark ${item.severity}`}><Bolt size={15} /></span><span className="alert-main"><b>{item.severity} severity</b><small>Peak reading {item.peak_value}</small></span>
                <span className="alert-times"><b>{localTime(item.started_at)}</b><small>{item.cleared_at ? `Cleared ${localTime(item.cleared_at)}` : 'Still active'}</small></span>
                <span className={item.email_sent ? 'mail-state sent' : 'mail-state'}>{item.email_sent ? <Check size={13} /> : <Bell size={13} />}{item.email_sent ? 'Sent' : 'No email'}</span>
              </div>)}{alerts.length === 0 && <div className="empty-state"><Check size={17} /> No fire alerts recorded</div>}</div>
            </section>
          </div>

          <section className="section-panel people-panel" id="people">
            <div className="section-heading"><div><p className="eyebrow">DIRECTORY / {users.length} ACTIVE</p><h2>Users</h2></div><div className="register-actions">
              <button className="button button-quiet small-button" onClick={() => openDialog({ kind: 'register', role: 'teacher' })}><Plus size={14} /> Teacher</button>
              <button className="button button-primary small-button" onClick={() => openDialog({ kind: 'register', role: 'student' })}><Plus size={14} /> Student</button>
            </div></div>
            {status?.registration_active && <div className="registration-banner"><span className="pending-pulse" /> Waiting for card <b>{expiresIn}s</b><button onClick={() => void cancelRegistration()}>Cancel</button></div>}
            <div className="user-list">{users.map(user => <div className="user-row" key={user.id}>
              <span className={`avatar user-avatar ${user.role}`}>{initials(user.name)}</span><span className="user-main"><b>{user.name}</b><small>{user.student_id || 'No student ID'} · <span>{user.role}</span></small></span>
              <code className="user-uid">{user.rfid_uid}</code><button className="small-action" onClick={() => openDialog({ kind: 'edit', user })}>Edit</button>
              <button className="small-action danger-action" onClick={() => void deleteUser(user)}>Delete</button>
            </div>)}{users.length === 0 && <div className="empty-row">No users registered yet.</div>}</div>
            <div className="inside-heading"><div><p className="eyebrow">DAILY PRESENCE</p><h3>Time inside</h3></div>
              <input aria-label="Choose local date for time inside" type="date" value={selectedDate} onChange={event => setSelectedDate(event.target.value)} /></div>
            <div className="inside-list">{inside.map(person => <div className="inside-row" key={person.user_id}>
              <span>{person.name}{person.approx && <small className="approx-tag">approx.</small>}</span><span className="inside-duration">{duration(person.seconds)}{person.currently_inside && <i>IN</i>}</span>
            </div>)}{inside.length === 0 && <div className="empty-state compact-empty">No time inside recorded for this date.</div>}</div>
          </section>

          <section className="section-panel log-panel" id="security">
            <div className="section-heading"><div><p className="eyebrow">ACCESS EVENTS</p><h2>Security log</h2></div><span className="count-label">Recent</span></div>
            <div className="log-list">{security.map(item => <div className="log-row" key={item.id}>
              <span className={`log-symbol ${item.event_type === 'unknown_card' || item.event_type.includes('wrong') ? 'warning' : ''}`}>
                {item.event_type === 'unknown_card' || item.event_type.includes('wrong') ? <ShieldAlert size={15} /> : <LockKeyhole size={15} />}</span>
              <span className="log-description"><b>{item.event_type.replaceAll('_', ' ')}</b><small>{item.source}{item.details?.rfid_uid ? ` · ${item.details.rfid_uid}` : ''}</small></span>
              <time>{localTime(item.created_at)}</time>
            </div>)}{security.length === 0 && <div className="empty-state">No security events recorded.</div>}</div>
          </section>

          <section className="section-panel audit-panel">
            <div className="section-heading"><div><p className="eyebrow">OPERATOR ACTIONS</p><h2>Admin audit</h2></div><span className="count-label">Recent</span></div>
            <div className="audit-list">{audit.map(item => <div className="audit-row" key={item.id}>
              <span className="audit-index">{String(item.id).padStart(4, '0')}</span><b>{item.action.replaceAll('_', ' ')}</b>
              <small>{item.details && Object.keys(item.details).length ? JSON.stringify(item.details) : '—'}</small><time>{localTime(item.created_at)}</time>
            </div>)}{audit.length === 0 && <div className="empty-state">Dashboard actions will appear here.</div>}</div>
          </section>
        </div>

        <footer className="footer-line"><span>SMART CLASSROOM SYSTEM</span><span>API <code>{apiBase}</code></span><span>UTC storage · browser local display</span></footer>
      </main>

      {dialog && <div className="modal-backdrop" role="presentation" onMouseDown={event => { if (event.target === event.currentTarget) setDialog(null); }}>
        <form className="modal" onSubmit={submitDialog}>
          <div className="modal-heading"><div><p className="eyebrow">{dialog.kind === 'register' ? 'CARD REGISTRATION' : dialog.kind === 'assign' ? 'UNKNOWN CARD' : 'USER DETAILS'}</p>
            <h2>{dialog.kind === 'register' ? `Register ${dialog.role}` : dialog.kind === 'assign' ? 'Assign card' : 'Edit user'}</h2></div>
            <button type="button" className="icon-button" onClick={() => setDialog(null)} title="Close"><X size={18} /></button></div>
          {dialog.kind === 'assign' && <div className="modal-uid">UID <code>{unknown.find(card => card.id === dialog.id)?.rfid_uid || '—'}</code></div>}
          <label className="field-label" htmlFor="person-name">Full name</label>
          <input id="person-name" className="text-input" autoFocus required value={formName}
            onChange={event => setFormName(event.target.value)} placeholder="Name" />
          {(dialog.kind === 'assign' || dialog.kind === 'edit') && <><label className="field-label" htmlFor="person-role">Role</label>
            <select id="person-role" name="person-role" className="text-input" defaultValue={dialog.kind === 'edit' ? dialog.user.role : 'student'}>
              <option value="teacher">Teacher</option><option value="student">Student</option>
            </select></>}
          <label className="field-label" htmlFor="student-id">Student ID <span>optional</span></label>
          <input id="student-id" className="text-input" value={studentId} onChange={event => setStudentId(event.target.value)} placeholder="ID number" />
          {dialog.kind === 'register' && <p className="modal-note"><Clock3 size={14} /> The reader waits for one card for 30 seconds.</p>}
          <div className="modal-actions"><button className="button button-quiet" type="button" onClick={() => setDialog(null)}>Cancel</button>
            <button className="button button-primary" type="submit">{dialog.kind === 'register' ? 'Start registration' : dialog.kind === 'assign' ? 'Assign user' : 'Save changes'}</button></div>
        </form>
      </div>}
    </div>
  );
}

export default App;
