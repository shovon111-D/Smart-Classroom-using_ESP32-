create extension if not exists pgcrypto;

create table if not exists users (
  id uuid primary key default gen_random_uuid(),
  name text not null,
  role text not null check (role in ('teacher','student')),
  student_id text,
  rfid_uid text not null,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),
  deleted_at timestamptz
);
create unique index if not exists users_rfid_active_uniq on users(rfid_uid) where deleted_at is null;
create unique index if not exists users_student_active_uniq on users(student_id) where deleted_at is null and student_id is not null;

create or replace function set_updated_at() returns trigger language plpgsql as $$
begin new.updated_at = now(); return new; end;
$$;
drop trigger if exists users_set_updated_at on users;
create trigger users_set_updated_at before update on users for each row execute function set_updated_at();

create table if not exists attendance_logs (
  id bigint generated always as identity primary key,
  user_id uuid not null references users(id) on delete restrict,
  rfid_uid text not null,
  direction text not null check (direction in ('IN','OUT')),
  source text not null default 'online' check (source in ('online','offline_sync')),
  time_approx boolean not null default false,
  logged_at timestamptz not null default now()
);
create index if not exists attendance_user_id on attendance_logs(user_id, id desc);
create index if not exists attendance_time on attendance_logs(logged_at desc);

create table if not exists unknown_cards (
  id bigint generated always as identity primary key,
  rfid_uid text not null,
  scan_count int not null default 1,
  scanned_at timestamptz not null default now(),
  resolved_at timestamptz
);
create unique index if not exists unknown_unresolved_uniq on unknown_cards(rfid_uid) where resolved_at is null;

create table if not exists alerts (
  id bigint generated always as identity primary key,
  type text not null default 'fire',
  severity text not null check (severity in ('low','medium','high')),
  peak_value int not null,
  started_at timestamptz not null default now(),
  cleared_at timestamptz,
  email_sent boolean not null default false
);
create unique index if not exists alerts_one_open on alerts(type) where cleared_at is null;

create table if not exists sensor_readings (
  id bigint generated always as identity primary key,
  mq2_value int not null,
  recorded_at timestamptz not null default now()
);
create index if not exists sensor_time on sensor_readings(recorded_at desc);

create table if not exists access_events (
  id bigint generated always as identity primary key,
  event_type text not null check (event_type in
    ('wrong_code','master_unlock','master_lock','remote_unlock','remote_lock','wrong_passkey')),
  source text not null check (source in ('keypad','dashboard')),
  created_at timestamptz not null default now()
);

create table if not exists audit_log (
  id bigint generated always as identity primary key,
  action text not null,
  details jsonb,
  created_at timestamptz not null default now()
);

create table if not exists device_state (
  id int primary key default 1 check (id = 1),
  last_seen timestamptz,
  wifi_rssi int,
  door_state text not null default 'LOCKED'
    check (door_state in ('LOCKED','UNLOCKED_TIMED','UNLOCKED_LATCHED','FIRE_UNLOCKED')),
  fire_active boolean not null default false,
  mq2_value int,
  occupancy int not null default 0 check (occupancy >= 0),
  pending_command text check (pending_command in ('lock','unlock')),
  pending_command_at timestamptz,
  pending_registration jsonb,
  registration_expires timestamptz,
  users_version int not null default 0,
  last_offline_seq bigint not null default 0
);
insert into device_state(id) values (1) on conflict (id) do nothing;

create or replace function bump_users_version() returns trigger language plpgsql as $$
begin update device_state set users_version = users_version + 1 where id = 1; return null; end;
$$;
drop trigger if exists users_changed on users;
create trigger users_changed after insert or update or delete on users for each statement execute function bump_users_version();

create or replace function apply_tap(p_user_id uuid, p_uid text, p_source text,
  p_time_approx boolean, p_logged_at timestamptz) returns text
language plpgsql security definer set search_path = public as $$
declare v_last text; v_dir text;
begin
  perform 1 from device_state where id = 1 for update;
  select direction into v_last from attendance_logs where user_id = p_user_id order by id desc limit 1;
  v_dir := case when v_last = 'IN' then 'OUT' else 'IN' end;
  insert into attendance_logs(user_id, rfid_uid, direction, source, time_approx, logged_at)
    values (p_user_id, p_uid, v_dir, p_source, p_time_approx, p_logged_at);
  update device_state set occupancy = case when v_dir = 'IN' then occupancy + 1 else greatest(occupancy - 1, 0) end where id = 1;
  return v_dir;
end;
$$;

create or replace function soft_delete_user(p_user_id uuid) returns void
language plpgsql security definer set search_path = public as $$
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
$$;

create or replace function reset_occupancy() returns void
language plpgsql security definer set search_path = public as $$
begin
  perform 1 from device_state where id = 1 for update;
  insert into attendance_logs(user_id, rfid_uid, direction)
    select user_id, rfid_uid, 'OUT' from (
      select distinct on (user_id) user_id, rfid_uid, direction from attendance_logs order by user_id, id desc
    ) last_log where direction = 'IN';
  update device_state set occupancy = 0 where id = 1;
end;
$$;

create or replace function record_unknown(p_uid text, p_at timestamptz default now()) returns void
language plpgsql security definer set search_path = public as $$
begin
  insert into unknown_cards(rfid_uid, scanned_at) values (p_uid, p_at)
  on conflict (rfid_uid) where resolved_at is null
  do update set scanned_at = excluded.scanned_at, scan_count = unknown_cards.scan_count + 1;
end;
$$;

create or replace function record_offline_event(
  p_seq bigint, p_boot bigint, p_kind text, p_uid text, p_event_type text,
  p_action text, p_time_approx boolean, p_logged_at timestamptz
) returns bigint
language plpgsql security definer set search_path = public as $$
declare v_state device_state%rowtype; v_user_id uuid;
begin
  if p_seq < 1 or p_boot < 0 then raise exception 'invalid offline sequence'; end if;
  select * into v_state from device_state where id = 1 for update;
  if p_seq <= v_state.last_offline_seq then return v_state.last_offline_seq; end if;

  if p_kind = 'rfid' then
    if p_uid is null or p_event_type is not null or p_action is not null then
      raise exception 'invalid offline RFID event';
    end if;
    select id into v_user_id from users where rfid_uid = p_uid and deleted_at is null limit 1;
    if found then
      perform apply_tap(v_user_id, p_uid, 'offline_sync', p_time_approx, p_logged_at);
    else
      perform record_unknown(p_uid, p_logged_at);
    end if;
  elsif p_kind = 'keypad' then
    if p_uid is not null or p_event_type is null or p_event_type not in ('wrong_code', 'success') or
       (p_event_type = 'wrong_code' and p_action is not null) or
       (p_event_type = 'success' and (p_action is null or p_action not in ('lock', 'unlock'))) then
      raise exception 'invalid offline keypad event';
    end if;
    insert into access_events(event_type, source, created_at)
    values (
      case when p_event_type = 'wrong_code' then 'wrong_code'
           when p_action = 'unlock' then 'master_unlock' else 'master_lock' end,
      'keypad', p_logged_at
    );
  else
    raise exception 'invalid offline event kind';
  end if;

  update device_state set last_offline_seq = p_seq where id = 1;
  return p_seq;
end;
$$;

create or replace function register_pending_user(p_uid text) returns jsonb
language plpgsql security definer set search_path = public as $$
declare v_state device_state%rowtype; v_user users%rowtype;
begin
  select * into v_state from device_state where id = 1 for update;
  if v_state.pending_registration is null or v_state.registration_expires <= now() then
    update device_state set pending_registration = null, registration_expires = null where id = 1;
    return '{}'::jsonb;
  end if;
  if exists(select 1 from users where rfid_uid = p_uid and deleted_at is null) then
    return jsonb_build_object('already_registered', true);
  end if;
  insert into users(name, role, student_id, rfid_uid)
  values (v_state.pending_registration->>'name', v_state.pending_registration->>'role',
          nullif(v_state.pending_registration->>'student_id',''), p_uid)
  returning * into v_user;
  update unknown_cards set resolved_at = now() where rfid_uid = p_uid and resolved_at is null;
  update device_state set pending_registration = null, registration_expires = null where id = 1;
  insert into audit_log(action, details) values ('register_user', jsonb_build_object('source','registration','user_id',v_user.id));
  return jsonb_build_object('registered', true, 'name', v_user.name);
end;
$$;

create or replace function assign_unknown_card(p_unknown_id bigint, p_name text, p_role text, p_student_id text)
returns jsonb language plpgsql security definer set search_path = public as $$
declare v_unknown unknown_cards%rowtype; v_user users%rowtype;
begin
  perform 1 from device_state where id = 1 for update;
  select * into v_unknown from unknown_cards where id = p_unknown_id and resolved_at is null for update;
  if not found then return '{}'::jsonb; end if;
  insert into users(name, role, student_id, rfid_uid)
  values (p_name, p_role, p_student_id, v_unknown.rfid_uid) returning * into v_user;
  update unknown_cards set resolved_at = now() where id = p_unknown_id;
  insert into audit_log(action, details) values ('register_user', jsonb_build_object('source','assign','user_id',v_user.id,'unknown_id',p_unknown_id));
  return jsonb_build_object('id',v_user.id,'name',v_user.name,'role',v_user.role,'student_id',v_user.student_id,'rfid_uid',v_user.rfid_uid);
end;
$$;

create or replace function manage_fire_alert(p_event text, p_value int, p_medium_min int, p_high_min int)
returns jsonb language plpgsql security definer set search_path = public as $$
declare v_alert alerts%rowtype; v_severity text; v_started boolean := false;
begin
  perform 1 from device_state where id = 1 for update;
  select * into v_alert from alerts where type = 'fire' and cleared_at is null order by id desc limit 1 for update;
  if p_event = 'clear' then
    if found then update alerts set cleared_at = now() where id = v_alert.id; end if;
    return null;
  end if;
  v_severity := case when p_value >= p_high_min then 'high' when p_value >= p_medium_min then 'medium' else 'low' end;
  if not found and p_event = 'start' then
    insert into alerts(type,severity,peak_value) values ('fire',v_severity,p_value) returning * into v_alert;
    v_started := true;
  elsif found and p_value > v_alert.peak_value then
    update alerts set peak_value = p_value, severity = v_severity where id = v_alert.id returning * into v_alert;
  end if;
  if not found and not v_started then return null; end if;
  return jsonb_build_object('id',v_alert.id,'severity',v_alert.severity,'peak_value',v_alert.peak_value,'started_now',v_started);
end;
$$;

drop function if exists record_heartbeat(integer, text, boolean, integer, integer, integer, integer, integer);
create or replace function record_heartbeat(p_rssi int, p_door_state text, p_fire_active boolean,
  p_mq2_value int,
  p_command_ttl_s int, p_medium_min int, p_high_min int) returns jsonb
language plpgsql security definer set search_path = public as $$
declare v_state device_state%rowtype; v_command text; v_registration boolean; v_alert jsonb;
begin
  select * into v_state from device_state where id = 1 for update;
  update device_state set last_seen = now(), wifi_rssi = p_rssi, door_state = p_door_state,
    fire_active = p_fire_active, mq2_value = p_mq2_value where id = 1;
  insert into sensor_readings(mq2_value) values (p_mq2_value);
  if p_fire_active then
    v_alert := manage_fire_alert('start',p_mq2_value,p_medium_min,p_high_min);
  else
    v_alert := manage_fire_alert('clear',p_mq2_value,p_medium_min,p_high_min);
  end if;
  if v_state.pending_command is not null and v_state.pending_command_at >= now() - make_interval(secs => p_command_ttl_s) then
    v_command := v_state.pending_command;
  end if;
  update device_state set pending_command = null, pending_command_at = null where id = 1 and pending_command is not null;
  if v_state.pending_registration is not null and v_state.registration_expires > now() then
    v_registration := true;
  else
    v_registration := false;
    if v_state.pending_registration is not null then
      update device_state set pending_registration = null, registration_expires = null where id = 1;
    end if;
  end if;
  select * into v_state from device_state where id = 1;
  return jsonb_build_object('command',v_command,'users_version',v_state.users_version,
    'registration_active',v_registration,'alert',v_alert);
end;
$$;

revoke all on function apply_tap(uuid,text,text,boolean,timestamptz) from public, anon, authenticated;
revoke all on function soft_delete_user(uuid) from public, anon, authenticated;
revoke all on function reset_occupancy() from public, anon, authenticated;
revoke all on function record_unknown(text,timestamptz) from public, anon, authenticated;
revoke all on function record_offline_event(bigint,bigint,text,text,text,text,boolean,timestamptz) from public, anon, authenticated;
revoke all on function register_pending_user(text) from public, anon, authenticated;
revoke all on function assign_unknown_card(bigint,text,text,text) from public, anon, authenticated;
revoke all on function manage_fire_alert(text,int,int,int) from public, anon, authenticated;
revoke all on function record_heartbeat(int,text,boolean,int,int,int,int) from public, anon, authenticated;
grant execute on function apply_tap(uuid,text,text,boolean,timestamptz) to service_role;
grant execute on function soft_delete_user(uuid) to service_role;
grant execute on function reset_occupancy() to service_role;
grant execute on function record_unknown(text,timestamptz) to service_role;
grant execute on function record_offline_event(bigint,bigint,text,text,text,text,boolean,timestamptz) to service_role;
grant execute on function register_pending_user(text) to service_role;
grant execute on function assign_unknown_card(bigint,text,text,text) to service_role;
grant execute on function manage_fire_alert(text,int,int,int) to service_role;
grant execute on function record_heartbeat(int,text,boolean,int,int,int,int) to service_role;

do $$
begin
  alter table users enable row level security;
  alter table attendance_logs enable row level security;
  alter table unknown_cards enable row level security;
  alter table alerts enable row level security;
  alter table sensor_readings enable row level security;
  alter table access_events enable row level security;
  alter table audit_log enable row level security;
  alter table device_state enable row level security;
end;
$$;

drop policy if exists anon_read on users;
create policy anon_read on users for select to anon using (true);
drop policy if exists anon_read on attendance_logs;
create policy anon_read on attendance_logs for select to anon using (true);
drop policy if exists anon_read on unknown_cards;
create policy anon_read on unknown_cards for select to anon using (true);
drop policy if exists anon_read on alerts;
create policy anon_read on alerts for select to anon using (true);
drop policy if exists anon_read on sensor_readings;
create policy anon_read on sensor_readings for select to anon using (true);
drop policy if exists anon_read on access_events;
create policy anon_read on access_events for select to anon using (true);
drop policy if exists anon_read on audit_log;
create policy anon_read on audit_log for select to anon using (true);
drop policy if exists anon_read on device_state;
create policy anon_read on device_state for select to anon using (true);
drop policy if exists authenticated_read on users;
drop policy if exists authenticated_read on attendance_logs;
drop policy if exists authenticated_read on unknown_cards;
drop policy if exists authenticated_read on alerts;
drop policy if exists authenticated_read on sensor_readings;
drop policy if exists authenticated_read on access_events;
drop policy if exists authenticated_read on audit_log;
drop policy if exists authenticated_read on device_state;
grant select on users, attendance_logs, unknown_cards, alerts, sensor_readings,
  access_events, audit_log, device_state to anon;

do $$
begin
  if not exists (select 1 from pg_publication_tables where pubname = 'supabase_realtime' and tablename = 'attendance_logs') then
    alter publication supabase_realtime add table attendance_logs;
  end if;
  if not exists (select 1 from pg_publication_tables where pubname = 'supabase_realtime' and tablename = 'unknown_cards') then
    alter publication supabase_realtime add table unknown_cards;
  end if;
  if not exists (select 1 from pg_publication_tables where pubname = 'supabase_realtime' and tablename = 'alerts') then
    alter publication supabase_realtime add table alerts;
  end if;
  if not exists (select 1 from pg_publication_tables where pubname = 'supabase_realtime' and tablename = 'sensor_readings') then
    alter publication supabase_realtime add table sensor_readings;
  end if;
  if not exists (select 1 from pg_publication_tables where pubname = 'supabase_realtime' and tablename = 'access_events') then
    alter publication supabase_realtime add table access_events;
  end if;
  if not exists (select 1 from pg_publication_tables where pubname = 'supabase_realtime' and tablename = 'audit_log') then
    alter publication supabase_realtime add table audit_log;
  end if;
  if not exists (select 1 from pg_publication_tables where pubname = 'supabase_realtime' and tablename = 'device_state') then
    alter publication supabase_realtime add table device_state;
  end if;
  if not exists (select 1 from pg_publication_tables where pubname = 'supabase_realtime' and tablename = 'users') then
    alter publication supabase_realtime add table users;
  end if;
end;
$$;
