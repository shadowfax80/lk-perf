SELECT (SELECT COUNT(*) FROM sched) AS scheduler_rows,(SELECT COUNT(*) FROM counter) AS counter_rows;
