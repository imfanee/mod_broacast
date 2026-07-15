# mod_broadcast — production hardening handoff

This pass focused on **operator visibility**, **config/reload safety**, **listener→speaker promotion without hangup**, **recording edge cases**, **producer wall-clock drift observability**, **graceful destroy**, **heartbeat / wedge detection**, and **expanded CLI output**.

## Files touched (approximate)

| File | Nature of change |
|------|------------------|
| `mod_broadcast.h` | Lock-rank doc; `find_controls_rdlocked`; drift uses `stat_ticks` (no `producer_tick_idx`); histogram optional |
| `mod_broadcast.c` | `module_load_us`; promotion path; `hash_rwlock` + `config_rwlock` destroy on shutdown |
| `broadcast_config.c` | `broadcast_config_clone_profile` Doxygen + lock contract; `find_controls_rdlocked` |
| `broadcast_listener.c` | DTMF control match under `config_rwlock` |
| `broadcast_core.c` | Wedged listeners: `listener_lock` **wrlock**, `MFLAG_KICKED`, `strdup` UUID batch, hangup; destroy causes; auto-record |
| `broadcast_producer.c` | Wall-clock drift vs `producer_first_tick_us + stat_ticks*interval`; `producer-stall` event coalesce |
| `conf/autoload_configs/broadcast.conf.xml` | `heartbeat-interval-sec` sample |
| `README.md`, `TESTING.md`, `HANDOFF.md`, `examples/*` | Operator docs and samples |

Line counts: use `git diff --stat` on your branch.

## New / clarified event subclasses

(Reserved in `broadcast_event_reserve_all` / freed in `broadcast_event_free_all` — verify in `broadcast_events.c`.)

- `broadcast::speaker-request`
- `broadcast::recording-failed`
- `broadcast::producer-stall`
- `broadcast::heartbeat`

## New profile parameters

- `heartbeat-interval-sec` (default commonly 60 in sample XML; `0` disables heartbeat events)

## API / CLI additions

- `broadcast <name> destroy [grace_ms] [announcement_file]` → `broadcast_destroy_ex`
- `broadcast <name> listener <member_id>` — detail (text + JSON)
- `broadcast stats` / `broadcast --json stats` — module aggregates
- `broadcast <name> producer` JSON includes missed ticks + max drift
- `broadcast <name> producer histogram` — when built with `BROADCAST_ENABLE_HISTOGRAM`

## Hangup causes (audit snapshot)

- Mass destroy of listeners: `SWITCH_CAUSE_SERVICE_UNAVAILABLE` (FreeSWITCH 1.10 lacks `SWITCH_CAUSE_ADMINISTRATIVE_BLOCK`; see DESIGN NOTE in `broadcast_core.c`).
- Kicked listener: `SWITCH_CAUSE_NORMAL_CLEARING`
- Wedged listener: `SWITCH_CAUSE_MEDIA_TIMEOUT`
- Caps / speaker collision / listener add failure: `SWITCH_CAUSE_USER_BUSY` where applicable

## DESIGN NOTE deviations from original brief text

1. **Administrative hangup cause:** implemented as `SERVICE_UNAVAILABLE` instead of non-existent `ADMINISTRATIVE_BLOCK`.
2. **Full V1–V4 metrics:** procedures and tables are in `TESTING.md`; populate with real lab numbers (not invented here).

## Operator awareness

- JSON `info` `recordings[].path` is **not** JSON-escaped; avoid double-quotes in file paths or consume via text mode.
- Histogram requires explicit `-DBROADCAST_ENABLE_HISTOGRAM` compile flag (documented in `README.md`).
