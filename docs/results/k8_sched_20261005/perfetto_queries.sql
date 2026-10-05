select t.name as thread, count(*) as slices, round(sum(s.dur) / 1e6, 3) as cpu_ms
from sched_slice s join thread t using (utid)
group by t.utid order by cpu_ms desc;
select count(*) as wakeups from instant where name = 'sched_wakeup';
select count(*) as freq_samples, min(value) as min_khz, max(value) as max_khz from counter c join cpu_counter_track t on c.track_id = t.id where t.name = 'cpufreq';
select t.name as thread, count(*) as n from thread_state s join thread t using (utid) where s.state = "R" and s.waker_utid is not null group by t.utid order by n desc;
select w.name as waker, count(*) as n from thread_state s join thread w on s.waker_utid = w.utid group by w.utid order by n desc;
