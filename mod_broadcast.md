# mod_broadcast — One-to-Many Audio Broadcast for FreeSWITCH

**Module:** `mod_broadcast` (FreeSWITCH 1.10.13-dev, `src/mod/applications/mod_broadcast/`)
**Purpose:** Lock-free, one-to-many live audio broadcast — a single "speaker" leg is fanned out to thousands of "listener" legs in real time.
**Status as of 2026-07-09:** 8 concurrency crashes found and fixed; validated at 1,000 live listeners with a live SIP speaker; benchmarked to ~5,150 deliverable listeners on a 16-core box (two-link).

---

## 1. Overview & Architecture

mod_broadcast implements a **Single-Producer / Multi-Consumer (SPMC)** audio pipeline:

- **One producer thread per broadcast** reads audio from the current speaker (or a file), timestamps each frame, and writes it into a lock-free ring buffer (`ring-size` frames, default 50).
- **Each listener** is a pure **consumer** with its own `consumer_seq` cursor into the ring. On every 20 ms tick a listener copies the frame at its cursor and writes it to its own channel — a **write-only hot path** with **no shared mutation**, so listener cost is **O(1)** and listeners never contend with each other.
- Because listeners only *read* the ring and advance a private sequence number, adding listeners scales linearly until the **network TX path** (not CPU, not locks) becomes the bottleneck.

**Roles:** every member joins as either a `speaker` (at most one active at a time — see §7) or a `listener`. A listener can be **promoted** to speaker, and a speaker can be cleared/replaced, all at runtime.

**Member = session lifetime.** Each member's memory lives in its FreeSWITCH session pool and dies with the session. All cross-thread access to a member is pinned via `switch_core_session_read_lock` / session locate+rwunlock so a member cannot be freed out from under another thread.

**Lock order (must be preserved to stay deadlock-free):**
```
hash > config_rwlock > control > speaker > listener > recording > audio_in_mutex
```

---

## 2. Feature List

- One-to-many live broadcast, arbitrarily many listeners per room, many independent rooms (`max-broadcasts`, default 500).
- Lock-free SPMC ring; O(1) write-only listener hot path.
- Runtime **speaker promotion / replacement / clear** (listener → speaker and back), race-safe.
- **Single-speaker invariant** with optional `speaker-override` (barge) policy.
- **Silence policy** when no speaker is talking: `zero` (digital silence), `cn` (comfort noise), or `moh:<file>` (music on hold).
- **Energy-based talk detection** (speaker only) → `speaker-talking` / `speaker-silent` events.
- **Recording** of the mixed broadcast: on-demand or `auto-record`, with a templated output path.
- **File playback into the broadcast** (`play` / `stop_play`) — producer plays a file to all listeners; takes precedence over the live speaker.
- **DTMF caller-controls** — configurable per-profile digit → action bindings (hangup, request-speaker, energy up/down, …).
- **Per-listener resync / lag handling** — a listener that falls behind `max-lag-frames` snaps forward and fires `listener-resync`.
- **Wedged-listener kick** — a listener whose sequence hasn't advanced for >5 s is hung up with `MEDIA_TIMEOUT` (self-healing under overload).
- **20 lifecycle event types** on the FreeSWITCH event bus (see §6) via a dedicated lock-free event ring + emitter thread.
- **Rich API introspection** — per-room info, per-listener detail, producer histogram, module-wide stats.
- **Channel variables** exported to each leg for CDR/reporting (see §5).
- Multiple audio profiles (`default` 8 kHz, `high-quality` 48 kHz, `lecture` 16 kHz + auto-record), tunable per room.
- Ghost / exclusive / tag / display_name member options.
- ISO C90, compiles clean under `-Werror`.

---

## 3. Dialplan Application

```
<action application="broadcast" data="<name>+<param>=<value>+<param>=<value>..."/>
```

The first token is the **broadcast name** (room). Remaining `+key=value` tokens are member params:

| Param | Values | Meaning |
|---|---|---|
| `role` | `speaker` \| `listener` | Join role (default `listener`) |
| `ghost` | bool | Join without firing join/leave events (silent monitor) |
| `exclusive` | bool | Exclusive speaker claim |
| `tag` | string | Free-form tag stored on the member |
| `display_name` | string | Display name for events/API |

**Examples:**
```xml
<!-- Join room "liveroom" as the speaker -->
<action application="broadcast" data="liveroom+role=speaker"/>

<!-- Join room "liveroom" as a listener -->
<action application="broadcast" data="liveroom+role=listener"/>
```

The room is auto-created on first join and destroyed when empty (unless locked/kept).

---

## 4. API / CLI Command Reference

```
broadcast <name> <subcommand> [args]
```
Callable from `fs_cli`, `bgapi`, ESL, or the XML dialplan (`api`/`bgapi`). Dispatched by `broadcast_api_dispatch` (`broadcast_api.c`).

| Subcommand | Args | Description |
|---|---|---|
| `info` | — | Full room summary: speaker, listener count, producer state, missed ticks, resyncs, uptime |
| `listeners` | — | List all listeners (id, uuid, join time, lag, resync count) |
| `listener` | `<member_id>` | Detailed stats for one listener |
| `producer` | — | Producer thread state + latency histogram |
| `stats` | — | Module-wide stats across all broadcasts |
| `set_speaker` | `<member_id>` | Make an existing member the speaker (via promotion signal) |
| `promote_listener` | `<member_id>` | Promote a listener to speaker (via promotion signal) |
| `clear_speaker` | — | Remove the current speaker (fall back to silence policy) |
| `kick` | `<member_id>` | Hang up one member |
| `kick_all` | — | Hang up all listeners |
| `lock` | — | Lock the room (no new joins) |
| `unlock` | — | Unlock the room |
| `pause` | — | Pause the producer (listeners get silence) |
| `resume` | — | Resume the producer |
| `record` | `[path]` | Start recording the mixed broadcast |
| `norecord` | — | Stop recording |
| `play` | `<file>` | Play a file into the broadcast (overrides live speaker) |
| `stop_play` | — | Stop file playback (return to live speaker) |
| `dtmf` | `<member_id> <digits>` | Inject DTMF to a member |
| `destroy` | `[grace_ms] [announcement_file]` | Force-destroy the room. If `grace_ms` (0-60000) is given, wait that long before hard teardown; during grace, new joiners are refused, and if `announcement_file` (WAV path) is given, the producer plays it to all listeners instead of the live speaker. Fires `broadcast::destroy-grace`. `grace_ms > 60000` is clamped with a warning. |

> **Note:** `set_speaker` and `promote_listener` do **not** write the speaker pointer directly. They call `broadcast_request_promotion`, which signals the target member's *own* session thread to publish itself as speaker — this is the core fix for the speaker-lifetime TOCTOU (see §8, crash #7).

---

## 5. Channel Variables

Set on each member's channel (available in CDRs / `hangup_hook`):

| Variable | Meaning |
|---|---|
| `broadcast_name` | Room name joined |
| `broadcast_uuid` | Room UUID |
| `broadcast_profile` | Profile used |
| `broadcast_member_id` | This member's numeric id |
| `broadcast_role` | `speaker` / `listener` |
| `broadcast_join_time` | Join timestamp |
| `broadcast_left_reason` | Why the member left |
| `broadcast_total_duration_ms` | Total time in the room |
| `broadcast_was_speaker_for_ms` | Time spent as speaker (if promoted) |
| `broadcast_resync_count` | Number of resyncs this member incurred |
| `broadcast_ended_by_self` | `true` if the member left voluntarily (self-hangup or DTMF LEAVE), `false` if kicked by admin, wedge detector, or destroy. Empty if not applicable. |
| `broadcast_kicked_by` | Who caused the removal when not self: `admin` (via `broadcast <name> kick`), `wedged` (wedge detector removed a stalled listener), `destroy` (room destroyed). Empty for normal exit. |
| `broadcast_final_role` | The member's role at exit (`speaker` or `listener`). Useful when a listener was promoted mid-session. |

---

## 6. Events (FreeSWITCH event bus)

22 custom event subclasses, emitted by a dedicated lock-free event ring + emitter thread (`broadcast_events.c`). Subclass prefix `broadcast::`:

`create`, `destroy`, `destroy-grace`, `speaker-set`, `speaker-clear`, `speaker-talking`, `speaker-silent`, `listener-join`, `listener-leave`, `listener-resync`, `listener-kicked`, `recording-start`, `recording-stop`, `recording-failed`, `pause`, `resume`, `lock`, `unlock`, `speaker-request`, `producer-stall`, `heartbeat`, `dtmf-action`.

- `destroy-grace` — fired when `destroy` begins its grace period; headers include `Grace-Ms` and `Announcement-File`.
- `dtmf-action` — fired by the `BCTL_ACTION_EVENT` DTMF binding; carries the triggering digit sequence and the operator-supplied payload from the `data` XML attribute.

`heartbeat` fires every `heartbeat-interval-sec` (0 = off). `verbose-events` gates the high-frequency talking/silent events.

---

## 7. Speaker Semantics (single-speaker + switching)

- **At most one active speaker** per room at any instant. A second member joining as speaker does **not** get live unless `speaker-override` is set (barge) — otherwise the first speaker holds the floor.
- **Switching speakers** is done via the API: `broadcast <room> set_speaker <new_id>` or `promote_listener <id>`, or `clear_speaker` to go to silence. These are race-safe (crash #7 fix).
- **Invariant:** `current_speaker` is only ever published by the member's **own** session thread. Foreign threads (API callers) merely *request* promotion; the member's thread commits it and then enters `broadcast_speaker_main_wait`. This eliminates the window where an API thread published a speaker that had already bailed and freed its session.

To make user **1001** the speaker at runtime: find their member id (`broadcast <room> listeners`), then `broadcast <room> set_speaker <id>`. To hand the floor to another leg, `set_speaker` the new id (the old speaker is demoted automatically).

The **speaker mute toggle** feature is activated via the `mute-toggle` DTMF action. This sets or clears the atomic `MFLAG_MUTED_SPEAKER` flag on the current speaker. During the producer thread's read tick, if this flag is set, the producer bypasses reading from the speaker's audio buffer and instead zeros out the dynamic buffer, sending digital silence to all listeners. Crucially, muting a speaker does not modify `current_speaker` or trigger any demotion or promotion transitions. Mute toggling is entirely distinct from `clear_speaker` (which unsets the current speaker and falls back to comfort noise or music on hold) and from `pause` (which halts the producer entirely).

---

## 8. Crashes Found & Fixed (8 total)

All eight were concurrency / lifetime bugs surfaced by a chaos soak (rapid join/leave/promote/clear/record under 1,000-listener load). Each is a use-after-free or shutdown-order race.

| # | Bug | Root cause | Fix |
|---|---|---|---|
| 1 | **Shutdown runtime race** | Runtime housekeeping ran against broadcasts already being torn down during module shutdown | Ordered shutdown; stop runtime work before destroy |
| 2 | **`b_api_info` UAF** | API `info` read a broadcast without pinning it; concurrent destroy freed it mid-read | Pin (ref/rdlock) around the API read |
| 3 | **`b_api_listener_detail` UAF** | Per-listener detail dereferenced a member that could leave concurrently | Session-pin the member for the read |
| 4 | **Producer-via-finder session pinning** | Producer resolved the speaker session by lookup without a read-lock; session could vanish between find and read | `switch_core_session_read_lock` the speaker for the whole read |
| 5 | **Demotion dangling pointer** | Clearing/replacing the speaker left `current_speaker` pointing at a freed member | Clear-if-current under speaker lock; publish only from owner thread |
| 6 | **Speaker input-read UAF** | Speaker input path read from a codec/session being torn down | Guard + cleanup ordering in `broadcast_speaker_stop_input` |
| 7 | **Speaker-promotion TOCTOU** *(systemic)* | Foreign API thread set `current_speaker = m`, but `m`'s own thread gates `speaker_main_wait` on `current_speaker == m`; `m` could bail and free its session before the API published → producer reads freed member | **Invariant: only the member's own thread publishes itself.** New `broadcast_request_promotion` signal (unlink from listener bridge, set `MFLAG_ROLE_TRANSITION_TO_SPEAKER`, clear `MFLAG_RUNNING`, wait for bridge-exit, roll back on timeout); the member's thread then calls `broadcast_set_speaker` on itself and enters `broadcast_speaker_main_wait`. `broadcast_set_speaker` now refuses a member still in the listener bridge. |
| 8 | **Emitter shutdown race** | `mod_broadcast_shutdown` called `broadcast_destroy_all()` **before** `broadcast_event_emitter_stop()`; the emitter thread then read freed `event_ring` pools → SIGSEGV at `broadcast_events.c:235` | **Reordered:** `broadcast_event_emitter_stop()` runs first (joins the emitter), *then* `broadcast_destroy_all()` frees the pools |

**Validation of the #7/#8 fixes:** a 2,400-attempt promotion storm + functional promote→replace→clear + combined chaos soak produced **0 core dumps**. (Note: because `reload` executes the *loaded* module's shutdown code, the reload that installs the #8 fix still crashes once — expected; two of the historical cores were exactly this.)

---

## 9. Testing Performed

| Test | Scenario | Result |
|---|---|---|
| **Test 1** | ~1,000 SIPp listeners, live speaker | Stable; missed ticks 0, resyncs 0 |
| **Test 2** | Music source (loopback speaker) at 1,000 listeners; clear/replace | Audio clean at 1,000; speaker clear/replace correct |
| **Test 3** | Live SIP speaker (dial 5000) + live SIP listener (5001) under 1,000-listener SIPp load; runtime promotion | Live speaker delivered to all; **promotion validated at 1,000-listener scale** |
| **Chaos soak** | Rapid join/leave/promote/clear/record for hours under load | Surfaced all 8 crashes; after fixes, 0 cores over multi-hour soak |
| **Promotion storm** | 2,400 automated promote/replace/clear cycles | 0 cores |

**How to join in a live test (test box):** dial **5000** to join `liveroom` as speaker, **5001** to join as listener, **5002** = endless music (loopback speaker source). Start a music speaker:
```
fs_cli -x "bgapi originate {origination_uuid=$u}loopback/5002/carriers &broadcast(liveroom+role=speaker)"
```

---

## 10. Benchmark Findings (16-core / 30 GB Hetzner VM)

**The listener ceiling is network-pps-bound, not CPU-bound.** At the ceiling CPU still had headroom, but the box became unresponsive — classic softirq/single-queue saturation.

| Configuration | Deliverable pps | ≈ Listeners (50 pps each) | Limiter |
|---|---|---|---|
| Single link (one virtio TX queue) | ~121,000 pps | ~2,400 | Single virtio_net TX queue |
| **Two links (public eth0 + private enp7s0)** | **~257,000 pps** | **~5,150 (clean ~4,000)** | Two TX queues → 2.1× |

**Key discoveries:**
- **iperf3's "0% sender loss" was misleading** — it counts *socket writes*, not wire transmits. NIC `tx_packets` counters showed FreeSWITCH actually transmitted only ~121k pps on one link; the rest were silently dropped at the TX queue. iperf3 confirmed the **fabric itself is clean** — the choke is host-side TX, not the network.
- **Both VMs are single-queue `virtio_net`** (`ethtool -l` Combined max = 1) — NET_RX softirq pinned to one core was the pps choke. The highest-impact remaining change is **host-side virtio multiqueue** (hypervisor `queues=16` + guest `ethtool -L eth0 combined 16`), which the guest cannot enable alone.
- **`max-listeners` default (2000) is a hard cap** — the benchmark plateaued at exactly 2000 (FS answered listener #2001 then BYE'd it) until raised to 50000; the config change needs an unload+load to take effect.

---

## 11. NIC / OS Tuning Applied (both boxes, persisted)

- **RPS = 0xffff** (fan RX softirq across all 16 cores) + **RFS** (`rps_sock_flow_entries`/`rps_flow_cnt` = 32768) + `txqueuelen = 10000`.
- **sysctl** (`/etc/sysctl.d/99-rtp-tuning.conf`): `rmem/wmem_max = 128 MB`, defaults 32 MB, `netdev_max_backlog = 300000`, `netdev_budget = 600`, `udp_mem` raised, `somaxconn = 4096`.
- Persisted via `/usr/local/sbin/net-rps-tune.sh` + **systemd `net-rps-tune.service`** (enabled/active) — re-applies RPS/RFS/txqueuelen on boot. qdisc `fq_codel`.

---

## 12. Setup Gotchas (hard-won)

1. **`external_priv` profile MUST set `rtp-timer-name=soft`** — without it, listeners on the private path block in `read_frame` during the opening window and get wedge-kicked at 5 s (MEDIA_TIMEOUT). The stock `external` profile already had it.
2. **Autoload quirk** — an autoloaded mod_broadcast misbehaves until the first `reload` (or `unload`+`load`) after a fresh boot; re-arm with unload+load.
3. **`max-listeners` default 2000** caps the room silently — raise for large tests; requires unload+load to re-read.
4. **Hetzner cloud firewall** must whitelist the load-gen's public IP for public-path SIP (whitelisted `5.161.120.1`).
5. **File-play takes precedence over the live speaker** — a speaker is silent while a file plays; `stop_play` before switching to a live speaker.
6. **Loopback speaker legs are somewhat fragile** (occasional media timeout) — re-originate if the speaker drops.
7. **`pkill -9 sipp` orphans listeners** on the FS side because the listener dialplan sets `media_timeout=0` — they linger until wedge-kick; prefer graceful SIPp shutdown.
8. **SIPp ACK/BYE `To:` header must carry the full URI** (`To: <sip:[service]@[remote_ip]:[remote_port]>[peer_tag_param]`); a bare tag → FS can't match the ACK → 200 OK retransmits → call torn down at exactly 32 s (Timer H). Not an FS bug.

---

## 13. Source Files & Function Inventory

| File | Responsibility | Key functions |
|---|---|---|
| `mod_broadcast.c` | Module load/shutdown, app + API registration, member run loop, transition-branch speaker commit | `mod_broadcast_load/shutdown`, `broadcast_app_function`, `broadcast_api_function` |
| `mod_broadcast.h` | Types, flags, public prototypes | — |
| `broadcast_core.c` | Room lifecycle, hash + locking, kick, housekeeping | `broadcast_create`, `broadcast_destroy_ex`, `broadcast_destroy_all`, `broadcast_find(_and_ref)`, `broadcast_release`, `broadcast_hash_{rd,wr}{lock,unlock}`, `broadcast_kick_listener`, `broadcast_kick_all_listeners`, `broadcast_runtime_housekeeping`, `b_hangup_listener_session_by_uuid`, `b_expand_auto_record_template` |
| `broadcast_producer.c` | Producer thread — read speaker/file, timestamp, write ring, silence policy | `b_apply_silence_policy` |
| `broadcast_speaker.c` | Speaker publish/clear, promotion signal, speaker input path | `broadcast_request_promotion`, `broadcast_set_speaker`, `broadcast_clear_speaker(_if_current)`, `broadcast_speaker_start_input`, `broadcast_speaker_stop_input`, `broadcast_speaker_main_wait` |
| `broadcast_listener.c` | Listener consumer loop, DTMF caller-controls | `broadcast_listener_run`, `broadcast_listener_handle_dtmf`, `b_listener_dispatch_control`, `b_listener_match_controls` |
| `broadcast_member.c` | Member alloc/prepare, listener add/del/unlink, codec setup | `broadcast_member_prepare`, `broadcast_member_next_id`, `broadcast_listener_add`, `broadcast_listener_del`, `broadcast_listener_unlink_for_promotion`, `broadcast_member_ensure_speaker_audio`, `broadcast_member_cleanup_codecs` |
| `broadcast_record.c` | Broadcast recording | `broadcast_record_start`, `broadcast_record_stop`, `broadcast_record_stop_all`, `b_guess_rec_type` |
| `broadcast_events.c` | Lock-free event ring + emitter thread | `broadcast_event_emitter_start/stop`, `broadcast_enqueue_event`, `broadcast_fire_critical_event`, `broadcast_event_reserve_all`, `broadcast_event_free_all`, `b_evt_add_common`, `b_find_member_by_id`, `b_fire_evt_msg` |
| `broadcast_api.c` | API dispatch + all introspection sub-handlers | `broadcast_api_dispatch`, `b_api_info`, `b_api_listeners`, `b_api_listener_detail`, `b_api_producer(_histogram)`, `b_api_stats`, `b_api_module_stats`, `b_api_list`, `b_cflags_to_str`, `b_find_member_by_id_str`, `b_find_member_pinned`, `b_fmt_uptime_hms`, `b_fmt_utc_from_us` |
| `broadcast_config.c` | XML config load/reload, profiles, caller-controls | `broadcast_config_load`, `broadcast_config_reload`, `broadcast_config_unload`, `b_parse_profile`, `b_parse_control_groups`, `b_parse_action`, `b_param_bool`, `b_config_find_profile_unlocked`, `b_free_profiles_list` |

---

## 14. Configuration Reference (`broadcast.conf.xml`)

**Module settings:** `default-profile`, `max-broadcasts` (500), `event-ring-size` (1024).

**Per-profile params:**

| Param | Default | Meaning |
|---|---|---|
| `rate` | 8000 | Sample rate (Hz) |
| `interval` | 20 | Frame interval (ms) |
| `channels` | 1 | Channel count |
| `ring-size` | 50 | Producer ring depth (frames) |
| `max-lag-frames` | 10 | Listener lag before snap-forward/resync |
| `speaker-override` | false | Allow a new speaker to barge the current one |
| `silence-window-ms` | 800 | Silence detection window |
| `silence-policy` | zero | `zero` \| `cn` \| `moh:<file>` when no speaker |
| `moh-sound` | — | MOH file for `moh:` policy |
| `max-listeners` | 50000* | Hard listener cap (*raised from 2000 default for benchmarking) |
| `listener-pre-buffer-ms` | 0 | Snap-forward delta |
| `timer-name` | timerfd | Producer timer |
| `energy-detection` | true | Speaker talk detection |
| `comfort-noise-level` | 0 | CN level |
| `agc` | false | Automatic gain control |
| `enter/exit/kicked/locked/speaker-joined-sound` | — | Prompt sounds |
| `verbose-events` | false | Emit high-frequency talking/silent events |
| `join-event-delay-ms` | 0 | Delay before firing join event |
| `heartbeat-interval-sec` | 60 | `broadcast::heartbeat` period (0 = off) |
| `caller-controls` | default | DTMF control group name |
| `auto-record` | false | Auto-start recording on room create |
| `record-template` | `/var/spool/freeswitch/broadcast/${name}-...wav` | Recording path template |

**Bundled profiles:** `default` (8 kHz), `high-quality` (48 kHz), `lecture` (16 kHz + MOH silence policy + auto-record + `lecture-controls`, max-listeners 500).

**Caller-control groups:** `default` (digit `0` = hangup); `lecture-controls` (`0`=hangup, `9`=leave, `1`=request-speaker, `*1`=energy-up, `*2`=energy-down, `*6`=mute-toggle, `*7`=vol-up, `*8`=vol-down, `*9`=vol-reset, `**1`=execute-app, `**2`=lua, `**3`=js, `**4`=transfer, `##`=event).

### DTMF caller-control actions

The following action names are recognized by the XML configuration parser (`b_parse_action` in `broadcast_config.c`):

* `hangup` — Hangs up the member's channel immediately. `data` is unused. Applies to both roles.
* `leave` — Exits the broadcast room cleanly and returns to the dialplan. `data` is unused. Applies to both roles.
* `request-speaker` — Enqueues a `BEVT_SPEAKER_REQUEST` event on the FreeSWITCH event bus. `data` is unused. Applies to listeners.
* `energy-up` — Adjusts the talker energy threshold up by subtracting 50 from the current threshold (clamped to >= 100). `data` is unused. Applies to both roles (speaker read, listener write).
* `energy-down` — Adjusts the talker energy threshold down by adding 50 to the current threshold (clamped to <= 5000). `data` is unused. Applies to both roles (speaker read, listener write).
* `mute-toggle` — Toggles the `MFLAG_MUTED_SPEAKER` flag atomically. `data` is unused. Applies to speakers only.
* `vol-up` — Increases the granular volume gain index (clamped to <= 12), adjusting read or write gain. `data` is unused. Applies to both roles.
* `vol-down` — Decreases the granular volume gain index (clamped to >= -12), adjusting read or write gain. `data` is unused. Applies to both roles.
* `vol-reset` — Resets the volume gain index to 0 and scaling factor to 1.0. `data` is unused. Applies to both roles.
* `execute-app` — Executes an arbitrary FreeSWITCH application. `data` is required (format: `<app_name> <app_args>`). Applies to both roles.
* `lua` — Runs an external Lua script on the session. `data` is required (script path). Applies to both roles. Degrades to a WARNING log and no-ops if `mod_lua` is not loaded.
* `js` — Runs an external JavaScript/V8 script on the session. `data` is required (script path). Applies to both roles. Degrades to a WARNING log and no-ops if `mod_v8` is not loaded.
* `transfer` — Transfers the channel to a different extension. `data` is required (format: `<extension> [<dialplan> [<context>]]`). Applies to both roles.
* `event` — Fires a custom `broadcast::dtmf-action` event. `data` is optional (custom payload string). Applies to both roles.

#### Example `lecture-controls` group

```xml
    <group name="lecture-controls">
      <control action="hangup"           digits="0"/>
      <control action="leave"            digits="9"/>
      <control action="request-speaker"  digits="1"/>
      <control action="energy-up"        digits="*1"/>
      <control action="energy-down"      digits="*2"/>
      <control action="mute-toggle"      digits="*6"/>
      <control action="vol-up"           digits="*7"/>
      <control action="vol-down"         digits="*8"/>
      <control action="vol-reset"        digits="*9"/>
      <control action="execute-app"      digits="**1" data="playback /usr/share/freeswitch/sounds/en/us/callie/misc/8000/beep.wav"/>
      <control action="lua"              digits="**2" data="/usr/src/freeswitch/src/mod/applications/mod_broadcast/test/test.lua"/>
      <control action="js"               digits="**3" data="/usr/src/freeswitch/src/mod/applications/mod_broadcast/test/test.js"/>
      <control action="transfer"         digits="**4" data="5002 XML default"/>
      <control action="event"            digits="##"  data="custom-signal-payload"/>
    </group>
```

---

## 15. Outstanding / Next Steps

- **Valgrind 5-minute run** once installed (`apt install valgrind`).
- **30-minute steady-state soak** with random DTMF injection.
- **End-to-end DTMF verification** against a real SIP endpoint on the test box (script confirmed API accepts injection; not confirmed that pressing `9` on a real handset triggers LEAVE).
- **Host-side virtio multiqueue** (still the highest-impact scaling item).

---

## 16. Continuation Sprint (2026-07-10)

A continuation sprint was executed on 2026-07-10 to close key documentation and implementation gaps, specifically addressing reload safety under load-generation, channel variable completeness for billing/CDR purposes, safe room destruction with a grace period, and a complete DTMF caller-controls framework.

### Features Delivered

* **W1 — Config reload race protection:** Introduced a global `config_rwlock` (rank 0.5 in the lock hierarchy). This read-write lock protects profile lookups and cloning from TOCTOU crashes during rapid XML configuration reload storms. `broadcast_config_reload` takes a write-lock, while `broadcast_config_find_profile` requires a caller-held read-lock.
* **W4 — Channel variables completeness:** Standardized reporting on session teardown. Three new channel variables are now populated on exit:
  * `broadcast_ended_by_self` (indicates voluntary leave/hangup)
  * `broadcast_kicked_by` (logs administrator, wedge-detector, or destruction removal)
  * `broadcast_final_role` (records role at teardown, preserving speaker promotion history)
* **W7 — Destroy with grace:** Extended the destroy logic to prevent abrupt termination. The `broadcast_destroy_ex` API accepts `grace_ms` (0–60000 ms, clamped with a warning) and an optional `announcement_file` WAV path. While the grace timer runs, new participants are blocked by `BFLAG_DESTRUCT`, and existing listeners are played the announcement (or silence) bypassing the active speaker.
* **W-DTMF — Full caller-controls framework:** Expanded the control bindings to support 12 action types (9 new). Updated `broadcast_control_binding_t` to include an `arg` string parsed from the XML `data` attribute. Added speaker-side DTMF polling within `broadcast_speaker_main_wait` so talkers can adjust gains or trigger scripts. Detects `mod_lua` and `mod_v8` at module load-time to print non-fatal warnings if they are missing.
* **New event subclasses:** Added `broadcast::destroy-grace` and `broadcast::dtmf-action` to the event registration pool, bringing the total registered subclasses on the event bus to 22.

### What Was Not Touched
To prevent regressions, the core SPMC lock-free audio path, atomic pointer discipline, lock ordering ranks 1–6 (with rank 0.5 added safely as a prefix), core refcounting (`broadcast_find_and_ref` / `broadcast_release`), and all eight prior concurrency bug fixes remained completely untouched.

### Validation Performed
* **Smoke test:** Originated a speaker leg playing suite-espanola-op-47-leyenda.wav and a listener leg. Verified clean audio delivery and 0 missed ticks.
* **Reload race exercise:** Ran 10 rapid config reloads while a live room was active; config reloaded cleanly, and active channels remained uninterrupted.
* **Grace timing:** Tested `destroy 3000` with simulated announcers. The room delayed teardown for exactly 3 seconds while blocking new joiners.
* **DTMF matrix:** Validated all actions using simulated `uuid_recv_dtmf` inputs. Confirmed variable output and script executions (Lua/JS) correctly logged to `/tmp`.
