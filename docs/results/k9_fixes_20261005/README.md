# K9: dump-record write test (2026-10-05)

Same capture twice per image (`profiler start 1000`, `masktest 4000 500`, dump of about 16,000 records):

| Image | Writes per record | Records lost |
|---|---|---|
| K7 (`dump_loss_k7.log.gz`, image in ../k7_stat_20261005) | 130 printf calls | 34 + 23 of 32,062 (0.18%) |
| K9 (`dump_loss_k9.log.gz`, image here) | 1 | 22 + 32 of 32,061 (0.17%) |

Fragmented output is not what drives the link loss.
