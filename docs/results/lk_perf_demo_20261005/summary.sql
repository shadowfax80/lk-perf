SELECT COUNT(*) AS samples, MIN(ts) AS first_ns, MAX(ts) AS last_ns, (MAX(ts)-MIN(ts))/1e9 AS sample_span_s, COUNT(DISTINCT utid) AS threads FROM cpu_profile_stack_sample;
