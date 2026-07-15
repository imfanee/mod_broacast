# Agent Brief — Implement `mod_broadcast` for FreeSWITCH 1.10.12+

You are a senior FreeSWITCH C developer with deep expertise in the `mod_conference` codebase, FreeSWITCH core APIs (APR threads/mutexes/rwlocks/pools, `switch_buffer_t`, `switch_core_session_*`, `switch_core_codec_*`, `switch_core_timer_*`, `switch_event_*`, `switch_core_hash_*`), and lock-free concurrency patterns (SPMC ring buffers, acquire/release memory ordering). You write production-grade C that compiles cleanly with `-Wall -Wextra -Werror -Wno-unused-parameter` against the FreeSWITCH 1.10.12 tree without warnings.

You have full read/write access to a local clone of the FreeSWITCH source tree. Your task is to deliver a **production-ready** `mod_broadcast` module that implements every requirement in the attached `mod broadcast spec.md` specification document.

---

## 1. Authoritative Source of Truth

**Treat the attached `mod broadcast spec.md` as the binding specification.** All architectural decisions, data structures, threading rules, lock ordering, configuration schema, CLI commands, event subclasses, and behavior described there are mandatory. Where this prompt and the spec disagree, the spec wins. Where the spec is silent on an implementation detail, you decide using FreeSWITCH conventions visible in `mod_conference`. Where you make any non-trivial implementation choice not covered by the spec, document it inline with a `/* DESIGN NOTE: ... */` comment.

You may consult `src/mod/applications/mod_conference/` freely as a reference for FreeSWITCH coding conventions, idioms, lifecycle patterns, lock discipline, configuration loading, event firing, and API command dispatch. **You may copy code structure and patterns from `mod_conference`** — that is encouraged. You may **not** simply rename `mod_conference` to `mod_broadcast` — the architecture is fundamentally different (SPMC ring versus per-member mixer). Use `mod_conference` for *how to talk to FreeSWITCH*, not for *what to do*.

---

## 2. Deliverables (Definition of Done)

When you finish, the following must all be true:

### 2.1 Source Tree

A new directory `src/mod/applications/mod_broadcast/` exists, containing:

- `mod_broadcast.c` — module entry, dialplan app, API dispatcher, `SWITCH_MODULE_*` macros.
- `mod_broadcast.h` — shared header with all type declarations, constants, flag bits, and function prototypes.
- `broadcast_core.c` — `broadcast_obj_t` lifecycle: create, destroy, hash management, runtime housekeeping.
- `broadcast_member.c` — `member_obj_t` lifecycle: add/remove, role transitions, codec setup/teardown.
- `broadcast_producer.c` — producer thread main loop, ring write, atomic seq publish, silence policy application.
- `broadcast_listener.c` — listener bridge loop, ring read, lag detection, torn-read defense, DTMF dispatch.
- `broadcast_speaker.c` — speaker input thread, energy/silence detection, audio buffer write.
- `broadcast_record.c` — recording consumer lifecycle, writer thread, multi-target support.
- `broadcast_events.c` — per-broadcast event ring (SPSC), module-wide emitter thread, all event emission helpers.
- `broadcast_api.c` — every CLI/API command listed in spec §13, including the `--json` variant.
- `broadcast_config.c` — `broadcast.conf.xml` parser, profile management, caller-controls binding parser.
- `Makefile.am` — exactly as in spec §28.4 but verified against your tree's autotools version.
- `conf/autoload_configs/broadcast.conf.xml` — exactly as in spec §11.1, written so it loads cleanly on first install.

### 2.2 Build Integration

- The line `applications/mod_broadcast` is added to the project root's `modules.conf` at the appropriate position (alphabetical near `mod_conference`).
- `./bootstrap.sh && ./configure && make` completes without errors or warnings attributable to your code. Warnings from other modules are not your responsibility, but your module must produce zero.
- `make install` places `mod_broadcast.so` in the FreeSWITCH module directory and `broadcast.conf.xml` in `conf/autoload_configs/`.

### 2.3 Loadability and Basic Function

After `make install` and a FreeSWITCH restart (or `load mod_broadcast` via `fs_cli`):

- `module_exists mod_broadcast` → `+OK`.
- `broadcast version` → returns the module version string.
- `broadcast list` → returns "0 broadcasts active" cleanly.
- `broadcast reload` → reloads `broadcast.conf.xml` without crash.

### 2.4 Functional Completeness

Every command, event, configuration parameter, channel variable, profile field, and behavior described in `mod broadcast spec.md` sections 9–17 and §13–§15 must be implemented and functional. No `TODO`, no `FIXME`, no stub returning a placeholder. If you encounter a corner where the spec is ambiguous, choose the conservative interpretation (less behavior change, more defensive coding) and document the choice with a `/* DESIGN NOTE: */` comment.

### 2.5 Code Quality

- All allocations use `switch_core_alloc`, `switch_core_session_alloc`, `switch_core_strdup`, or pool-aware equivalents. No raw `malloc`/`free` in module code.
- All thread synchronization uses APR primitives (`switch_mutex_t`, `switch_thread_rwlock_t`, `switch_thread_t`). No `pthread_*` directly.
- Lock acquisition follows the global lock order in spec §7.2 with no exceptions.
- Memory ordering on `producer_seq` and per-slot `seq` uses `__atomic_store_n(..., __ATOMIC_RELEASE)` and `__atomic_load_n(..., __ATOMIC_ACQUIRE)` as in spec §8.3–8.4.
- All public functions have a brief block comment describing purpose, parameters, return value, and locking requirements (which locks the caller must / must not hold).
- All `static` helper functions have at least a one-line comment.
- Indentation: tabs, matching `mod_conference`'s style. Line length: 120 columns soft, 140 hard.
- Naming: snake_case for functions and variables, `UPPER_SNAKE_CASE` for macros/constants, `broadcast_` prefix on every public function, `BROADCAST_` prefix on every public macro/constant, `MFLAG_` / `BFLAG_` for flag bits as in spec §5.

### 2.6 Header Block

Every `.c` and `.h` file you create must begin with this exact license/author header (FreeSWITCH MPL-style block, customized):

```c
/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2026, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Module: mod_broadcast — One-to-many audio broadcast for very large scale
 *                        (lectures, sermons, town halls, public addresses,
 *                        training sessions, broadcasts).
 *
 * Author: Faisal Hanif <imfanee@gmail.com>
 *
 * <THIS FILE>: <one-line description>
 *
 */
```

The `Author:` line must be exactly `Faisal Hanif <imfanee@gmail.com>` — do not change spelling, do not add other authors. If you copy logic from `mod_conference`, keep the original FreeSWITCH copyright but add Faisal as Author for the new file.

### 2.7 Namespace Discipline

Every symbol your module exposes — every function, macro, type, event subclass name, dialplan application, API command, channel variable, configuration file name — must use the `broadcast` namespace. Specifically:

| Kind | Form |
|---|---|
| Public C functions | `broadcast_*` |
| Static C helpers | `b_*` or descriptive (e.g., `parse_data_string`) |
| Types | `broadcast_obj_t`, `member_obj_t`, `ring_frame_t`, `rec_consumer_t`, `evt_msg_t`, `broadcast_profile_t` |
| Macros | `BROADCAST_*`, `BFLAG_*`, `MFLAG_*` |
| Dialplan app | `broadcast` |
| API command | `broadcast` |
| Event subclass | `broadcast::*` (every one listed in spec §14.2) |
| Channel variables | `broadcast_name`, `broadcast_uuid`, `broadcast_role`, `broadcast_member_id`, `broadcast_join_time`, `broadcast_profile`, `broadcast_left_reason`, `broadcast_resync_count`, `broadcast_total_duration_ms` |
| Config file | `broadcast.conf.xml` |
| Profile names | user-defined, no namespacing rule |

**Do not reuse any `mod_conference` symbol name.** No `conference_*` functions, no `CFLAG_*` (use `BFLAG_*`), no `conference::` event subclasses. Even if you copy `mod_conference` code, rename everything to the broadcast namespace before it lands in your module.

### 2.8 Production Readiness Checklist

Before declaring done, verify each of these by code inspection or by running the corresponding test:

- [ ] Module loads and unloads cleanly 10 times in a row with no leaks (run with `valgrind --leak-check=full` on a small test, or use FreeSWITCH's built-in pool tracking).
- [ ] A broadcast with 1 speaker and 0 listeners can be created and destroyed cleanly.
- [ ] A broadcast with 1 speaker and 1 listener exchanges audio. Use a SIPp test or two `loopback/...` originate calls.
- [ ] A broadcast with 1 speaker and 200 listeners exchanges audio for ≥5 minutes with no resync events, no missed ticks, no crashes.
- [ ] A broadcast with 1 speaker and 1000 listeners exchanges audio for ≥1 minute. Producer tick avg < 100 µs, no missed ticks.
- [ ] Speaker reassignment (`broadcast <name> set_speaker <id>`) works and produces ≤1 tick of silence to listeners.
- [ ] Speaker hangup leaves listeners connected; producer transitions to silence policy without crashing.
- [ ] Recording produces a valid WAV file (verify with `soxi` or `ffprobe`) for a 1-minute broadcast.
- [ ] Recording to a `shout://` URL produces a valid stream (verify with `ffprobe` or VLC).
- [ ] `broadcast destroy` while 100 listeners are active disconnects them all cleanly within 2 seconds.
- [ ] `fsctl shutdown` while a broadcast is active completes without crash.
- [ ] All events listed in spec §14.2 fire on the appropriate triggers (verify with `event plain CUSTOM broadcast::*` in fs_cli).
- [ ] Lock-order assertions (debug build, if you implement them) do not fire under stress.
- [ ] No compiler warnings.
- [ ] No `TODO` or `FIXME` strings in committed code.
- [ ] All files have the header block from §2.6.

---

## 3. Implementation Order (Recommended)

Follow this order to keep each step testable. Do not skip ahead.

### Phase 1 — Skeleton (compiles and loads)

1. Create directory `src/mod/applications/mod_broadcast/`.
2. Write `Makefile.am` based on spec §28.4.
3. Write `mod_broadcast.h` with all type declarations and constants from spec §5.
4. Write `mod_broadcast.c` with `SWITCH_MODULE_LOAD_FUNCTION`, `SWITCH_MODULE_SHUTDOWN_FUNCTION`, `SWITCH_MODULE_RUNTIME_FUNCTION`, `SWITCH_MODULE_DEFINITION` — bodies can be near-empty but must initialize and tear down `broadcast_globals` (hash, rwlock, pool reference). Register one stub dialplan app and one stub API command.
5. Write `conf/autoload_configs/broadcast.conf.xml` per spec §11.1.
6. Write a minimal `broadcast_config.c` that loads the XML and stores the default profile.
7. Add to `modules.conf`.
8. Build, install, load. Confirm `module_exists mod_broadcast` succeeds.

### Phase 2 — Broadcast Object Lifecycle

1. Implement `broadcast_create` / `broadcast_destroy` / `broadcast_find` in `broadcast_core.c`. Use per-broadcast memory pool. Initialize all locks. Register/unregister in the module hash.
2. Implement `broadcast list`, `broadcast <name> info`, `broadcast <name> destroy` API commands. No members yet, but the hash management is exercised.
3. Implement the event emitter thread skeleton in `broadcast_events.c` (start in load, stop in shutdown). Wire `broadcast::create` and `broadcast::destroy` events through it.
4. Test: create and destroy 100 broadcasts via API. Verify no leaks.

### Phase 3 — Producer + Silence

1. Implement `broadcast_producer.c` with the full producer loop from spec §6.2 and §8.3.
2. Producer runs on broadcast creation. With no speaker, it emits silence frames per `silence-policy`.
3. Implement `broadcast <name> producer` API command to view producer stats.
4. Test: create a broadcast. Verify producer ticks accumulate at expected rate (50/sec for 20ms interval). Verify no missed ticks. Verify destroy joins the producer thread cleanly.

### Phase 4 — Members and Listeners

1. Implement `member_obj_t` allocation, codec setup (L16 read/write codec init via `switch_core_codec_init`), and lifecycle in `broadcast_member.c`.
2. Implement the dialplan `broadcast` application in `mod_broadcast.c`. Parse `name@profile+key=val` syntax. For now, accept only `role=listener`.
3. Implement `broadcast_listener.c` with the full listener bridge loop from spec §6.4 and §10.4.
4. Test: originate a loopback call into the `broadcast` application as a listener. Verify it joins, hears silence at correct cadence, hangs up cleanly. Verify `broadcast <name> listeners` shows the join. Verify `broadcast::listener-join` and `broadcast::listener-leave` events fire.

### Phase 5 — Speaker

1. Implement `broadcast_speaker.c` with the speaker input thread per spec §6.3 and §9.3.
2. Implement `broadcast_set_speaker` / `broadcast_clear_speaker` per spec §9.2.
3. Extend the dialplan app to handle `role=speaker`.
4. Test: originate one speaker leg playing a tone, and several listener legs. Verify listeners hear the tone. Verify speaker hangup leaves listeners hearing silence, not killed.

### Phase 6 — All Admin Commands

1. Implement every CLI/API command listed in spec §13.1: `kick`, `kick_all`, `lock`, `unlock`, `pause`, `resume`, `dtmf`, `play`, `stop_play`, `set_speaker`, `clear_speaker`, `record`, `norecord`.
2. Implement the JSON variant (`broadcast --json ...`) producing structured output.
3. Test: exercise each command against a live broadcast. Verify expected state change and event emission.

### Phase 7 — Recording

1. Implement `broadcast_record.c` with `broadcast_record_start` / `broadcast_record_stop` per spec §15.
2. Support `.wav`, `.mp3` (if mod_shout loaded), `shout://...`.
3. Implement auto-record on broadcast creation if profile says so.
4. Test: record a 1-minute broadcast to WAV. Verify file plays back correctly with `soxi`/`ffprobe`. Test recording stop/start mid-broadcast. Test multiple simultaneous recordings.

### Phase 8 — DTMF Caller Controls

1. Implement the `caller-controls` parser in `broadcast_config.c`.
2. Implement DTMF dispatch in the listener loop (and speaker loop, briefly).
3. Wire control actions: `hangup`, `request-speaker`, `energy-up`, `energy-down`.
4. Test: dial in, press the configured digits, verify the corresponding action occurs.

### Phase 9 — Load Testing and Hardening

1. Write SIPp scenarios: `bcast_speaker.xml`, `bcast_listener.xml`.
2. Run 100, 500, 1000, 2000-listener scenarios. Measure join latency, producer tick stats.
3. Profile with `perf top -p $(pidof freeswitch)` and identify any unexpected hotspots.
4. Tune `RING_SIZE`, `MAX_LAG_FRAMES`, event-ring size based on observation if needed.
5. Run a 24-hour soak with 100 listeners. Verify no leaks, no resync growth, no missed ticks.

### Phase 10 — Final Polish

1. Verify §2.6 header block is present in every `.c` and `.h` file with the exact `Author: Faisal Hanif <imfanee@gmail.com>` line.
2. Verify §2.5 code quality standards on every file (`grep -r 'TODO\|FIXME' src/mod/applications/mod_broadcast/` → empty).
3. Verify §2.7 namespace discipline: `grep -r 'conference_\|CFLAG_\|conference::' src/mod/applications/mod_broadcast/` → empty.
4. Build with `-Wall -Wextra -Werror` and confirm zero warnings from the module.
5. Run the full §2.8 production readiness checklist.

---

## 4. Hard Rules

These are non-negotiable. Violating any of them means the deliverable is not done.

**R1 — No mutex on the audio data path.** Producer's frame write to the ring takes zero locks. Listener's frame read from the ring takes zero locks. The only lock the producer takes per tick is the speaker rwlock (read) and the speaker's `audio_in_mutex` (briefly, to read the buffer). The listener takes nothing.

**R2 — Producer is O(1) in listener count.** The producer thread must not iterate the listener list, ever. If you find yourself writing a `for (m = listener_head; m; m = m->next)` inside the producer loop, you are doing it wrong.

**R3 — Lock order strictly per spec §7.2.** No exceptions. If you need to acquire two locks, they must be in the documented order. Add `/* lock order: ... */` comments where you take more than one.

**R4 — All threads joined before pool destruction.** Use `switch_thread_join` on every spawned thread before destroying its pool. The most common module crash is a thread reading from a freed pool.

**R5 — Memory ordering uses GCC built-ins explicitly.** `__atomic_store_n(..., __ATOMIC_RELEASE)` and `__atomic_load_n(..., __ATOMIC_ACQUIRE)` for the seq counters. Do not rely on `volatile`. Do not rely on `switch_atomic_t` for 64-bit values; verify the type's width or use a `uint64_t` + atomic built-ins.

**R6 — No new code calls `switch_event_fire` from the producer or listener hot path.** All event emission goes through the per-broadcast SPSC event ring drained by the module-wide emitter thread. The only exceptions are critical lifecycle events (broadcast destroy) which fire synchronously under the control mutex.

**R7 — No `sleep`, no `usleep`, no `switch_yield` in the producer or listener loop.** Use `switch_core_timer_next` for tick synchronization. The only place `switch_yield` is acceptable is the event emitter idle path (yield 5 ms when no work).

**R8 — Configuration changes do not affect running broadcasts.** `broadcast reload` re-reads XML and applies to *new* broadcasts only. Active broadcasts retain their snapshot of profile values from creation time.

**R9 — Every event subclass listed in spec §14.2 is `switch_event_reserve_subclass`'d on load and `switch_event_free_subclass`'d on shutdown.** No exceptions, no skips.

**R10 — Module symbols never collide with `mod_conference` symbols.** If you copy a function from `mod_conference`, rename it to the `broadcast_` namespace before saving.

---

## 5. Where the Spec Hands Off to You

The spec is comprehensive but does not pre-decide everything. Use your judgment on these, and document your choices inline:

- **Exact CLI argument parsing for `--json`** — the spec says JSON output is supported but doesn't define the schema. Define a clean, stable JSON schema per command and document it in the file header of `broadcast_api.c`.
- **DTMF caller-control action callback signatures** — model on `mod_conference`'s `caller_control_action_t` structure but with `broadcast_` naming.
- **Listener-list iteration order** — the spec says listeners are stored in a linked list. Choose head insertion (matches `mod_conference`). Document.
- **Event emitter wakeup mechanism** — the spec says the emitter idles 5 ms between drains. You may instead use a `switch_thread_cond_t` for cleaner wakeup if you wish; document the choice.
- **Producer tick histogram bucket boundaries** — the spec gives example buckets in §20.4; you may use those or tune.
- **Profile field defaults when XML is missing a value** — apply the values shown in spec §11.1's example XML.

---

## 6. Things That Will Probably Bite You

These are non-obvious traps. Address them proactively.

1. **`switch_buffer_t` is not multi-reader safe.** Only the producer reads from the speaker's `audio_buffer`. Only the speaker input thread writes to it. The mutex protects against the writer's mid-write state being read.

2. **`switch_core_codec_init` for L16 must use the speaker's actual packet rate**, not the broadcast's. If the speaker is Opus@48kHz with 20ms packets, init L16 at 48000/20ms. FreeSWITCH core handles the rate conversion between the negotiated codec and your L16. Look at `conference_member.c` ~ line 700 for the exact pattern.

3. **The dialplan app function must block** for the duration of the member's participation. For listeners, that means calling `broadcast_listener_run(m)` directly (not in a new thread). For speakers, you'll spawn the speaker input thread and block on a wait condition until the channel goes down or speaker is cleared.

4. **`switch_core_session_write_frame` returns the codec it encoded with via `frame->codec`.** Don't reuse a `switch_frame_t` between iterations without resetting fields.

5. **Channel hangup detection during a long-running app** — check `switch_channel_ready(channel)` every loop iteration, *and* check the result of `switch_core_session_write_frame` for non-success. Both can indicate a closed channel.

6. **Hash lookup under read lock is fine; hash insert/remove requires write lock.** Don't downgrade.

7. **Per-broadcast pool destruction must wait for the event emitter to drain that broadcast's event ring.** Otherwise the emitter may dereference freed memory. Either: (a) set a per-broadcast `event_ring_done` flag, have emitter skip if set, then destroy. Or (b) make the emitter notice the broadcast is gone via hash lookup before draining.

8. **`switch_core_timer_init` with `timerfd` may fail** on minimal systems. Fall back to `soft` cleanly with a warning log.

9. **The `switch_event_t` lifecycle** — after `switch_event_fire`, do not touch the event pointer; it's been consumed. Use `switch_event_dup` if you need a copy.

10. **APR rwlock readers/writers are reentrant in nested mode (`SWITCH_THREAD_RWLOCK_NESTED`)** — the same thread can acquire the read lock multiple times. Different from POSIX rwlocks. Be aware of this if you nest.

---

## 7. Communication and Iteration

As you work:

- **Commit logically.** One commit per phase (per §3). Commit messages should reference the spec section (e.g., "Implement producer thread per spec §6.2 and §8.3").
- **Document deviations.** Any place you intentionally diverge from the spec, leave a `/* DESIGN NOTE: <one-line>. Spec §X.Y says Z; chose W because... */` comment at the call site.
- **Surface blockers.** If you discover that the spec is impossible or contradictory in some detail, stop and explain the conflict with a proposed resolution. Do not silently work around.
- **Do not over-engineer.** The spec is v1. Roadmap items in §27 are explicitly out of scope. Do not preemptively implement WebRTC, video, multi-speaker mixing, or cross-host distribution.
- **Do not skip tests.** Each phase of §3 includes a test step. Run it before moving to the next phase.

---

## 8. Final Acceptance Criteria

The work is complete when an independent FreeSWITCH operator could:

1. Clone the FreeSWITCH source.
2. `./bootstrap.sh && ./configure && make && sudo make install` without seeing any warning attributable to `mod_broadcast`.
3. Add `<load module="mod_broadcast"/>` to `modules.conf.xml`.
4. Start FreeSWITCH.
5. Configure a dialplan that uses the `broadcast` application.
6. Run a 200-listener production session with one speaker for an hour.
7. Verify via `broadcast <name> stats` that no resyncs occurred, no missed ticks, no errors logged.
8. Recover the recording WAV file and play it back end-to-end without artifacts.
9. Observe events via `mod_event_socket` matching spec §14.2.

…all without consulting the spec author, with only the spec document and the module source as their references.

When you believe you are done, produce a short final report listing:

- Files created (with line counts).
- Commits made.
- Tests run with pass/fail status.
- Any `/* DESIGN NOTE: */` decisions made.
- Any deferred items with justification (there should be none; v1 is fully scoped).

Begin with Phase 1.
