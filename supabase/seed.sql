begin;

insert into users(id, name, role, student_id, rfid_uid) values
  ('d0000000-0000-4000-8000-000000000001', 'Demo Instructor', 'teacher', 'DEMO-T-001', 'D3A00001'),
  ('d0000000-0000-4000-8000-000000000002', 'Demo Student 01', 'student', 'DEMO-S-001', 'D3A00002'),
  ('d0000000-0000-4000-8000-000000000003', 'Demo Student 02', 'student', 'DEMO-S-002', 'D3A00003')
on conflict (id) do nothing;

do $$
begin
  if not exists (
    select 1 from attendance_logs where user_id = 'd0000000-0000-4000-8000-000000000001'
  ) then
    insert into attendance_logs(user_id, rfid_uid, direction, source, logged_at) values
      ('d0000000-0000-4000-8000-000000000001', 'D3A00001', 'IN', 'online', now() - interval '60 minutes'),
      ('d0000000-0000-4000-8000-000000000001', 'D3A00001', 'OUT', 'online', now() - interval '45 minutes');
  end if;
  if not exists (
    select 1 from attendance_logs where user_id = 'd0000000-0000-4000-8000-000000000002'
  ) then
    insert into attendance_logs(user_id, rfid_uid, direction, source, logged_at) values
      ('d0000000-0000-4000-8000-000000000002', 'D3A00002', 'IN', 'online', now() - interval '30 minutes'),
      ('d0000000-0000-4000-8000-000000000002', 'D3A00002', 'OUT', 'online', now() - interval '20 minutes');
  end if;
  if not exists (
    select 1 from attendance_logs where user_id = 'd0000000-0000-4000-8000-000000000003'
  ) then
    insert into attendance_logs(user_id, rfid_uid, direction, source, logged_at)
    values ('d0000000-0000-4000-8000-000000000003', 'D3A00003', 'IN', 'online', now() - interval '5 minutes');
  end if;
end;
$$;

insert into unknown_cards(rfid_uid, scan_count, scanned_at)
select 'D3A0FFFF', 2, now() - interval '2 minutes'
where not exists (
  select 1 from unknown_cards where rfid_uid = 'D3A0FFFF' and resolved_at is null
);

do $$
begin
  if not exists (select 1 from sensor_readings) then
    insert into sensor_readings(mq2_value, recorded_at) values
      (360, now() - interval '4 minutes'),
      (382, now() - interval '3 minutes'),
      (374, now() - interval '2 minutes'),
      (401, now() - interval '1 minute'),
      (390, now());
  end if;
  if not exists (select 1 from access_events) then
    insert into access_events(event_type, source, created_at)
    values ('wrong_code', 'keypad', now() - interval '3 minutes');
  end if;
  if not exists (select 1 from audit_log) then
    insert into audit_log(action, details)
    values ('registration_start', jsonb_build_object('demo', true, 'name', 'Demo Instructor'));
  end if;
end;
$$;

select 1 from device_state where id = 1 for update;
update device_state
set occupancy = (
  select count(*)
  from (
    select distinct on (attendance_logs.user_id) attendance_logs.user_id, attendance_logs.direction
    from attendance_logs
    join users on users.id = attendance_logs.user_id
    where users.deleted_at is null
    order by attendance_logs.user_id, attendance_logs.id desc
  ) last_direction
  where last_direction.direction = 'IN'
)
where id = 1;

commit;
