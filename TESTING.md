# mod_broadcast — validation log

This file is the **template and checklist** for Phase 3 validation (V1–V4) from the production-hardening brief. Fill in measured numbers on a dedicated lab system with SIPp, controlled network paths, and non-production CDR sinks.

**Environment (record per run)**

- FreeSWITCH version / commit:
- Kernel / CPU model / RAM:
- `mod_broadcast` build flags (e.g. `BROADCAST_ENABLE_HISTOGRAM`):
- Profile used (`rate`, `interval_ms`, `ring-size`, `timer-name`):

---

## V1 — SIPp load matrix

For each scenario capture:

- `broadcast <name> producer` (text or JSON) at steady state and peak.
- `top -H -p $(pidof freeswitch)` snapshot at peak.
- `vmstat 1` excerpt during steady state.
- RSS before / after (`ps` or `/proc`).
- Log scan: `grep -E 'WARNING|ERR|CRIT' /var/log/freeswitch/freeswitch.log` (paths as appropriate).

| Scenario | Target | Pass criteria | Result / notes |
|----------|--------|---------------|----------------|
| 100 listeners × 30 min | ramp 10/s | avg tick <200µs, 0 resync, 0 missed, no crash | |
| 500 listeners × 5 min | ramp 50/s | avg <500µs, same | |
| 1000 listeners × 2 min | ramp 100/s | avg <1ms, same | |
| Speaker handoff stress | 100 listeners, random `set_speaker` q30s ×20 | no crash, no >100ms gap (measure with PCAP/tone) | |
| Listener churn | 200 listeners, 5 joins+5 leaves / sec | stable producer metrics, list growth 0 | |
| Destroy stress | 50×(20 listeners +1 speaker), rapid destroy | all dead <12s, no leak (RSS) | |
| Reload stress | 5 broadcasts, `broadcast reload` ×10 | no crash, active unaffected | |

---

## V2 — Valgrind (short)

```bash
valgrind --leak-check=full --show-leak-kinds=all \
  --suppressions=$FREESWITCH_SRC/build/freeswitch.supp \
  freeswitch -nc -nf
```

Run ~5 minutes with **1 speaker + 10 listeners + recording**. Record whether any **definitely lost** blocks mention `mod_broadcast` symbols. APR pool “still reachable” noise is expected.

**Result:**

---

## V3 — 24-hour soak

Single broadcast, **1 speaker + 100 listeners**, listener SIPp pool cycling ~5 min lifetime, speaker plays looped media.

Pass:

- No crash.
- RSS growth ≤ ~50 MB vs start.
- Stable producer avg / drift.
- Resync rate < 1 / listener / hour (aggregate).
- WAV valid (`soxi` / `ffprobe`).

**Result:**

---

## V4 — Chaos

| Test | Pass criteria | Result |
|------|---------------|--------|
| Kill speaker SIP UA (`killall -9 sipp`) | Listeners hear silence policy; FS stable | |
| `iptables` drop 50% listener IPs | Others unaffected; wedged removed ≤5s | |
| Disk full during record | `recording-stop` with write-error; broadcast survives | |
| `fsctl shutdown elegant` with 100 listeners | clean disconnect <20s, no crash | |

---

## Automated smoke (regression)

The `test/` directory contains SIPp XML and a helper script. Re-run after each change set:

```bash
cd src/mod/applications/mod_broadcast/test
# Edit paths / IPs, then:
./run_sipp_examples.sh
```

Document last pass date / outcome here:

**Last smoke:** _______________
