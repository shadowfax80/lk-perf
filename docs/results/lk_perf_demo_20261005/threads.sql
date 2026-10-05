SELECT t.tid,t.name,COUNT(*) AS samples,MIN(s.ts) AS first_ns,MAX(s.ts) AS last_ns FROM cpu_profile_stack_sample s JOIN thread t ON t.utid=s.utid GROUP BY t.tid,t.name ORDER BY t.tid;
