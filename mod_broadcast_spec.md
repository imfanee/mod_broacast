# `mod_broadcast` — FreeSWITCH One-to-Many Audio Broadcast Module

**Comprehensive Design & Implementation Specification**

---

| Field | Value |
|---|---|
| Module name | `mod_broadcast` |
| Target FreeSWITCH version | 1.10.12+ |
| Primary platform | Linux x86_64 (also valid on ARM64) |
| Module type | Application + API + Dialplan + Endpoint-adjacent |
| Concurrency model | Single-producer multiple-consumer (SPMC) with lock-free read path |
| Status | v1 design specification |
| Document version | 1.0 |

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Background and Motivation](#2-background-and-motivation)
3. [Design Goals and Non-Goals](#3-design-goals-and-non-goals)
4. [Architectural Overview](#4-architectural-overview)
5. [Core Data Structures](#5-core-data-structures)
6. [Threading Model](#6-threading-model)
7. [Synchronization Strategy](#7-synchronization-strategy)
8. [The SPMC Ring Buffer](#8-the-spmc-ring-buffer)
9. [Speaker Subsystem](#9-speaker-subsystem)
10. [Listener Subsystem](#10-listener-subsystem)
11. [Configuration](#11-configuration)
12. [Dialplan Application](#12-dialplan-application)
13. [CLI and API Commands](#13-cli-and-api-commands)
14. [Events](#14-events)
15. [Recording Subsystem](#15-recording-subsystem)
16. [Codec Handling](#16-codec-handling)
17. [Memory Management](#17-memory-management)
18. [Module Lifecycle](#18-module-lifecycle)
19. [Error Handling and Failure Modes](#19-error-handling-and-failure-modes)
20. [Observability](#20-observability)
21. [Performance Characteristics](#21-performance-characteristics)
22. [Security Considerations](#22-security-considerations)
23. [Migration from `mod_conference`](#23-migration-from-mod_conference)
24. [Build and Installation](#24-build-and-installation)
25. [Operational Runbook](#25-operational-runbook)
26. [Test Strategy](#26-test-strategy)
27. [Roadmap](#27-roadmap)
28. [Skeleton Source Layout](#28-skeleton-source-layout)
29. [Glossary](#29-glossary)
30. [References](#30-references)

---

## 1. Executive Summary

`mod_broadcast` is a purpose-built FreeSWITCH module that implements true one-to-many audio fan-out for SIP/RTP endpoints. Unlike `mod_conference`, which is a per-member N-to-N mixer with O(N) per-tick cost and per-member mutex contention, `mod_broadcast` uses a **single-producer multiple-consumer (SPMC)** ring buffer pattern: one speaker produces audio frames, all listeners consume those frames with no shared mutable state on the read path.

### 1.1 Why It Exists

In production deployments of large `mod_conference` rooms with one speaker and many silent listeners (the dominant pattern for lectures, religious services, public addresses, town halls, training sessions, and webinars), the mixer thread saturates at approximately 150 members on a typical 32-core KVM guest. Above that threshold, new participants experience random 3–50 second delays before they begin hearing audio, caused by the interaction of mixer-thread overrun and the `flush_len` catastrophic-flush guard in `conference_loop_output()`. `mod_broadcast` eliminates this class of failure structurally by removing the mixer altogether.

### 1.2 Key Properties

- **O(1) producer work per tick**, regardless of listener count.
- **Lock-free audio data path** on the read side; producer takes no mutex in the hot path.
- **One-tick join latency** (~20 ms plus thread startup), deterministic and bounded.
- **Drop-in dialplan ergonomics** — the same call flow patterns operators already use with `mod_conference` apply, with semantically clearer flag names.
- **Speaker reassignment at runtime** — clean speaker-handoff API for instructor changes, Q&A, or fail-over.
- **Recording, events, and observability** built in from v1.

### 1.3 Target Scale

On a 32-core / 128 GB host:

| Listeners | Producer thread | Total thread count | Behavior |
|---|---|---|---|
| 100 | <0.5% of one core | ~300 | Indistinguishable from idle |
| 500 | <0.5% of one core | ~1500 | Smooth |
| 1000 | <0.5% of one core | ~3000 | Smooth; bounded by RTP send-thread headroom |
| 2000 | <0.5% of one core | ~6000 | Approaches Linux per-process thread soft limits; tune `ulimit -u` and `fs.nr_open` |

The producer's cost is constant; the limit becomes per-listener RTP transmission and the kernel's ability to schedule thousands of session bridge threads, which is FreeSWITCH's existing strong suit.

---

## 2. Background and Motivation

### 2.1 The `mod_conference` Saturation Problem

`conference_thread_run()` is a single thread per conference. Per 20 ms tick, it executes:

```
Phase A: Read inputs
   for each of N members:
       lock conference->member_mutex (shared)
       read from member->audio_buffer
       contribute to main_frame mix
       unlock

Phase B: Personalised fan-out
   for each of N members:
       compute personalised mix (main_frame minus member's own contribution)
       apply per-member gain, AGC, resample
       lock member->audio_out_mutex
       write to member->mux_buffer
       unlock
```

At N=200 on a Hetzner KVM 32-core guest, observed per-tick wall-clock approaches and intermittently exceeds the 20 ms budget under VM steal-time. When the mixer overruns, it enters back-to-back catch-up mode, writing multiple frames into each member's `mux_buffer` per real-time interval. New joiners, whose `conference_loop_output()` initialization (codec init, input-thread creation, timer alignment) takes a non-trivial fraction of a second, see their `mux_buffer` exceed `flush_len` (≈500 ms of audio) on first inspection and trigger the catastrophic `switch_buffer_zero()` path — a hard buffer wipe that produces silence until lock and timer alignment with the mixer happens to drift into a clean window.

This is not a bug in `mod_conference`. It is the cost of a per-member personalised-mix architecture under scheduling pressure on a virtualized host. The mixer model is correct for the use case it was designed for (N-to-N conferencing); it is the wrong primitive for one-to-many broadcast.

### 2.2 The Right Primitive

True one-to-many broadcast is a solved pattern in systems engineering:

- DPDK packet pools use SPMC distribution.
- LMAX Disruptor uses a single producer publishing into a ring read by many consumers.
- The Linux kernel's ftrace ring buffer uses per-CPU producer with multi-reader consumption.
- JACK Audio and similar real-time audio servers use lock-free SPMC for cross-client routing.

The defining property: **the producer publishes monotonically, and consumers track their own read position independently.** There is no contention because no consumer mutates state visible to any other consumer or to the producer (other than a single atomic counter consumed via acquire-load).

`mod_broadcast` implements this pattern, adapted to FreeSWITCH's session lifecycle, codec system, and memory model.

### 2.3 Use Cases

The module is justified by, but not limited to, these production patterns:

- **Lectures and online classes** — one instructor, many students, with raise-hand Q&A capability.
- **Religious services** — single preacher, congregation listening.
- **Town hall meetings** — elected official addresses many constituents.
- **Corporate all-hands** — executive presents to thousands of employees.
- **Training sessions** — trainer presents to a cohort.
- **Public emergency announcements** — official broadcasts to subscribed listeners.
- **Sports commentary distribution** — single commentator's audio fanned to many subscribers.
- **Live event audio distribution over SIP** — replacing legacy paging systems.

All of these share the property that 95–100% of participants are listen-only at any given moment, and the cost of running them as full conferences is dominated by work that delivers no value (per-member personalised mix when all per-member contributions are silence).

---

## 3. Design Goals and Non-Goals

### 3.1 Goals

**G1 — Deterministic join latency.** A new listener must begin receiving audio within one tick (20 ms by default) of joining, plus FreeSWITCH session-establishment overhead. No condition in the module shall introduce a variable wait of more than 50 ms.

**G2 — O(1) producer work per tick.** Producer thread CPU time must not grow with listener count. The thread iterates the speaker, not the listener list.

**G3 — Lock-free audio data path.** Producer writes to the ring with no mutex. Consumers read from the ring with no mutex. Only the listener-list and speaker-pointer updates use locks, and those are off the hot path.

**G4 — Speaker reassignment without audio gap.** Transferring speaker role from member A to member B must not produce more than one tick of silence to listeners and must not require listener disconnection.

**G5 — Graceful degradation under speaker absence.** With no assigned speaker (or speaker temporarily silent), the producer emits configurable silence/comfort-noise/MoH frames at full cadence. Listeners never experience timing drift due to producer silence.

**G6 — Codec agnosticism on both sides.** Speaker and listeners may use any FreeSWITCH-supported codec; mismatches are handled by the existing codec-implementation transcoding via L16 intermediate representation. The module itself never assumes a codec.

**G7 — Recording as first-class.** Every broadcast may be recorded with one API call. Recording is mechanically identical to a listener (additional consumer of the ring) and must not perturb the audio path.

**G8 — Observability.** The module must expose per-broadcast and per-listener metrics sufficient to diagnose any production issue (lag, drops, joins, leaves, speaker transitions, recording state) without requiring source-level debugging.

**G9 — Operational safety.** Module reload, broadcast destruction, host shutdown, and arbitrary listener disconnects must never produce a crash, deadlock, or memory leak. All resource releases must be deterministic.

**G10 — No `mod_conference` dependency.** `mod_broadcast` is a standalone module that does not link against, share state with, or require `mod_conference`.

### 3.2 Non-Goals

**NG1 — Conferencing.** `mod_broadcast` is not a conference. It does not mix multiple inputs. If two members produce simultaneously, only the assigned speaker's audio is broadcast. The other is dropped on the floor (with optional `floor_request` events).

**NG2 — Video.** v1 is audio-only. Video broadcast is a separate problem (RFC 4575, video forwarding, simulcast) addressed in roadmap v3.

**NG3 — WebRTC fan-out.** v1 targets SIP/RTP endpoints. Verto/WebRTC integration is roadmap v2.

**NG4 — Multi-host distribution.** A single `mod_broadcast` instance runs on a single FreeSWITCH host. Inter-host distribution (e.g., multiple FreeSWITCHes serving the same broadcast) is achieved via existing FreeSWITCH patterns (eventbridge, mod_event_socket relay), not built into the module.

**NG5 — Replacement of `mod_conference`.** This is a complementary module. Operators should run `mod_conference` for symmetric small-group calls and `mod_broadcast` for one-to-many sessions. The two coexist on the same host.

**NG6 — Persistent session state across restarts.** A FreeSWITCH restart terminates all broadcasts. Listeners reconnect via the control plane; the module does not attempt recovery.

---

## 4. Architectural Overview

### 4.1 Component Diagram

```
┌─────────────────────────────────────────────────────────────────────┐
│                     FreeSWITCH process                              │
│                                                                     │
│   ┌─────────────────────────────────────────────────────────────┐   │
│   │                    mod_broadcast                            │   │
│   │                                                             │   │
│   │   ┌──────────────────┐                                      │   │
│   │   │ broadcast_hash   │  switch_hash_t of broadcast_obj_t    │   │
│   │   └──────────────────┘                                      │   │
│   │           │                                                 │   │
│   │           ▼                                                 │   │
│   │   ┌──────────────────────────────────────────────────────┐  │   │
│   │   │ broadcast_obj_t "lecture101"                         │  │   │
│   │   │                                                      │  │   │
│   │   │   ┌──────────────────┐                               │  │   │
│   │   │   │ producer_thread  │  ───┐                         │  │   │
│   │   │   └──────────────────┘     │ writes                  │  │   │
│   │   │                            ▼                         │  │   │
│   │   │   ┌──────────────────────────────────────┐           │  │   │
│   │   │   │ ring_buffer[RING_SIZE]               │           │  │   │
│   │   │   │ producer_seq (atomic)                │           │  │   │
│   │   │   └──────────────────────────────────────┘           │  │   │
│   │   │                            │                         │  │   │
│   │   │       reads (lock-free)    │                         │  │   │
│   │   │   ┌────────────────────────┴────────────┐            │  │   │
│   │   │   ▼                ▼                    ▼            │  │   │
│   │   │ ┌─────┐         ┌─────┐              ┌─────┐         │  │   │
│   │   │ │ L1  │         │ L2  │   ... ...    │ Ln  │         │  │   │
│   │   │ └─────┘         └─────┘              └─────┘         │  │   │
│   │   │                                                      │  │   │
│   │   │  + speaker_member  (rwlock-protected pointer)        │  │   │
│   │   │  + listener_list   (rwlock-protected)                │  │   │
│   │   │  + recording_consumers (rwlock-protected)            │  │   │
│   │   │  + event_queue (lock-free queue → emitter thread)    │  │   │
│   │   │  + stats (atomic counters)                           │  │   │
│   │   └──────────────────────────────────────────────────────┘  │   │
│   │                                                             │   │
│   │   ┌──────────────────────────────────────────────────────┐  │   │
│   │   │ event_emitter_thread (per-module, drains broadcasts) │  │   │
│   │   └──────────────────────────────────────────────────────┘  │   │
│   │                                                             │   │
│   └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│   Speaker SIP session ──┐                                           │
│                         ▼                                           │
│                    (broadcast app)                                  │
│                                                                     │
│   Listener SIP sessions × N ──→ (broadcast app, role=listener)      │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 4.2 Data Flow

1. **Speaker joins** via `<action application="broadcast" data="lecture101+role=speaker"/>`.
2. The module creates (or looks up) `broadcast_obj_t "lecture101"`, allocates the speaker's `member_obj_t`, and installs it as `current_speaker` under the speaker rwlock.
3. The producer thread, if not already running, is started for this broadcast.
4. **Listeners join** via `<action application="broadcast" data="lecture101+role=listener"/>`.
5. Each listener's `member_obj_t` is appended to `listener_list` (write lock), and the listener's bridge thread enters its output loop.
6. **Producer tick** (every 20 ms):
   - Acquire speaker rwlock (read).
   - Snapshot `current_speaker`.
   - Release rwlock.
   - If speaker exists and has data in its `audio_buffer`, read one frame.
   - Otherwise generate one frame of silence/comfort-noise/MoH per policy.
   - Write frame to `ring_buffer[producer_seq % RING_SIZE]`.
   - Release-store `producer_seq++`.
7. **Listener bridge thread**, on each timer tick:
   - Load `producer_seq` with acquire semantics.
   - If `consumer_seq < producer_seq`, read frame from `ring_buffer[consumer_seq % RING_SIZE]`, increment local `consumer_seq`.
   - If `consumer_seq == producer_seq`, no new frame; write silence to channel.
   - If `producer_seq - consumer_seq > MAX_LAG_FRAMES`, snap forward and log resync.
   - Call `switch_core_session_write_frame()` with the L16 PCM; codec encoding happens transparently.

### 4.3 Key Architectural Decisions

| Decision | Choice | Rationale |
|---|---|---|
| Audio buffer scope | Single shared ring per broadcast | Producer writes once; eliminates O(N) write fan-out |
| Audio format in ring | L16 PCM at broadcast rate | Allows per-listener encoding; matches FreeSWITCH internal format |
| Ring size | 50 frames (1 second at 20 ms) | Comfortable lag tolerance without unbounded memory |
| Frame size | Configurable; default 160 samples × 2 bytes = 320 B for 8 kHz mono | Matches default codec frame |
| Speaker pointer | Single rwlock-protected pointer | Read-mostly; updated only on speaker changes |
| Listener list | Singly-linked list + rwlock | Iteration is rare (admin queries only); insert/delete bounded |
| Inter-thread events | Lock-free SPSC queue per producer + single emitter thread | Producer never blocks on event subsystem |

---

## 5. Core Data Structures

### 5.1 Type Map

```c
typedef struct broadcast_obj broadcast_obj_t;
typedef struct member_obj    member_obj_t;
typedef struct ring_frame    ring_frame_t;
typedef struct rec_consumer  rec_consumer_t;
typedef struct evt_msg       evt_msg_t;
```

### 5.2 `ring_frame_t`

The fixed-size element of the SPMC ring.

```c
#define BROADCAST_MAX_FRAME_BYTES 1920   /* 48 kHz × 20 ms × 2 ch × 2 B */

struct ring_frame {
    uint64_t  seq;                       /* monotonic, matches producer_seq at write */
    uint32_t  datalen;                   /* actual bytes used in data */
    uint32_t  samples;                   /* sample count per channel */
    uint8_t   data[BROADCAST_MAX_FRAME_BYTES];
    uint8_t   silence;                   /* 1 if generated silence, 0 if real audio */
    uint8_t   _pad[7];                   /* 8-byte alignment */
};
```

Notes:
- Fixed-size frames avoid heap allocation in the hot path.
- `seq` is duplicated inside the frame so consumers can validate they read the frame they expected (defense against torn reads, see [§8.4](#84-memory-ordering-and-consistency)).
- `silence` flag lets consumers optimize the encode path (some codecs can emit DTX frames cheaper than encoding zero PCM).

### 5.3 `broadcast_obj_t`

The central object representing one active broadcast.

```c
struct broadcast_obj {
    /* identity */
    char                       *name;
    char                        uuid[37];
    switch_memory_pool_t       *pool;

    /* configuration (immutable after creation) */
    broadcast_profile_t        *profile;
    uint32_t                    rate;            /* 8000 / 16000 / 48000 */
    uint32_t                    interval_ms;     /* default 20 */
    uint32_t                    samples_per_frame;
    uint32_t                    bytes_per_frame;
    uint8_t                     channels;        /* 1 or 2 */

    /* ring */
    ring_frame_t               *ring;            /* RING_SIZE entries */
    uint32_t                    ring_size;
    switch_atomic_t             producer_seq;    /* 64-bit, atomic */

    /* producer */
    switch_thread_t            *producer_thread;
    switch_core_timer_t         producer_timer;
    volatile int                producer_running;

    /* speaker */
    switch_thread_rwlock_t     *speaker_lock;
    member_obj_t               *current_speaker;

    /* listeners */
    switch_thread_rwlock_t     *listener_lock;
    member_obj_t               *listener_head;
    uint32_t                    listener_count;

    /* recording */
    switch_thread_rwlock_t     *recording_lock;
    rec_consumer_t             *recording_head;

    /* control */
    switch_mutex_t             *control_mutex;   /* serializes admin ops */
    volatile uint32_t           cflags;          /* CFLAG_* bits */
    char                       *silence_policy;  /* "zero" | "cn" | "moh:file.wav" */

    /* events */
    evt_msg_t                  *event_ring;
    switch_atomic_t             event_prod_seq;
    switch_atomic_t             event_cons_seq;
    uint32_t                    event_ring_size;

    /* stats (all atomic) */
    switch_atomic_t             stat_ticks;
    switch_atomic_t             stat_missed_ticks;
    switch_atomic_t             stat_speaker_silent_ticks;
    switch_atomic_t             stat_listener_resyncs;
    switch_atomic_t             stat_listeners_joined;
    switch_atomic_t             stat_listeners_left;
    switch_time_t               created_at;
    switch_time_t               speaker_active_since;

    /* destruction */
    volatile int                destroyed;
};

/* cflags */
#define BFLAG_RUNNING           (1 << 0)
#define BFLAG_DESTRUCT          (1 << 1)
#define BFLAG_LOCKED            (1 << 2)   /* admin lock — no new joiners */
#define BFLAG_RECORDING         (1 << 3)
#define BFLAG_PAUSE             (1 << 4)   /* producer publishes silence even with speaker */
```

### 5.4 `member_obj_t`

Represents one speaker or one listener.

```c
typedef enum {
    BMEMBER_ROLE_SPEAKER = 1,
    BMEMBER_ROLE_LISTENER = 2,
    BMEMBER_ROLE_OBSERVER = 3    /* future use: admin monitor with no audio */
} bmember_role_t;

struct member_obj {
    /* identity */
    uint32_t                    id;
    char                       *uuid;
    bmember_role_t              role;
    switch_memory_pool_t       *pool;

    /* parent */
    broadcast_obj_t            *broadcast;

    /* session */
    switch_core_session_t      *session;
    switch_channel_t           *channel;

    /* codec setup */
    switch_codec_t              read_codec;     /* raw L16 read */
    switch_codec_t              write_codec;    /* raw L16 write */
    switch_codec_implementation_t read_impl;

    /* speaker-only */
    switch_buffer_t            *audio_buffer;   /* speaker's pending PCM */
    switch_mutex_t             *audio_in_mutex; /* serializes input-thread → producer hand-off */
    switch_thread_t            *input_thread;
    volatile int                input_running;

    /* listener-only */
    uint64_t                    consumer_seq;
    switch_core_timer_t         output_timer;
    uint32_t                    resync_count;

    /* flags */
    volatile uint32_t           mflags;

    /* linkage in listener_list */
    member_obj_t               *next;
    member_obj_t               *prev;

    /* timing */
    switch_time_t               joined_at;
    switch_time_t               last_seen_seq_advance;

    /* destruction */
    volatile int                destroyed;
};

/* mflags */
#define MFLAG_RUNNING           (1 << 0)
#define MFLAG_ITHREAD           (1 << 1)
#define MFLAG_KICKED            (1 << 2)
#define MFLAG_GHOST             (1 << 3)
#define MFLAG_RECORDING_TARGET  (1 << 4)
```

### 5.5 `rec_consumer_t`

A recording target: same shape as a listener, but writes to a file/stream instead of a channel.

```c
typedef enum {
    BREC_TYPE_FILE = 1,
    BREC_TYPE_SHOUT = 2,        /* via mod_shout */
    BREC_TYPE_STREAM = 3        /* arbitrary URL via switch_core_file_open */
} brec_type_t;

struct rec_consumer {
    char                       *target;
    brec_type_t                 type;
    switch_file_handle_t        fh;
    uint64_t                    consumer_seq;
    switch_thread_t            *writer_thread;
    volatile int                running;
    switch_memory_pool_t       *pool;
    rec_consumer_t             *next;
};
```

### 5.6 `evt_msg_t`

Lock-free SPSC queue element for deferred event emission.

```c
typedef enum {
    BEVT_LISTENER_JOIN = 1,
    BEVT_LISTENER_LEAVE,
    BEVT_SPEAKER_SET,
    BEVT_SPEAKER_CLEAR,
    BEVT_SPEAKER_TALKING,
    BEVT_SPEAKER_SILENT,
    BEVT_LISTENER_RESYNC,
    BEVT_RECORDING_START,
    BEVT_RECORDING_STOP,
    BEVT_BROADCAST_CREATE,
    BEVT_BROADCAST_DESTROY
} bevt_type_t;

struct evt_msg {
    bevt_type_t                 type;
    uint32_t                    member_id;
    uint64_t                    seq;
    switch_time_t               when;
    char                        data[256];      /* type-specific small payload */
};
```

### 5.7 Module-Global State

```c
static struct {
    switch_memory_pool_t       *pool;
    switch_hash_t              *broadcast_hash;
    switch_thread_rwlock_t     *hash_rwlock;

    switch_thread_t            *event_emitter_thread;
    volatile int                emitter_running;

    /* configuration */
    char                       *default_profile_name;
    switch_xml_t                config_xml;

    /* statistics */
    switch_atomic_t             total_broadcasts_created;
    switch_atomic_t             total_listeners_served;

    volatile int                running;
} broadcast_globals;
```

---

## 6. Threading Model

### 6.1 Thread Inventory

| Thread | Count per process | Owned by | Purpose |
|---|---|---|---|
| Module event emitter | 1 | module-globals | Drains per-broadcast event queues, calls `switch_event_fire` |
| Broadcast producer | 1 per broadcast | `broadcast_obj_t` | 20 ms tick; reads speaker, writes ring |
| Speaker input | 1 per speaker | `member_obj_t` (role=speaker) | Reads RTP, writes to speaker's `audio_buffer` |
| Listener bridge (output) | 1 per listener | `member_obj_t` (role=listener); runs in session thread | Reads ring, writes RTP |
| Recording writer | 1 per recording target | `rec_consumer_t` | Reads ring, writes file/stream |

For a broadcast with 1 speaker + 200 listeners + 1 recording:

- 1 producer thread
- 1 speaker input thread
- 200 listener bridge threads (these are session bridge threads, not extra threads — they are the existing channel's thread of execution)
- 1 recording writer
- Plus the per-session RTP send/receive threads that FreeSWITCH always creates

**Net additional threads for the module**: producer + input + recording + emitter = 4 threads beyond the listener sessions FreeSWITCH would already create. This is dramatically less than `mod_conference`, which adds a per-member input thread (i.e., 200 extra threads for the same listener count).

### 6.2 Producer Thread Discipline

The producer thread is the only mandatory real-time-sensitive thread in the module. Its design rules are absolute:

**PD1 — Bounded work per tick.** Producer does O(1) work per tick. It does not iterate listeners. It does not iterate recordings. It does not emit events directly. It does not perform any blocking I/O.

**PD2 — No allocations in the hot path.** All buffers are pre-allocated at broadcast creation. Frame writes go into the pre-allocated ring slot.

**PD3 — Single timer per producer.** The producer uses `switch_core_timer_init` with `timerfd` (default) and `switch_core_timer_next` to wait. No `switch_yield`, no `usleep`, no `nanosleep`.

**PD4 — No locks in the steady-state path.** Producer takes the speaker rwlock (read) once per tick to snapshot the speaker pointer. The audio data path takes no locks. The ring write is an atomic store of `producer_seq`.

**PD5 — Bounded under speaker absence.** When no speaker is assigned, producer generates a silence frame using a pre-computed silence buffer or a streaming MoH source (handled by an asynchronous loader, not in the producer).

**PD6 — Producer never calls into channel APIs.** No `switch_channel_*`, no `switch_core_session_*` (except via the speaker's pre-staged `audio_buffer`). Channel interaction lives in the speaker input thread.

### 6.3 Speaker Input Thread

The speaker input thread is conceptually similar to `conference_loop_input` in `mod_conference`:

```c
while (MFLAG_RUNNING) {
    status = switch_core_session_read_frame(session, &read_frame, SWITCH_IO_FLAG_NONE, 0);
    if (!SWITCH_READ_ACCEPTABLE(status)) break;
    if (switch_test_flag(read_frame, SFF_CNG)) continue;

    switch_mutex_lock(member->audio_in_mutex);
    switch_buffer_write(member->audio_buffer, read_frame->data, read_frame->datalen);
    switch_mutex_unlock(member->audio_in_mutex);
}
```

The producer reads from `audio_buffer` under the same mutex:

```c
switch_mutex_lock(speaker->audio_in_mutex);
got = switch_buffer_read(speaker->audio_buffer, frame->data, broadcast->bytes_per_frame);
switch_mutex_unlock(speaker->audio_in_mutex);
```

This is the only mutex in the producer's data path. It is uncontended: only the speaker's input thread writes; only the producer reads. The mutex protects against `switch_buffer_t`'s internal state from being read mid-write. Lock duration: <5 µs.

### 6.4 Listener Bridge Thread (Output)

Runs in the session's bridge thread (i.e., the thread the dialplan application executes on). Equivalent to `conference_loop_output` but stripped to essentials:

```c
switch_core_timer_init(&timer, "timerfd", interval_ms, samples_per_frame, pool);
member->consumer_seq = switch_atomic_read(&broadcast->producer_seq);  /* snap to head */

while (MFLAG_RUNNING && switch_channel_ready(channel)) {
    switch_core_timer_next(&timer);

    uint64_t pseq = switch_atomic_read_acquire(&broadcast->producer_seq);

    if (member->consumer_seq < pseq) {
        if (pseq - member->consumer_seq > MAX_LAG_FRAMES) {
            /* fall too far behind: snap to current */
            member->consumer_seq = pseq - 1;
            switch_atomic_inc(&broadcast->stat_listener_resyncs);
            member->resync_count++;
            enqueue_event(BEVT_LISTENER_RESYNC, member);
        }

        ring_frame_t *rf = &broadcast->ring[member->consumer_seq % broadcast->ring_size];
        /* validate seq match (defense against torn reads) */
        if (rf->seq == member->consumer_seq) {
            write_frame.data = rf->data;
            write_frame.datalen = rf->datalen;
            write_frame.samples = rf->samples;
            write_frame.rate = broadcast->rate;
            write_frame.codec = &member->write_codec;
        } else {
            /* very rare: producer wrapped while we read; emit silence this tick */
            switch_generate_sln_silence(silence_buf, ...);
            write_frame.data = silence_buf;
            ...
        }
        member->consumer_seq++;
    } else {
        /* no new frame yet: emit silence (channel needs steady packets) */
        switch_generate_sln_silence(silence_buf, samples_per_frame, channels, 0);
        write_frame.data = silence_buf;
        write_frame.datalen = bytes_per_frame;
        write_frame.samples = samples_per_frame;
        write_frame.rate = broadcast->rate;
        write_frame.codec = &member->write_codec;
    }

    if (switch_core_session_write_frame(session, &write_frame, SWITCH_IO_FLAG_NONE, 0) != SWITCH_STATUS_SUCCESS) {
        break;
    }
}
```

Key properties:
- The bridge thread takes no broadcast-level lock.
- It accesses only its own `consumer_seq` and reads `broadcast->ring` (which is never resized) and `broadcast->producer_seq` (atomic).
- Silence generation is local; no allocation.

### 6.5 Recording Writer Thread

A recording consumer is functionally identical to a listener, except the output is a file:

```c
while (rec->running) {
    switch_core_timer_next(&timer);

    uint64_t pseq = switch_atomic_read_acquire(&broadcast->producer_seq);
    while (rec->consumer_seq < pseq) {
        ring_frame_t *rf = &broadcast->ring[rec->consumer_seq % broadcast->ring_size];
        if (rf->seq == rec->consumer_seq) {
            switch_core_file_write(&rec->fh, rf->data, &len);
        }
        rec->consumer_seq++;
    }
}
```

Recording writers can run slightly behind (file I/O latency) without affecting listeners — that is the entire point of independent `consumer_seq`.

### 6.6 Event Emitter Thread

A single module-wide thread drains all broadcasts' event ring buffers:

```c
while (broadcast_globals.emitter_running) {
    int worked = 0;
    /* iterate broadcasts read-locked */
    switch_thread_rwlock_rdlock(broadcast_globals.hash_rwlock);
    for (each broadcast b in broadcast_hash) {
        while (b->event_cons_seq < b->event_prod_seq) {
            evt_msg_t *m = &b->event_ring[b->event_cons_seq % b->event_ring_size];
            fire_event(b, m);
            switch_atomic_inc(&b->event_cons_seq);
            worked = 1;
        }
    }
    switch_thread_rwlock_unlock(broadcast_globals.hash_rwlock);
    if (!worked) switch_yield(5000);   /* 5 ms */
}
```

This thread is the only place `switch_event_fire` is called for module-emitted events. It removes event-system jitter from the producer and listener hot paths.

---

## 7. Synchronization Strategy

### 7.1 Lock Inventory

| Lock | Type | Holder examples | Hold time |
|---|---|---|---|
| `broadcast_globals.hash_rwlock` | rwlock | hash insert/remove (write); lookups (read) | <10 µs |
| `broadcast->speaker_lock` | rwlock | speaker change (write); producer per-tick read | <5 µs |
| `broadcast->listener_lock` | rwlock | join/leave (write); admin listing (read) | <10 µs typical, bounded by N |
| `broadcast->recording_lock` | rwlock | start/stop recording (write); admin listing (read) | <5 µs |
| `broadcast->control_mutex` | mutex | serializes admin commands | <100 µs |
| `member->audio_in_mutex` (speaker only) | mutex | speaker input thread + producer | <5 µs |

### 7.2 Lock Ordering

Strict global lock order to prevent deadlock. Locks must be acquired in this order when nested:

```
1. broadcast_globals.hash_rwlock
2. broadcast->control_mutex
3. broadcast->speaker_lock
4. broadcast->listener_lock
5. broadcast->recording_lock
6. member->audio_in_mutex
```

A thread holding lock N may acquire any of locks N+1..6. A thread holding lock N must release it before acquiring any lock 1..N-1.

This is enforced by code review and (optionally) by adding `LOCK_ORDER_ASSERT()` macros in debug builds that maintain a per-thread lock-acquisition counter.

### 7.3 Lock-Free Components

The audio data path uses no locks:

- **Ring buffer writes**: only producer writes; producer is single-threaded by design.
- **Ring buffer reads**: each listener reads `ring[consumer_seq % ring_size]`. Listeners do not coordinate; collisions are impossible because each touches only its own `consumer_seq`.
- **`producer_seq` updates**: atomic release-store by producer; atomic acquire-load by consumers.
- **`consumer_seq` updates**: each owned by exactly one thread (the listener's own bridge thread). Visible to admin queries via atomic read; never written by anyone else.

### 7.4 Why RWLocks for Speaker Pointer and Listener List

The speaker pointer is read every 20 ms by the producer and written only on speaker reassignment (rare). RWLock with reader bias is the correct choice: the read path is nearly free (atomic-counter increment), and writer latency is acceptable.

The listener list is read by the producer (no — the producer doesn't iterate the list; that's the point) and by admin tools (rare). It is written on every listener join/leave. RWLock here serves admin-tool concurrency: a `broadcast info` command should not block joins, and joins should not block one another except briefly. Use APR's nested rwlock semantics (`SWITCH_THREAD_RWLOCK_NESTED`) for safety in re-entrant paths.

### 7.5 Memory Ordering Guarantees

On x86_64, all stores have implicit release semantics and all loads have implicit acquire semantics for normally-aligned word-size operations. On ARM64 and weaker architectures, explicit barriers are required.

Use GCC/Clang built-ins for portability:

```c
/* Producer publishes a new frame */
__atomic_store_n(&broadcast->producer_seq.counter,
                 producer_seq + 1,
                 __ATOMIC_RELEASE);

/* Consumer observes producer's progress */
uint64_t pseq = __atomic_load_n(&broadcast->producer_seq.counter,
                                __ATOMIC_ACQUIRE);
```

The release-store on the producer's increment-of-seq pairs with the acquire-load on the consumer side, establishing happens-before ordering: all stores to `ring[seq % size]` (the frame data and its embedded `seq` field) made by the producer before the seq increment are visible to a consumer that reads the new seq.

### 7.6 Validation of Read Consistency

The ring buffer is fixed-size; if the producer overruns the ring by more than its size while a consumer is reading the same slot, the consumer can read a partially-overwritten frame. The defense is:

1. Frame carries its own `seq`.
2. Consumer compares `frame.seq == consumer_seq` after reading.
3. If unequal, the consumer treats the read as missed (silence emit this tick) and advances `consumer_seq` to current producer seq.

This is a torn-read detector. In practice it should fire never or vanishingly rarely (producer overrun requires >RING_SIZE × 20 ms = 1 second of consumer lag, which only happens if the consumer is preempted for over a second).

---

## 8. The SPMC Ring Buffer

### 8.1 Layout

A fixed array of `ring_frame_t`, allocated at broadcast creation, sized to hold `RING_SIZE` frames:

```c
broadcast->ring = switch_core_alloc(pool, sizeof(ring_frame_t) * RING_SIZE);
broadcast->ring_size = RING_SIZE;
```

The default `RING_SIZE` is 50 frames = 1 second at 20 ms interval. This is comfortably more than `MAX_LAG_FRAMES` (10 frames) so the lag-snap path triggers before the torn-read path.

Memory cost: at 8 kHz/mono/20 ms, each frame is ~340 B (header + 320 B data). 50 frames = ~17 KB per broadcast. At 48 kHz/stereo, ~100 KB per broadcast. Negligible.

### 8.2 Indexing

Index by `seq % ring_size`. No need to track separate head/tail pointers — consumers carry their own `consumer_seq`, and the producer carries `producer_seq`. The ring is purely a circular array indexed by a monotonic counter modulo size.

### 8.3 Write Path

```c
/* Producer, in the tick loop, after assembling 'frame' from speaker buffer */
uint64_t seq = __atomic_load_n(&broadcast->producer_seq.counter, __ATOMIC_RELAXED);
ring_frame_t *slot = &broadcast->ring[seq % broadcast->ring_size];

slot->datalen = frame->datalen;
slot->samples = frame->samples;
slot->silence = frame->silence;
memcpy(slot->data, frame->data, frame->datalen);
slot->seq = seq;                          /* write seq LAST inside the slot */

/* Publish — release barrier ensures all of the above are visible before seq counter advances */
__atomic_store_n(&broadcast->producer_seq.counter, seq + 1, __ATOMIC_RELEASE);
```

The order matters: data first, slot's own `seq` field next, global `producer_seq` last with release semantics.

### 8.4 Read Path

```c
/* Listener bridge thread */
uint64_t pseq = __atomic_load_n(&broadcast->producer_seq.counter, __ATOMIC_ACQUIRE);

if (member->consumer_seq < pseq) {
    /* check for lag */
    if (pseq - member->consumer_seq > MAX_LAG_FRAMES) {
        member->consumer_seq = pseq - 1;
        member->resync_count++;
        /* event emit deferred to emitter thread */
        enqueue_event(broadcast, BEVT_LISTENER_RESYNC, member->id);
    }

    ring_frame_t *slot = &broadcast->ring[member->consumer_seq % broadcast->ring_size];

    /* validate */
    if (__atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE) == member->consumer_seq) {
        /* fast path */
        write_pcm = slot->data;
        write_len = slot->datalen;
        member->consumer_seq++;
    } else {
        /* torn read — should be exceedingly rare */
        write_pcm = silence_buf;
        write_len = bytes_per_frame;
        member->consumer_seq = pseq;
    }
} else {
    /* no new frame yet */
    write_pcm = silence_buf;
    write_len = bytes_per_frame;
}
```

### 8.5 Sizing

`RING_SIZE` should satisfy:

- ≥ 2 × `MAX_LAG_FRAMES` so the lag-snap path always fires before torn-read becomes possible.
- ≥ 5 frames (100 ms) so brief consumer stalls don't lose data.
- ≤ a few seconds so memory is bounded.

Default 50 is recommended. Configurable per profile.

### 8.6 Recovery from Producer Stall

If the producer thread itself stalls (e.g., scheduler preemption, debug pause), `producer_seq` stops advancing. Consumers see `consumer_seq == producer_seq` and emit silence. When the producer resumes:

- Producer continues from where it left off; its internal seq is preserved.
- Consumers naturally pick up the next frame.
- The "missed" wall-clock time is real-time silence — listeners experienced a gap.

The producer thread should log a warning if its tick wall-clock exceeds `2 × interval_ms`, which indicates a scheduler problem that operators need to know about. The `stat_missed_ticks` counter tracks this.

---

## 9. Speaker Subsystem

### 9.1 Speaker Lifecycle

```
[uninitialized] ──speaker joins──> [active] ──speaker leaves──> [uninitialized]
                                     │ ▲
                            handoff  │ │  handoff
                                     ▼ │
                                   [transitioning]
```

States:

- **uninitialized**: `broadcast->current_speaker == NULL`. Producer emits silence policy frames.
- **active**: `current_speaker` points to a live member. Producer reads from member's audio_buffer.
- **transitioning**: Brief state during speaker change (single producer tick); old speaker still draining or new speaker not yet feeding.

### 9.2 Speaker Assignment

A member becomes speaker via one of:

1. **Join with `role=speaker`**: the dialplan application sets the role explicitly. The first member with `role=speaker` is installed as `current_speaker` automatically.
2. **API command**: `broadcast <name> set_speaker <member_id>`. Reassigns regardless of current state.
3. **Promotion from listener**: `broadcast <name> promote_listener <member_id>`. Same effect; explicit naming.

Implementation:

```c
switch_status_t broadcast_set_speaker(broadcast_obj_t *b, member_obj_t *new_speaker)
{
    member_obj_t *old;

    switch_thread_rwlock_wrlock(b->speaker_lock);
    old = b->current_speaker;

    if (old == new_speaker) {
        switch_thread_rwlock_unlock(b->speaker_lock);
        return SWITCH_STATUS_SUCCESS;
    }

    if (new_speaker) {
        new_speaker->role = BMEMBER_ROLE_SPEAKER;
        /* start input thread for new speaker if not yet running */
        broadcast_speaker_start_input(new_speaker);
    }

    b->current_speaker = new_speaker;
    b->speaker_active_since = switch_micro_time_now();
    switch_thread_rwlock_unlock(b->speaker_lock);

    if (old) {
        /* demote old speaker: stop its input thread; convert to listener or release */
        broadcast_speaker_stop_input(old);
        old->role = BMEMBER_ROLE_LISTENER;
        /* old speaker continues hearing the broadcast as a listener */
    }

    enqueue_event(b, new_speaker ? BEVT_SPEAKER_SET : BEVT_SPEAKER_CLEAR,
                  new_speaker ? new_speaker->id : 0);

    return SWITCH_STATUS_SUCCESS;
}
```

### 9.3 Speaker Input Thread

Each active speaker has its own input thread. The thread is started on speaker assignment and stopped on demotion.

```c
static void *SWITCH_THREAD_FUNC broadcast_speaker_input_run(switch_thread_t *thread, void *obj)
{
    member_obj_t *m = (member_obj_t *)obj;
    broadcast_obj_t *b = m->broadcast;
    switch_frame_t *read_frame;
    switch_status_t status;

    m->input_running = 1;
    switch_set_flag(m, MFLAG_ITHREAD);

    while (m->input_running && (m->mflags & MFLAG_RUNNING) && switch_channel_ready(m->channel)) {
        status = switch_core_session_read_frame(m->session, &read_frame, SWITCH_IO_FLAG_NONE, 0);
        if (!SWITCH_READ_ACCEPTABLE(status)) break;
        if (switch_test_flag(read_frame, SFF_CNG)) continue;
        if (read_frame->datalen == 0) continue;

        switch_mutex_lock(m->audio_in_mutex);
        switch_buffer_write(m->audio_buffer, read_frame->data, read_frame->datalen);
        switch_mutex_unlock(m->audio_in_mutex);
    }

    switch_clear_flag(m, MFLAG_ITHREAD);
    m->input_running = 0;
    return NULL;
}
```

The thread terminates naturally when:

- Channel hangs up (read returns failure).
- `input_running` is cleared by speaker demotion.
- `MFLAG_RUNNING` is cleared by member destruction.

### 9.4 Multiple Speaker Candidates

Only one speaker is active at a time. If a member joins with `role=speaker` while another speaker is already active:

- **Default behavior**: reject the new join with a log warning and a `BEVT_SPEAKER_REJECTED` event. The new joiner gets a busy tone or is hung up via dialplan.
- **Override mode**: configurable per profile (`<param name="speaker-override" value="true"/>`), where the new joiner displaces the current speaker. Use case: hot-standby instructor.

### 9.5 Speaker Silence Detection

In the speaker input thread, energy is computed every frame (lightweight: sum-of-absolute-values, ~3 µs at 320 samples). When energy stays below threshold for `silence_window_ms` (default 800 ms), emit `BEVT_SPEAKER_SILENT`. When energy returns, emit `BEVT_SPEAKER_TALKING`.

This is purely informational — listeners always receive frames regardless of speaker silence. The events let your control plane drive a mute indicator in the UI.

### 9.6 Speaker Audio Buffer Sizing

`audio_buffer` is a dynamic `switch_buffer_t` sized for ~200 ms of pre-roll:

```c
switch_buffer_create_dynamic(&speaker->audio_buffer,
                             /* block */ 1024,
                             /* start */ broadcast->bytes_per_frame * 10,
                             /* max   */ broadcast->bytes_per_frame * 25);  /* 500 ms cap */
```

If the speaker's network is jittery, frames pile up to 500 ms before drops. This 500 ms is invisible to listeners — the producer drains it at 20 ms cadence. Brief speaker-side jitter is smoothed; sustained speaker-side problems eventually drop frames in the speaker's buffer (not in the listeners' path).

---

## 10. Listener Subsystem

### 10.1 Listener Lifecycle

```
[joining] ──add to list──> [active] ──hangup or kick──> [leaving] ──drained──> [destroyed]
```

### 10.2 Listener Join

```c
switch_status_t broadcast_listener_add(broadcast_obj_t *b, member_obj_t *m)
{
    if (b->cflags & BFLAG_LOCKED) return SWITCH_STATUS_FALSE;

    switch_thread_rwlock_wrlock(b->listener_lock);

    m->id = ++b->next_member_id;
    m->role = BMEMBER_ROLE_LISTENER;
    m->joined_at = switch_micro_time_now();

    /* snap consumer_seq to current producer head */
    m->consumer_seq = __atomic_load_n(&b->producer_seq.counter, __ATOMIC_ACQUIRE);

    /* prepend to list */
    m->next = b->listener_head;
    m->prev = NULL;
    if (b->listener_head) b->listener_head->prev = m;
    b->listener_head = m;

    b->listener_count++;

    switch_thread_rwlock_unlock(b->listener_lock);

    switch_atomic_inc(&b->stat_listeners_joined);
    enqueue_event(b, BEVT_LISTENER_JOIN, m->id);

    switch_set_flag(m, MFLAG_RUNNING);
    return SWITCH_STATUS_SUCCESS;
}
```

The join takes the listener_lock briefly (single-digit microseconds for list insertion). After release, the listener's bridge thread begins iterating immediately on the next tick.

### 10.3 Listener Leave

```c
switch_status_t broadcast_listener_del(broadcast_obj_t *b, member_obj_t *m)
{
    switch_clear_flag(m, MFLAG_RUNNING);

    switch_thread_rwlock_wrlock(b->listener_lock);
    if (m->prev) m->prev->next = m->next; else b->listener_head = m->next;
    if (m->next) m->next->prev = m->prev;
    b->listener_count--;
    switch_thread_rwlock_unlock(b->listener_lock);

    switch_atomic_inc(&b->stat_listeners_left);
    enqueue_event(b, BEVT_LISTENER_LEAVE, m->id);

    return SWITCH_STATUS_SUCCESS;
}
```

Memory release for the member happens when the session's memory pool is destroyed (handled by FreeSWITCH core).

### 10.4 Listener Bridge Loop (Full Skeleton)

```c
void broadcast_listener_run(member_obj_t *m)
{
    broadcast_obj_t *b = m->broadcast;
    switch_core_session_t *session = m->session;
    switch_channel_t *channel = m->channel;
    switch_frame_t write_frame = {0};
    uint8_t silence_buf[BROADCAST_MAX_FRAME_BYTES];
    uint8_t encode_scratch[BROADCAST_MAX_FRAME_BYTES];

    write_frame.data = encode_scratch;
    write_frame.buflen = sizeof(encode_scratch);
    write_frame.rate = b->rate;
    write_frame.codec = &m->write_codec;
    write_frame.samples = b->samples_per_frame;
    write_frame.datalen = b->bytes_per_frame;

    memset(silence_buf, 0, sizeof(silence_buf));

    switch_core_timer_init(&m->output_timer, "timerfd",
                           b->interval_ms, b->samples_per_frame, m->pool);

    while ((m->mflags & MFLAG_RUNNING) && switch_channel_ready(channel) && !b->destroyed) {
        switch_core_timer_next(&m->output_timer);

        uint64_t pseq = __atomic_load_n(&b->producer_seq.counter, __ATOMIC_ACQUIRE);

        if (m->consumer_seq < pseq) {
            if (pseq - m->consumer_seq > MAX_LAG_FRAMES) {
                m->consumer_seq = pseq - 1;
                m->resync_count++;
                switch_atomic_inc(&b->stat_listener_resyncs);
                enqueue_event(b, BEVT_LISTENER_RESYNC, m->id);
            }

            ring_frame_t *slot = &b->ring[m->consumer_seq % b->ring_size];
            uint64_t slot_seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);

            if (slot_seq == m->consumer_seq) {
                memcpy(encode_scratch, slot->data, slot->datalen);
                write_frame.datalen = slot->datalen;
                write_frame.samples = slot->samples;
            } else {
                /* torn read; emit silence and snap forward */
                memcpy(encode_scratch, silence_buf, b->bytes_per_frame);
                write_frame.datalen = b->bytes_per_frame;
                write_frame.samples = b->samples_per_frame;
                m->consumer_seq = pseq - 1;
            }
            m->consumer_seq++;
        } else {
            /* no new frame */
            memcpy(encode_scratch, silence_buf, b->bytes_per_frame);
            write_frame.datalen = b->bytes_per_frame;
            write_frame.samples = b->samples_per_frame;
        }

        if (switch_core_session_write_frame(session, &write_frame, SWITCH_IO_FLAG_NONE, 0)
            != SWITCH_STATUS_SUCCESS) {
            break;
        }
    }

    switch_core_timer_destroy(&m->output_timer);
}
```

### 10.5 Listener-Side DTMF Handling

Listeners may want to:

- Press 1 → raise hand (request to speak).
- Press 0 → leave the broadcast.
- Press * → toggle a feature.

DTMF arrives via the session's normal event channel. The listener's bridge thread polls for DTMF between ticks:

```c
/* Inside the main loop, after switch_core_timer_next */
char dtmf_buf[2];
switch_dtmf_t dtmf;
if (switch_channel_dequeue_dtmf(channel, &dtmf) == SWITCH_STATUS_SUCCESS) {
    broadcast_listener_handle_dtmf(m, dtmf.digit);
}
```

`broadcast_listener_handle_dtmf` is configurable per profile via a callback or a key-binding table similar to `caller-controls` in `mod_conference`.

### 10.6 Listener Output Timer Source

`timerfd` is the recommended timer source on Linux. It uses `CLOCK_MONOTONIC` via `timerfd_create`, giving accurate 20 ms ticks with low jitter. On VMs, `timerfd` is more accurate than the `soft` timer because it relies on kernel `hrtimer` infrastructure.

For systems where `timerfd` is unavailable, fall back to `soft`. The profile parameter `timer-name` controls this:

```xml
<param name="timer-name" value="timerfd"/>
```

---

## 11. Configuration

### 11.1 `conf/autoload_configs/broadcast.conf.xml`

```xml
<configuration name="broadcast.conf" description="One-to-Many Audio Broadcast">

  <!-- Module-wide settings -->
  <settings>
    <param name="default-profile" value="default"/>
    <param name="max-broadcasts"  value="500"/>
    <param name="event-ring-size" value="1024"/>
  </settings>

  <!-- Profiles: per-broadcast configuration templates -->
  <profiles>

    <profile name="default">
      <!-- Audio parameters -->
      <param name="rate"                value="8000"/>
      <param name="interval"            value="20"/>
      <param name="channels"            value="1"/>

      <!-- Ring sizing -->
      <param name="ring-size"           value="50"/>
      <param name="max-lag-frames"      value="10"/>

      <!-- Speaker behavior -->
      <param name="speaker-override"    value="false"/>
      <param name="silence-window-ms"   value="800"/>
      <param name="silence-policy"      value="zero"/>   <!-- zero | cn | moh:filename -->
      <param name="moh-sound"           value=""/>

      <!-- Listener behavior -->
      <param name="max-listeners"       value="2000"/>
      <param name="listener-pre-buffer-ms" value="0"/>   <!-- snap-forward delta -->

      <!-- Timer -->
      <param name="timer-name"          value="timerfd"/>

      <!-- Audio enhancement (all off by default for performance) -->
      <param name="energy-detection"    value="true"/>   <!-- speaker only -->
      <param name="comfort-noise-level" value="0"/>
      <param name="agc"                 value="false"/>

      <!-- Sounds (kept empty by default to minimize overhead) -->
      <param name="enter-sound"         value=""/>
      <param name="exit-sound"          value=""/>
      <param name="kicked-sound"        value=""/>
      <param name="locked-sound"        value=""/>
      <param name="speaker-joined-sound" value=""/>

      <!-- Event behavior -->
      <param name="verbose-events"      value="false"/>
      <param name="join-event-delay-ms" value="0"/>

      <!-- DTMF / caller controls -->
      <param name="caller-controls"     value="default"/>

      <!-- Recording -->
      <param name="auto-record"         value="false"/>
      <param name="record-template"     value="/var/spool/freeswitch/broadcast/${name}-${strftime(%Y%m%d-%H%M%S)}.wav"/>
    </profile>

    <profile name="high-quality">
      <param name="rate"        value="48000"/>
      <param name="interval"    value="20"/>
      <param name="channels"    value="1"/>
      <param name="ring-size"   value="50"/>
      <param name="timer-name"  value="timerfd"/>
      <!-- inherits other defaults -->
    </profile>

    <profile name="lecture">
      <param name="rate"             value="16000"/>
      <param name="silence-policy"   value="moh:hold-music.wav"/>
      <param name="speaker-override" value="false"/>
      <param name="auto-record"      value="true"/>
      <param name="caller-controls"  value="lecture-controls"/>
      <param name="max-listeners"    value="500"/>
    </profile>

  </profiles>

  <!-- DTMF caller control bindings -->
  <caller-controls>

    <group name="default">
      <!-- listeners can do almost nothing by default -->
      <control action="hangup"  digits="0"/>
    </group>

    <group name="lecture-controls">
      <control action="hangup"           digits="0"/>
      <control action="request-speaker"  digits="1"/>
      <control action="energy-up"        digits="*1"/>
      <control action="energy-down"      digits="*2"/>
    </group>

  </caller-controls>

</configuration>
```

### 11.2 Profile Inheritance

Profiles do not inherit from one another in v1 — each is self-contained. v2 may add `<profile name="X" extends="default">` semantics.

### 11.3 Runtime Reload

`reloadxml` followed by `broadcast reload` re-reads `broadcast.conf.xml`. Already-running broadcasts retain their original profile snapshot; new broadcasts use the updated config. This avoids mid-flight reconfiguration that could destabilize active sessions.

---

## 12. Dialplan Application

### 12.1 Application Syntax

```
broadcast <name>[@<profile>][+<key>=<value>[,<key>=<value>...]]
```

### 12.2 Supported Keys

| Key | Values | Default | Description |
|---|---|---|---|
| `role` | `speaker` \| `listener` | `listener` | Member's role at join |
| `ghost` | `true` \| `false` | `false` | Suppress join/leave announcements |
| `pre_buffer_ms` | int | 0 | Snap consumer_seq backward N ms on join (for slight delay-tolerance) |
| `kicked_sound` | path | (profile) | Per-member kicked sound override |
| `display_name` | string | caller_id | Display name in `broadcast info` output |
| `mute_on_join` | `true`/`false` | `true` | (listener) listener is muted on join — always true; reserved for symmetry |
| `tag` | string | `""` | Free-form tag attached to member; visible in events |

### 12.3 Examples

Speaker leg:

```xml
<extension name="bcast_speaker">
  <condition field="destination_number" expression="^bcast_speaker_(.+)$">
    <action application="answer"/>
    <action application="broadcast" data="$1@lecture+role=speaker"/>
  </condition>
</extension>
```

Listener leg:

```xml
<extension name="bcast_listener">
  <condition field="destination_number" expression="^bcast_listen_(.+)$">
    <action application="answer"/>
    <action application="playback" data="ivr/welcome.wav"/>
    <action application="broadcast" data="$1@lecture+role=listener,tag=audience"/>
  </condition>
</extension>
```

### 12.4 Channel Variables Set by the Application

On entry to the broadcast, these variables are set on the channel:

- `broadcast_name` — broadcast identifier
- `broadcast_uuid` — broadcast UUID
- `broadcast_role` — `speaker` or `listener`
- `broadcast_member_id` — assigned member ID
- `broadcast_join_time` — epoch seconds at join
- `broadcast_profile` — profile name used

On exit:

- `broadcast_left_reason` — `hangup` | `kicked` | `destroyed` | `speaker_promoted` | `error`
- `broadcast_resync_count` — number of resyncs experienced (listeners only)
- `broadcast_total_duration_ms` — wall-clock time in broadcast

### 12.5 Auto-Recording

If `auto-record=true` in the profile, the first speaker triggering broadcast creation initiates recording. Recording continues until the broadcast destructs.

### 12.6 Application Behavior on Abnormal Exit

- Channel hangs up: bridge thread breaks on `switch_channel_ready() == FALSE`, runs cleanup, calls `broadcast_listener_del` or `broadcast_speaker_demote`.
- Broadcast destroyed while listener active: bridge thread observes `b->destroyed == 1`, exits gracefully, channel returns to dialplan for follow-up actions.
- Speaker leaves (and no replacement): producer transitions to silence-policy mode automatically; listeners stay connected.

---

## 13. CLI and API Commands

### 13.1 Console Commands

```
broadcast list
broadcast list [<name>]
broadcast <name> info
broadcast <name> listeners
broadcast <name> stats
broadcast <name> producer
broadcast <name> set_speaker <member_id>
broadcast <name> clear_speaker
broadcast <name> kick <member_id>
broadcast <name> kick_all
broadcast <name> lock
broadcast <name> unlock
broadcast <name> pause
broadcast <name> resume
broadcast <name> record <filename>
broadcast <name> norecord
broadcast <name> destroy
broadcast <name> dtmf <member_id> <digits>
broadcast <name> play <filename>
broadcast <name> stop_play
broadcast reload
broadcast version
```

### 13.2 Command Details

#### `broadcast list`

Lists all broadcasts with one-line summary each.

```
broadcast list
─────────────────────────────────────────────────────────────────
NAME              PROFILE    LISTENERS  SPEAKER     UPTIME    REC
lecture101        lecture    187        m_001       00:42:17  YES
sermon_main       default    342        m_001       01:13:05  YES
townhall_district default    51         (none)      00:03:22  NO
─────────────────────────────────────────────────────────────────
3 broadcasts active, 580 total listeners
```

#### `broadcast <name> info`

Detailed broadcast state including configuration, runtime stats, speaker, recording status.

```
broadcast lecture101 info
─────────────────────────────────────────────────────────────────
Broadcast:        lecture101
UUID:             a8f2c1e0-...
Profile:          lecture
Created:          2026-05-11 14:32:18 UTC
Audio:            16000 Hz, mono, 20ms interval
Ring size:        50 frames
Listeners:        187
Speaker:          member 1 (display="Prof. Karim", uuid=xyz)
Speaker active:   00:41:55
Recording:        /var/spool/freeswitch/broadcast/lecture101-20260511-143218.wav
Producer:
  Ticks/sec:      50.0
  Missed ticks:   3 (0.000020%)
  Avg tick µs:    142
Stats:
  Listeners joined: 215
  Listeners left:   28
  Resyncs:          2
─────────────────────────────────────────────────────────────────
```

#### `broadcast <name> listeners`

Tabular listing of all listeners.

```
ID     UUID                                 NAME              JOINED      LAG  RESYNC
m_002  9f81...                              Alice             00:42:00    0    0
m_003  2c44...                              Bob               00:41:55    0    0
m_004  d7e2...                              Carol             00:40:12    1    0
...
```

`LAG` is `producer_seq - consumer_seq`. A lag of 0–2 is normal. Sustained lag >5 indicates an endpoint clock issue.

#### `broadcast <name> producer`

Producer-thread-specific stats. Useful for diagnosing tick stalls.

```
broadcast lecture101 producer
─────────────────────────────────────────────────────────────────
Thread state:    running
Tick interval:   20 ms
Ticks total:     124312
Ticks missed:    3
Avg tick wall:   142 µs (0.71% of budget)
Max tick wall:   8231 µs  (last seen 12 min ago)
Producer seq:    124310
Last frame:      real audio
Silence policy:  zero
─────────────────────────────────────────────────────────────────
```

#### `broadcast <name> set_speaker <member_id>`

Promotes the named listener to speaker. Demotes the current speaker (if any) to listener role.

```
broadcast lecture101 set_speaker m_087
+OK speaker changed: m_001 → m_087
```

#### `broadcast <name> kick <member_id>` and `kick_all`

`kick <id>` removes one listener. The listener's bridge thread observes `MFLAG_KICKED`, plays the kicked sound, and hangs up. `kick_all` does this for every listener; speaker is unaffected (use `clear_speaker` or `destroy` for full teardown).

#### `broadcast <name> lock` and `unlock`

Locked broadcasts reject new joiners. Useful for "doors closed" after the start of an event.

#### `broadcast <name> pause` and `resume`

Paused producers emit silence regardless of speaker state. Used for break announcements without disturbing speaker session.

#### `broadcast <name> record <filename>` and `norecord`

Adds a recording consumer. Multiple recordings to different filenames are allowed.

#### `broadcast <name> destroy`

Tears the broadcast down. All members are disconnected (with kicked-sound if configured), recording is finalized, producer thread is joined, hash entry is removed.

#### `broadcast <name> dtmf <member_id> <digits>`

Injects DTMF into a member's channel. Same semantics as `conference dtmf`.

#### `broadcast <name> play <filename>` and `stop_play`

Replaces the producer's audio source temporarily with a file. While playing, the producer reads from the file instead of the speaker. Useful for inserting announcements or pre-recorded segments. `stop_play` returns to speaker-sourced audio.

### 13.3 API Commands (for `mod_event_socket` clients)

All console commands are also `bgapi` callable:

```
bgapi broadcast list
bgapi broadcast lecture101 info
bgapi broadcast lecture101 set_speaker m_087
```

### 13.4 JSON API

A JSON-output variant for programmatic consumers:

```
broadcast --json list
broadcast --json lecture101 info
```

Returns structured output suitable for control-plane integration.

---

## 14. Events

### 14.1 Event Class

All events use `SWITCH_EVENT_CUSTOM` with subclass prefix `broadcast::`.

### 14.2 Event List

| Subclass | Trigger |
|---|---|
| `broadcast::create` | Broadcast object created |
| `broadcast::destroy` | Broadcast object destroyed |
| `broadcast::speaker-set` | New speaker installed |
| `broadcast::speaker-clear` | Speaker removed, no replacement |
| `broadcast::speaker-talking` | Speaker energy crossed threshold (talking) |
| `broadcast::speaker-silent` | Speaker energy below threshold for window |
| `broadcast::listener-join` | Listener added |
| `broadcast::listener-leave` | Listener removed |
| `broadcast::listener-resync` | Listener snapped to head due to lag |
| `broadcast::listener-kicked` | Listener kicked by admin |
| `broadcast::recording-start` | Recording consumer added |
| `broadcast::recording-stop` | Recording consumer removed |
| `broadcast::pause` | Producer paused |
| `broadcast::resume` | Producer resumed |
| `broadcast::lock` | Broadcast locked (no new joiners) |
| `broadcast::unlock` | Broadcast unlocked |

### 14.3 Common Event Headers

Every event carries these headers:

```
Event-Subclass:    broadcast::<type>
Broadcast-Name:    <name>
Broadcast-UUID:    <uuid>
Broadcast-Profile: <profile-name>
Timestamp:         <unix-microseconds>
```

Per-type additional headers:

`broadcast::listener-join`:

```
Member-ID:         <id>
Member-UUID:       <channel-uuid>
Caller-ID-Name:    <name>
Caller-ID-Number:  <number>
Tag:               <user-supplied tag>
Listener-Count:    <current count>
```

`broadcast::listener-resync`:

```
Member-ID:         <id>
Resync-Total:      <count for this listener>
Lag-Frames:        <how many frames behind before snap>
```

`broadcast::speaker-talking` / `silent`:

```
Member-ID:         <id>
Energy:            <last measured energy>
Silence-Window-Ms: <configured window>
```

### 14.4 Event Emission Strategy

All events are queued on the broadcast's `event_ring` (lock-free SPSC) by the originating thread. The module-wide emitter thread drains all event rings and calls `switch_event_fire`.

This decouples producer/listener hot paths from the event subsystem's mutexes and dispatcher overhead. The producer never blocks on event emission.

### 14.5 Event-Ring Overflow

If a broadcast emits events faster than the emitter thread drains (extremely rare; would require thousands of events/sec), the oldest events are dropped and a counter `stat_events_dropped` is incremented. Critical state-change events (`speaker-set`, `speaker-clear`, `broadcast::destroy`) are never dropped; they bypass the ring and are emitted synchronously from the originating thread under the control_mutex.

---

## 15. Recording Subsystem

### 15.1 Recording Model

A recording is mechanically a listener that writes to a file instead of a channel. It is added to the broadcast as a `rec_consumer_t`, given its own `consumer_seq` initialized to current `producer_seq`, and run on its own writer thread.

### 15.2 Starting Recording

```c
switch_status_t broadcast_record_start(broadcast_obj_t *b, const char *target)
{
    rec_consumer_t *rec;
    switch_memory_pool_t *pool;

    switch_core_new_memory_pool(&pool);
    rec = switch_core_alloc(pool, sizeof(*rec));
    rec->pool = pool;
    rec->target = switch_core_strdup(pool, target);
    rec->type = guess_rec_type(target);

    rec->fh.channels = b->channels;
    rec->fh.native_rate = b->rate;
    if (switch_core_file_open(&rec->fh, target, b->channels, b->rate,
                              SWITCH_FILE_FLAG_WRITE | SWITCH_FILE_DATA_SHORT,
                              pool) != SWITCH_STATUS_SUCCESS) {
        switch_core_destroy_memory_pool(&pool);
        return SWITCH_STATUS_FALSE;
    }

    rec->consumer_seq = __atomic_load_n(&b->producer_seq.counter, __ATOMIC_ACQUIRE);
    rec->running = 1;

    switch_thread_rwlock_wrlock(b->recording_lock);
    rec->next = b->recording_head;
    b->recording_head = rec;
    switch_thread_rwlock_unlock(b->recording_lock);

    switch_threadattr_t *thd_attr;
    switch_threadattr_create(&thd_attr, pool);
    switch_thread_create(&rec->writer_thread, thd_attr,
                         broadcast_record_writer_run, rec, pool);

    enqueue_event(b, BEVT_RECORDING_START, 0);
    return SWITCH_STATUS_SUCCESS;
}
```

### 15.3 Recording Writer Thread

```c
static void *SWITCH_THREAD_FUNC broadcast_record_writer_run(switch_thread_t *thread, void *obj)
{
    rec_consumer_t *rec = (rec_consumer_t *)obj;
    broadcast_obj_t *b = /* parent ref stored in rec */;
    switch_core_timer_t timer;

    switch_core_timer_init(&timer, "timerfd", b->interval_ms,
                           b->samples_per_frame, rec->pool);

    while (rec->running && !b->destroyed) {
        switch_core_timer_next(&timer);

        uint64_t pseq = __atomic_load_n(&b->producer_seq.counter, __ATOMIC_ACQUIRE);
        while (rec->consumer_seq < pseq && rec->running) {
            ring_frame_t *slot = &b->ring[rec->consumer_seq % b->ring_size];
            if (__atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE) == rec->consumer_seq) {
                switch_size_t len = slot->samples;
                switch_core_file_write(&rec->fh, slot->data, &len);
            }
            rec->consumer_seq++;
        }
    }

    switch_core_file_close(&rec->fh);
    switch_core_timer_destroy(&timer);
    return NULL;
}
```

### 15.4 Recording Targets

Supported via `switch_core_file_open`:

- `*.wav` — local WAV file
- `*.mp3` — local MP3 (requires `mod_shout`)
- `shout://...` — streaming to Icecast (via `mod_shout`)
- `http://...` — HTTP PUT (via `mod_http_cache` write mode)

The module does not implement formats itself; it relies on FreeSWITCH's existing file-format modules.

### 15.5 Multiple Concurrent Recordings

Multiple recordings to different targets may run simultaneously (e.g., a local archive + a streaming relay). Each is an independent `rec_consumer_t` with its own thread.

### 15.6 Stopping Recording

```c
switch_status_t broadcast_record_stop(broadcast_obj_t *b, const char *target)
{
    switch_thread_rwlock_wrlock(b->recording_lock);
    /* find and unlink matching rec_consumer */
    /* set rec->running = 0 */
    switch_thread_rwlock_unlock(b->recording_lock);

    /* wait for writer thread to exit */
    switch_thread_join(&status, rec->writer_thread);
    switch_core_destroy_memory_pool(&rec->pool);

    enqueue_event(b, BEVT_RECORDING_STOP, 0);
    return SWITCH_STATUS_SUCCESS;
}
```

### 15.7 Recording File Naming

The profile parameter `record-template` supports the same `${strftime(...)}` and channel-variable expansions as `mod_dptools`. Default:

```
/var/spool/freeswitch/broadcast/${name}-${strftime(%Y%m%d-%H%M%S)}.wav
```

### 15.8 Recording Failure Handling

If `switch_core_file_write` fails (disk full, permission, etc.), the writer thread logs an error, sets `rec->running = 0`, and exits. A `broadcast::recording-stop` event fires with `Reason: write-error`. The broadcast itself is unaffected.

---

## 16. Codec Handling

### 16.1 Internal Format

All audio in the ring is **L16 (signed 16-bit linear PCM)** at the broadcast's configured `rate` and `channels`. This matches FreeSWITCH's internal mixing format. The module never touches raw codec data.

### 16.2 Speaker Side

The speaker session's read codec is set to L16 via `switch_core_codec_init`:

```c
switch_core_codec_init(&speaker->read_codec, "L16", NULL, NULL,
                       broadcast->rate, broadcast->interval_ms,
                       broadcast->channels,
                       SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
                       NULL, speaker->pool);
switch_core_session_set_read_codec(speaker->session, &speaker->read_codec);
```

FreeSWITCH's core then handles the transcoding from the speaker's negotiated SIP codec (PCMU, Opus, G.722, etc.) to L16 automatically on every read.

### 16.3 Listener Side

The listener session's write codec is set to L16:

```c
switch_core_codec_init(&listener->write_codec, "L16", NULL, NULL,
                       broadcast->rate, broadcast->interval_ms,
                       broadcast->channels,
                       SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
                       NULL, listener->pool);
switch_core_session_set_write_codec(listener->session, &listener->write_codec);
```

When the bridge thread calls `switch_core_session_write_frame` with L16 data, FreeSWITCH's core encodes to the listener's negotiated SIP codec before RTP transmission. Each listener pays its own encode cost on its own bridge thread — there is no central encoding bottleneck.

### 16.4 Sample-Rate Mismatch

If the speaker negotiates a codec at a different rate from the broadcast (e.g., speaker is Opus@48 kHz, broadcast is 8 kHz), FreeSWITCH's core resamples on read. If a listener negotiates a codec at a different rate from the broadcast, FreeSWITCH resamples on write. Both happen transparently; the module's ring stays at broadcast rate.

Choose `broadcast->rate` to match the dominant endpoint codec to minimize resampling cost. For typical SIP deployments using PCMU/PCMA, use 8 kHz. For VoLTE/Opus-heavy deployments, use 16 kHz.

### 16.5 Channels

Default is mono. Stereo is supported (`channels=2`) for music/HQ broadcasts but doubles the ring bandwidth. The producer reads stereo frames from the speaker; the ring stores stereo; listeners write stereo. No channel manipulation happens inside the module.

### 16.6 Frame Size

`samples_per_frame = (rate * interval_ms) / 1000`. Default 8000 × 20 / 1000 = 160 samples = 320 bytes mono. Maximum is 48000 × 20 / 1000 × 2 channels × 2 bytes = 3840 bytes, which fits in `BROADCAST_MAX_FRAME_BYTES = 1920` only for mono 48 kHz. Stereo 48 kHz requires raising the constant; v1 caps stereo at 16 kHz to keep frame size ≤ 1280 bytes.

---

## 17. Memory Management

### 17.1 Pool Hierarchy

```
module_pool (created at SWITCH_MODULE_LOAD_FUNCTION)
  ├── broadcast_globals.broadcast_hash
  └── broadcast_globals.event_emitter_thread

broadcast_pool (one per broadcast, created via switch_core_new_memory_pool)
  ├── broadcast->name (strdup'd)
  ├── broadcast->ring (alloc'd)
  ├── broadcast->event_ring (alloc'd)
  └── broadcast->producer_thread

member_pool (uses session's pool for SIP members, separate pool for non-channel members)
  ├── member->audio_buffer (dynamic, separate pool inside)
  ├── member->read_codec, write_codec
  └── member->input_thread (speakers only)

rec_consumer_pool (one per recording)
  ├── rec->fh
  └── rec->writer_thread
```

### 17.2 Allocation Rules

- All allocations go through `switch_core_alloc(pool, size)` or `switch_core_strdup(pool, s)`.
- Use `switch_core_session_alloc(session, size)` for per-member allocations tied to the session lifetime.
- Never call `malloc` or `free` directly.
- Pools are destroyed by `switch_core_destroy_memory_pool(&pool)`, which frees everything allocated against them.

### 17.3 Destruction Order

When destroying a broadcast:

1. Set `b->destroyed = 1` and `b->cflags |= BFLAG_DESTRUCT`.
2. Wait for all listener bridge threads to exit (they observe `b->destroyed` and break).
3. Stop and join all recording writer threads.
4. Stop and join speaker input thread.
5. Stop and join producer thread.
6. Stop and join event emitter consumer of this broadcast's ring.
7. Remove from hash.
8. Destroy `broadcast->pool`.

This order is critical: never destroy the pool while a thread is still reading from it.

### 17.4 Pool Lifetime Pitfalls

The most common mod_conference / mod_broadcast memory bug is destroying a broadcast pool while a listener bridge thread is still running. Defense:

- Use `switch_thread_join` on every spawned thread before pool destruction.
- Member objects allocated from session pools have their lifetime tied to the session — they survive the broadcast in cleanup if the broadcast destructs first.
- Test under heavy concurrent join/leave with the broadcast being destroyed externally; this is the most fertile bug ground.

### 17.5 Reference Counting

The broadcast object uses an implicit refcount via the listener_count + speaker_present + recording_count. Destruction is allowed only when:

- `listener_count == 0` AND
- `current_speaker == NULL` AND
- `recording_head == NULL`

Otherwise, `broadcast destroy` kicks everyone first (`kick_all`, `clear_speaker`, stop recordings) and then proceeds when counts hit zero.

For dynamic broadcasts (created by first joiner, destroyed when empty), set `BFLAG_DYNAMIC` and trigger destruction when the conditions are met.

---

## 18. Module Lifecycle

### 18.1 Load

```c
SWITCH_MODULE_LOAD_FUNCTION(mod_broadcast_load)
{
    switch_application_interface_t *app_interface;
    switch_api_interface_t *api_interface;

    *module_interface = switch_loadable_module_create_module_interface(pool, modname);
    broadcast_globals.pool = pool;
    broadcast_globals.running = 1;

    switch_core_hash_init(&broadcast_globals.broadcast_hash);
    switch_thread_rwlock_create(&broadcast_globals.hash_rwlock, pool);

    if (broadcast_load_config() != SWITCH_STATUS_SUCCESS) {
        return SWITCH_STATUS_GENERR;
    }

    SWITCH_ADD_APP(app_interface, "broadcast", "Join a broadcast",
                   "Join a one-to-many audio broadcast",
                   broadcast_app_function, "<name>[@profile][+key=val,...]",
                   SAF_NONE);

    SWITCH_ADD_API(api_interface, "broadcast", "Broadcast control",
                   broadcast_api_function, "...");

    broadcast_event_emitter_start();

    switch_event_reserve_subclass("broadcast::create");
    switch_event_reserve_subclass("broadcast::destroy");
    /* ... reserve all subclasses ... */

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                      "mod_broadcast loaded\n");
    return SWITCH_STATUS_SUCCESS;
}
```

### 18.2 Shutdown

```c
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_broadcast_shutdown)
{
    broadcast_globals.running = 0;

    /* destroy all broadcasts */
    switch_thread_rwlock_wrlock(broadcast_globals.hash_rwlock);
    for (each broadcast b in hash) {
        broadcast_destroy_locked(b);
    }
    switch_core_hash_destroy(&broadcast_globals.broadcast_hash);
    switch_thread_rwlock_unlock(broadcast_globals.hash_rwlock);

    broadcast_event_emitter_stop();

    switch_event_free_subclass("broadcast::create");
    /* ... free all subclasses ... */

    return SWITCH_STATUS_SUCCESS;
}
```

### 18.3 Runtime

```c
SWITCH_MODULE_RUNTIME_FUNCTION(mod_broadcast_runtime)
{
    /* optional: housekeeping every few seconds */
    while (broadcast_globals.running) {
        switch_yield(5000000);  /* 5 seconds */
        broadcast_runtime_housekeeping();
    }
    return SWITCH_STATUS_TERM;
}
```

### 18.4 Module Definition

```c
SWITCH_MODULE_DEFINITION(mod_broadcast, mod_broadcast_load, mod_broadcast_shutdown, mod_broadcast_runtime);
```

---

## 19. Error Handling and Failure Modes

### 19.1 Producer Thread Crash

If the producer thread dies (assertion failure, signal), `producer_running` is 0. The module's runtime housekeeping detects this and either:

- (Default) marks the broadcast as broken, kicks all listeners with `kicked_sound=error.wav`, destroys broadcast.
- (Configurable) restarts the producer thread, snapping `producer_seq` forward to maintain monotonicity.

Crash recovery is best-effort. Production deployments should monitor `broadcast list` for missing broadcasts and surface to operators.

### 19.2 Speaker Channel Failure

Speaker hangs up, channel error, RTP timeout:

- Speaker input thread exits, sets `input_running = 0`.
- Producer observes `current_speaker->input_running == 0` (via the speaker rwlock); clears speaker, transitions to silence policy.
- `broadcast::speaker-clear` event fires.
- Control plane may install a replacement speaker via API.

### 19.3 Listener Channel Failure

Listener hangs up:

- Bridge thread observes `switch_channel_ready() == FALSE`, breaks loop.
- Calls `broadcast_listener_del`, releases session pool.
- No impact on producer or other listeners.

### 19.4 Recording Failure

File write error (disk full, permission denied):

- Writer thread closes file, exits.
- `rec->running = 0`.
- `broadcast::recording-stop` event with `Reason: write-error`.
- Broadcast continues.

### 19.5 Out-of-Memory

Allocations via `switch_core_alloc` returning NULL:

- Producer thread: log critical, mark broadcast for destruction, kick listeners.
- Listener thread: log error, exit listener cleanly.
- Hash insert failure on broadcast creation: return failure to dialplan, channel goes to next dialplan action.

The module assumes FreeSWITCH's pool allocator does not fail in normal operation; failures indicate a system-wide OOM and the module's response is graceful degradation, not recovery.

### 19.6 Timer Failure

`switch_core_timer_init` returns failure:

- Module: switch to `soft` timer as fallback.
- Producer: log error, refuse to start broadcast.
- Listener: log error, fall back to a fixed-interval `switch_yield`-based loop (degraded accuracy).

### 19.7 Hash Collision / Duplicate Name

Attempting to create a broadcast with a name that already exists:

- Default: the joining channel is added to the existing broadcast.
- With `+exclusive=true` parameter: the join is rejected.

### 19.8 Lock Order Violation (Debug Build)

In debug builds, lock acquisition is tracked per-thread. Acquiring a lock out of order triggers an assertion. Production builds skip this check for performance.

### 19.9 Resync Storm

If many listeners simultaneously resync (e.g., producer stall recovers), the event emitter can be flooded. Mitigation:

- Resync events are coalesced — only one resync event per listener per 1-second window.
- Resync stats remain accurate (atomic counters); only emission is throttled.

---

## 20. Observability

### 20.1 Logging Tiers

| Level | Used for |
|---|---|
| CRIT | Pool failures, producer thread crash, broadcast destroyed unexpectedly |
| ERR | Recording failure, codec init failure, listener add rejected |
| WARN | Producer tick over budget, listener resync, recording delay |
| INFO | Broadcast create/destroy, speaker change, recording start/stop |
| DEBUG | Per-tick details, listener join/leave, DTMF events |
| DEBUG2 | Per-frame writes, ring slot details (extremely verbose) |

Default log level is INFO. WARN and above always emitted.

### 20.2 Per-Broadcast Counters

Already covered in [§5.3](#53-broadcast_obj_t). Accessible via `broadcast <name> stats` and emitted in the `broadcast::heartbeat` event (every 60 s by default).

### 20.3 Per-Listener Counters

| Counter | Source |
|---|---|
| `consumer_seq` | atomic read from member |
| `lag_frames` | `producer_seq - consumer_seq` at moment of query |
| `resync_count` | member field |
| `joined_at_unix_us` | member field |
| `duration_ms` | computed |

Accessible via `broadcast <name> listeners`.

### 20.4 Producer Tick Histogram (Optional Build Flag)

With `--enable-broadcast-histogram` at compile time, the producer maintains a histogram of tick durations:

```
0-50µs       11231  (87.3%)
50-100µs      1200  (9.3%)
100-200µs      301  (2.3%)
200-500µs      105  (0.8%)
500-1000µs      30  (0.2%)
1-5ms           14  (0.1%)
5-20ms           3  (0.02%)
>20ms            0  (0%)
```

The histogram is exposed via `broadcast <name> producer histogram`. Useful for performance regression testing.

### 20.5 ESL / Event Socket Integration

Subscribe to events:

```
event plain CUSTOM broadcast::listener-join broadcast::listener-leave broadcast::speaker-set broadcast::speaker-clear
```

Custom event filtering by broadcast name uses the standard `nixevent` or `filter` ESL commands:

```
filter Broadcast-Name lecture101
```

### 20.6 Prometheus-Style Metrics (Optional Sidecar)

A companion Lua script subscribes to `broadcast::heartbeat` and exposes Prometheus metrics over HTTP:

```
broadcast_listeners{name="lecture101"} 187
broadcast_speaker_present{name="lecture101"} 1
broadcast_producer_tick_us_avg{name="lecture101"} 142
broadcast_listener_resyncs_total{name="lecture101"} 2
```

Not part of v1 module proper, but recommended for production deployments.

---

## 21. Performance Characteristics

### 21.1 CPU Cost Per Tick

On a modern 3 GHz x86 core (Intel Skylake or newer):

| Path | Cost |
|---|---|
| Producer tick (read speaker → write ring) | ~5 µs |
| Speaker input read (channel read + buffer write) | ~15 µs |
| Listener bridge tick (read ring → write channel L16) | ~10 µs per listener |
| Per-listener codec encode (PCMU/PCMA) | ~5 µs |
| Per-listener codec encode (Opus) | ~30–80 µs depending on bitrate |
| Per-listener codec encode (G.722) | ~20 µs |

For 200 PCMU listeners: producer + speaker + 200 × ~15 µs = ~3 ms per tick across all threads. Spread across cores, ~10% of one core.

### 21.2 Memory Cost

Per broadcast: ~25 KB (struct + ring + event_ring + locks).
Per listener (excluding session's own state): ~2 KB.
Per speaker (excluding session): ~10 KB (buffer + codec state).
Per recording: ~5 KB.

A 1000-listener broadcast: ~25 KB + 1000 × 2 KB = ~2 MB. Negligible.

### 21.3 Scaling Limits

**Linear with listener count up to FreeSWITCH's per-process limits:**

- Open files: bounded by `ulimit -n` (set to 100000+).
- Thread count: bounded by `ulimit -u` and kernel `pid_max`.
- RTP ports: bounded by `rtp-start-port`/`rtp-end-port` (default 60 K ports = 30 K simultaneous calls).

**Producer scaling**: independent of listener count. A single broadcast scales to N listeners at constant producer cost.

**Multi-broadcast scaling**: each broadcast has its own producer thread. 100 simultaneous broadcasts = 100 producer threads, each ~5 µs per tick. Total ~25 ms per second of one core. Easily fits.

### 21.4 Join Latency

End-to-end listener join latency (from SIP INVITE accepted to first audio frame received):

```
SIP negotiation                  ~50–200 ms (network round-trip)
Codec init (mod_broadcast)       ~1–5 ms
Timer init                       ~1 ms
Bridge thread start              ~1 ms
First tick wait                  0–20 ms (depends on timer alignment)
RTP packetization + send         ~5 ms
Network to endpoint              ~20–100 ms
TOTAL                            ~80–330 ms
```

The variable component is dominated by SIP/network, not the module. The module's contribution is bounded at ~30 ms regardless of broadcast size.

### 21.5 Speaker-to-Listener Glass-to-Glass Latency

```
Speaker mouth → mic → RTP send       ~20–40 ms (endpoint dependent)
RTP receive → speaker input thread   ~5–20 ms (RX jitter buffer)
Speaker buffer → producer tick       0–20 ms (next tick alignment)
Ring write → listener read           0–20 ms (consumer tick alignment)
Listener write → RTP send            ~5 ms
Endpoint RX → speaker output         ~20–40 ms
TOTAL                                ~70–145 ms
```

Comparable to `mod_conference` for the same path. The module does not add latency over a conference; it adds determinism.

### 21.6 Comparison Table

| Metric | mod_conference (200 listeners) | mod_broadcast (200 listeners) |
|---|---|---|
| Producer CPU per tick | ~10–20 ms (mixer) | ~5 µs (broadcast) |
| Threads added by module | ~400 (input × N + output × N) | ~3 |
| Listener join latency | 3–50 s (under saturation) | 80–330 ms |
| Memory per listener | ~50 KB | ~2 KB |
| Lock contention | High (per-member mutexes) | None on hot path |
| Maximum reliable listeners (1 conf/broadcast) | ~150 | ~1500 |

### 21.7 Benchmarking Methodology

To validate these numbers on a target host:

```
# Launch SIPp scenarios with increasing N
sipp -sf listener_scenario.xml -m 100 -r 5 -l 100 -s broadcast_listen_test 127.0.0.1
sipp -sf listener_scenario.xml -m 500 -r 5 -l 500 -s broadcast_listen_test 127.0.0.1
sipp -sf listener_scenario.xml -m 1000 -r 5 -l 1000 -s broadcast_listen_test 127.0.0.1

# Monitor producer thread tick time
fs_cli -x "broadcast lecture_test producer histogram"

# Monitor system
top -H -p $(pidof freeswitch)
vmstat 1
```

Target: producer tick avg < 100 µs at N=1000.

---

## 22. Security Considerations

### 22.1 Access Control

- Joining a broadcast as **speaker** must be controlled by dialplan (the dialplan decides who is granted `role=speaker`). Typically auth occurs via SIP credentials, ACL, or a pre-call IVR PIN check.
- Joining as **listener** is more permissive but should still be authenticated. Public broadcasts are operator-policy.
- Admin commands (`set_speaker`, `kick`, `destroy`) require ESL credentials or fs_cli console access.

### 22.2 DTMF-Based Authentication

For instructor-promotion via DTMF, the dialplan should verify a PIN before invoking `broadcast <name> set_speaker`. The module itself does not implement PIN logic; it accepts speaker assignment from authorized callers.

### 22.3 Recording Privacy

- All broadcasts where recording is enabled must inform participants (legal compliance, varies by jurisdiction).
- Profile parameter `auto-record-announce` (default `true`) plays a "this session is being recorded" prompt on join.
- Recording targets must be within configured `record-base-path` to prevent path traversal.

### 22.4 Resource Exhaustion

- `max-broadcasts` (module setting) caps total simultaneous broadcasts.
- `max-listeners` (profile setting) caps listeners per broadcast.
- Listener join rate limiting is NOT in v1; deploy with a Kamailio/OpenSIPS in front for rate control.

### 22.5 Memory Safety

The module uses only FreeSWITCH-blessed APIs (`switch_core_alloc`, `switch_core_strdup`, etc.). All allocations are bounded. No user-controlled buffer sizes; frame size is fixed at compile time and configuration time.

DTMF inputs are validated. Channel variables read from external sources are bounded-length copied.

### 22.6 Audit Logging

All admin actions (speaker change, kick, destroy, recording start/stop) emit events that should be archived for audit. Production deployments should configure `mod_event_socket` to relay these events to a SIEM.

---

## 23. Migration from `mod_conference`

### 23.1 Identifying Candidates

A `mod_conference` deployment is a candidate for migration to `mod_broadcast` if:

- 90%+ of participants are listen-only (have `mute` flag).
- Conference size regularly exceeds ~100 participants.
- Q&A (listener-becomes-speaker) is either absent, rare, or can be handled by explicit promotion.
- Latency budget is at SIP/RTP-typical (≤300 ms acceptable; no need for ≤50 ms).

### 23.2 Dialplan Translation

Conference dialplan:

```xml
<action application="conference" data="myconf+flags{mute|ghost}"/>
```

becomes broadcast dialplan:

```xml
<action application="broadcast" data="myconf+role=listener,tag=audience"/>
```

Conference moderator:

```xml
<action application="conference" data="myconf+flags{moderator|endconf}"/>
```

becomes broadcast speaker:

```xml
<action application="broadcast" data="myconf+role=speaker"/>
```

### 23.3 Feature Mapping

| `mod_conference` | `mod_broadcast` |
|---|---|
| Conference profile | Broadcast profile (different fields) |
| `+flags{mute}` | `role=listener` (mute is implicit) |
| `+flags{moderator}` | `role=speaker` |
| `+flags{ghost}` | `ghost=true` |
| `+flags{endconf}` | Speaker leaving triggers destruction if dynamic |
| `conference <name> mute <id>` | N/A — listeners are always muted |
| `conference <name> kick <id>` | `broadcast <name> kick <id>` |
| `conference <name> dtmf <id> <digits>` | `broadcast <name> dtmf <id> <digits>` |
| `conference <name> recording start` | `broadcast <name> record <filename>` |
| `conference <name> list` | `broadcast <name> listeners` |
| `conference list` | `broadcast list` |
| `conference <name> floor <id>` | `broadcast <name> set_speaker <id>` |

### 23.4 Event Migration

`conference::maintenance` becomes `broadcast::*` subclasses. Replace event subscriptions:

```
event plain CUSTOM conference::maintenance
```

with:

```
event plain CUSTOM broadcast::listener-join broadcast::listener-leave broadcast::speaker-set broadcast::speaker-clear
```

### 23.5 Hybrid Deployment

The two modules coexist. A common deployment pattern:

- Small interactive meetings (≤30 participants, full Q&A) → `mod_conference`.
- Large one-to-many events (>50 participants, optional Q&A via explicit promote) → `mod_broadcast`.

The control plane decides which to invoke based on the event type and expected size.

### 23.6 Migration Validation

Before cutting traffic over:

1. Run a parallel test with sample listeners on `mod_broadcast`.
2. Verify audio quality from listener perspective.
3. Verify recording integrity.
4. Measure join latency distribution under load (should be <500 ms p99).
5. Stress test with 2× expected listener count.
6. Validate failover (speaker disconnects → silence policy → speaker reconnects).

---

## 24. Build and Installation

### 24.1 Source Layout

Standard FreeSWITCH module structure:

```
src/mod/applications/mod_broadcast/
├── Makefile.am
├── mod_broadcast.c            (main module entry, dialplan app, API)
├── mod_broadcast.h            (shared header)
├── broadcast_core.c           (broadcast_obj lifecycle, hash management)
├── broadcast_member.c         (member add/remove, role transitions)
├── broadcast_producer.c       (producer thread, ring write)
├── broadcast_listener.c       (listener bridge loop, ring read)
├── broadcast_speaker.c        (speaker input thread)
├── broadcast_record.c         (recording subsystem)
├── broadcast_events.c         (event ring, emitter thread)
├── broadcast_api.c            (CLI/API command implementations)
├── broadcast_config.c         (XML config parsing)
└── conf/
    └── autoload_configs/
        └── broadcast.conf.xml
```

### 24.2 Build Integration

Add to `modules.conf` at the top of the FreeSWITCH source tree:

```
applications/mod_broadcast
```

Then rebuild:

```bash
./bootstrap.sh
./configure
make
sudo make install
```

### 24.3 Module Loading

Add to `conf/autoload_configs/modules.conf.xml`:

```xml
<load module="mod_broadcast"/>
```

Reload:

```bash
fs_cli -x "load mod_broadcast"
fs_cli -x "module_exists mod_broadcast"
```

### 24.4 Compile-Time Options

Optional `configure` flags:

- `--enable-broadcast-histogram` — compile in producer tick histogram.
- `--enable-broadcast-debug-locks` — compile in lock-order assertions.
- `--with-broadcast-max-frame-bytes=N` — override default max frame size.

### 24.5 Runtime Verification

```
fs_cli> module_exists mod_broadcast
+OK
fs_cli> broadcast version
mod_broadcast v1.0 (built 2026-05-11)
fs_cli> broadcast list
0 broadcasts active
```

### 24.6 Dependencies

- FreeSWITCH 1.10.12 or later headers.
- APR (already part of FreeSWITCH).
- libswitch (the core).
- Optional: `mod_shout` for streaming recordings.
- Optional: `mod_av` for video recording (roadmap v3).

No external library dependencies beyond what FreeSWITCH already requires.

---

## 25. Operational Runbook

### 25.1 Pre-Production Checklist

- [ ] `ulimit -n` ≥ 100000
- [ ] `ulimit -u` ≥ 65535
- [ ] `net.ipv4.udp_mem` tuned for expected RTP throughput
- [ ] FreeSWITCH `rtp-start-port`/`rtp-end-port` sized for max concurrent calls
- [ ] `timerfd` available (kernel ≥ 2.6.25)
- [ ] No `mod_conference` on the same host competing for the same call legs (or clear delineation in dialplan)
- [ ] Disk space and rotation for recordings
- [ ] Monitoring subscribed to `broadcast::*` events

### 25.2 Production Configuration Recommendations

```xml
<settings>
    <param name="default-profile"  value="default"/>
    <param name="max-broadcasts"   value="100"/>
    <param name="event-ring-size"  value="2048"/>
</settings>

<profile name="default">
    <param name="rate"            value="8000"/>
    <param name="interval"        value="20"/>
    <param name="ring-size"       value="50"/>
    <param name="max-lag-frames"  value="10"/>
    <param name="timer-name"      value="timerfd"/>
    <param name="silence-policy"  value="zero"/>
    <param name="auto-record"     value="true"/>
    <param name="max-listeners"   value="2000"/>
</profile>
```

### 25.3 Monitoring Dashboards

Recommended dashboards:

1. **Per-broadcast**:
   - Listener count over time
   - Producer tick average / p99
   - Resync events per minute
   - Recording size / write rate

2. **System-wide**:
   - Total active broadcasts
   - Total listeners across all broadcasts
   - Module thread count
   - Aggregate producer CPU

3. **Alerts**:
   - Producer tick p99 > 1 ms (orange) / > 5 ms (red)
   - Resync rate > 1/listener/min (orange)
   - Broadcast unexpectedly destroyed (red)
   - Recording failure (red)
   - Listener count drop > 10% in 60 s (orange — possible network issue)

### 25.4 Capacity Planning

For a 32-core / 128 GB host:

| Listeners | Broadcasts | CPU % (estimate) | RAM | Notes |
|---|---|---|---|---|
| 500 | 1 | 5–10% | ~2 GB | Comfortable |
| 1000 | 1 | 10–20% | ~3 GB | Good |
| 2000 | 1 | 20–40% | ~6 GB | Approaches RTP thread limits |
| 5000 | 5 (1k each) | 50–80% | ~15 GB | Horizontal scaling recommended |

Beyond 5000, distribute across multiple FreeSWITCH hosts with a shared control plane.

### 25.5 Common Operational Tasks

**Start a broadcast manually for testing**:

```
originate {origination_caller_id_name='Test Speaker'}user/1000 &broadcast(test_room+role=speaker)
```

**Add a listener manually**:

```
originate {origination_caller_id_name='Test Listener'}user/1001 &broadcast(test_room+role=listener)
```

**Promote a listener**:

```
broadcast test_room set_speaker m_005
```

**Kick a misbehaving listener**:

```
broadcast test_room kick m_005
```

**Lock the broadcast (no new joiners)**:

```
broadcast test_room lock
```

**Tear down**:

```
broadcast test_room destroy
```

### 25.6 Troubleshooting Decision Tree

**Listeners hear no audio:**
1. Check `broadcast <name> info` — is `Speaker` set?
2. If speaker set, check `broadcast <name> producer` — are ticks happening?
3. If producer running, check `broadcast <name> listeners` — what's the lag per listener?
4. If lag is zero but no audio, check listener's session: `uuid_dump <listener-uuid>` — codec negotiation, RTP stats.
5. If specific listener affected, check their network/endpoint.

**High producer tick time:**
1. Check `top -H` — is the producer thread CPU-bound?
2. Check `vmstat 1` — is `st` (steal-time) elevated? Move to bare metal or pin CPU.
3. Check `perf top -p $(pidof freeswitch)` — what function dominates?
4. Check for runaway logging — set log level to INFO if currently DEBUG.

**Listener resyncs accumulating:**
1. Check listener's endpoint clock stability — is it a particular SIP UA?
2. Check listener's network — RTP packet loss in `uuid_dump`?
3. If many listeners affected simultaneously, check producer for stalls.

**Recording file empty or corrupt:**
1. Check disk space.
2. Check file permissions on `record-template` path.
3. Check `broadcast::recording-stop` event for `Reason`.
4. Check `mod_shout` loaded if using streaming URL.

### 25.7 Upgrade Procedure

1. Build new module on staging.
2. Drain a host: stop accepting new broadcasts.
3. Wait for active broadcasts to end naturally (or destroy with operator notification).
4. `unload mod_broadcast`.
5. Replace `.so`.
6. `load mod_broadcast`.
7. Resume accepting broadcasts.

Hot-reload of configuration only: `broadcast reload` re-reads `broadcast.conf.xml` without affecting active broadcasts.

---

## 26. Test Strategy

### 26.1 Unit Tests

Use FreeSWITCH's existing `mod_test` infrastructure or a standalone harness.

Cover:

- Broadcast create/destroy lifecycle.
- Member add/remove with concurrent producer activity.
- Speaker assignment, demotion, replacement.
- Ring write/read correctness across wrap.
- Lag detection and snap-forward behavior.
- Torn-read defense.
- Recording write integrity (compare written WAV to known input).
- Event emission and subscriber receipt.

### 26.2 Integration Tests

Use SIPp scenarios:

- `test_speaker_simple.xml` — speaker joins, plays a tone, leaves.
- `test_listener_simple.xml` — listener joins, expects audio, leaves.
- `test_one_speaker_many_listeners.xml` — orchestration of 1 speaker + N listeners.
- `test_speaker_handoff.xml` — speaker A leaves, speaker B promoted.
- `test_listener_churn.xml` — listeners constantly joining and leaving while speaker active.

### 26.3 Load Tests

Use SIPp with high concurrency:

```bash
# 1000 listeners, ramp at 50/sec
sipp -sf listener.xml -m 1000 -r 50 -rp 1000 -l 1000 \
     -s broadcast_listen_test -i 127.0.0.1 127.0.0.1
```

Measure:

- Listener join latency p50, p95, p99.
- Producer tick stats during ramp and steady state.
- Memory growth.
- File descriptor and thread count.

### 26.4 Chaos Tests

- Kill speaker SIP UA mid-session → verify silence policy engages.
- Sever network to 50% of listeners → verify others unaffected, lag detection works.
- Fill disk while recording → verify recording aborts gracefully.
- `fsctl shutdown` mid-broadcast → verify clean shutdown, no crash.
- Restart FreeSWITCH 10 times in succession → verify no resource leak.

### 26.5 Long-Duration Tests

Run a single broadcast with 100 listeners for 24 hours:

- Producer tick stats should remain steady.
- No memory growth.
- No resync accumulation.
- Recording file should be ~24 hours of audio, no gaps.

### 26.6 Codec Coverage

Test with each common codec on speaker and listener sides:

- PCMU (G.711 µ-law)
- PCMA (G.711 A-law)
- G.722
- Opus 8 kHz
- Opus 16 kHz
- Opus 48 kHz
- GSM
- AMR-NB (if mod_amr available)

Mixed codecs: speaker = Opus 48 kHz, listeners = mix of PCMU/Opus/G.722. Verify all hear correct audio.

---

## 27. Roadmap

### 27.1 v1 (this specification)

- Single-host audio broadcast as described.
- SPMC ring with per-listener consumer_seq.
- Speaker reassignment.
- Recording.
- Events.
- CLI/API.

### 27.2 v2 (next 6 months)

- **WebRTC/Verto listener support**: listeners join via WebRTC instead of SIP. The ring buffer is codec-agnostic; only the per-listener encode path needs WebRTC awareness.
- **Multiple simultaneous speakers** (true conference subset): up to 4 speakers mixed for listeners. Adds optional mini-mixer thread for the speaker set.
- **Listener-side jitter buffer**: per-listener pre-buffer to absorb network jitter independently.
- **Profile inheritance**: `<profile name="X" extends="default">`.
- **Hot-reload of running broadcasts**: change profile parameters on a live broadcast.
- **Prometheus exporter built-in**: no Lua sidecar needed.
- **Multi-language announcements**: per-listener `enter-sound` based on channel language variable.

### 27.3 v3 (12+ months)

- **Video broadcast**: single video producer fanned to listeners. Uses simulcast for codec/bitrate variants. Requires `mod_av` infrastructure.
- **Cross-host distribution**: broadcasts replicated to multiple FreeSWITCH hosts via a clustering protocol. Each host runs its local listener pool.
- **Persistent state**: broadcasts survive FreeSWITCH restart (state in Redis or PostgreSQL). Listeners must reconnect but the broadcast object persists.
- **End-to-end encryption**: SRTP per-leg already supported by core; add broadcast-level group key for additional confidentiality.
- **Adaptive bitrate** per listener based on RTCP feedback.

### 27.4 Won't Build

Features explicitly out of scope:

- Mixing of listener audio into speaker's output (this is conference; use `mod_conference`).
- Spatial audio / positional mixing.
- AI features (transcription, translation, summarization) — these belong in `mod_audio_fork` or a separate processing pipeline tapping recording outputs.

---

## 28. Skeleton Source Layout

The following is the minimum viable skeleton for v1. Each section is annotated with what to implement.

### 28.1 `mod_broadcast.h`

```c
#ifndef MOD_BROADCAST_H
#define MOD_BROADCAST_H

#include <switch.h>

/* configuration constants */
#define BROADCAST_DEFAULT_RING_SIZE       50
#define BROADCAST_DEFAULT_MAX_LAG_FRAMES  10
#define BROADCAST_MAX_FRAME_BYTES         1920
#define BROADCAST_DEFAULT_INTERVAL_MS     20
#define BROADCAST_DEFAULT_RATE            8000
#define BROADCAST_EVENT_RING_SIZE         1024

/* forward declarations */
typedef struct broadcast_obj    broadcast_obj_t;
typedef struct member_obj       member_obj_t;
typedef struct ring_frame       ring_frame_t;
typedef struct rec_consumer     rec_consumer_t;
typedef struct evt_msg          evt_msg_t;
typedef struct broadcast_profile broadcast_profile_t;

typedef enum {
    BMEMBER_ROLE_SPEAKER  = 1,
    BMEMBER_ROLE_LISTENER = 2,
    BMEMBER_ROLE_OBSERVER = 3
} bmember_role_t;

typedef enum {
    BEVT_BROADCAST_CREATE = 1,
    BEVT_BROADCAST_DESTROY,
    BEVT_SPEAKER_SET,
    BEVT_SPEAKER_CLEAR,
    BEVT_SPEAKER_TALKING,
    BEVT_SPEAKER_SILENT,
    BEVT_LISTENER_JOIN,
    BEVT_LISTENER_LEAVE,
    BEVT_LISTENER_RESYNC,
    BEVT_LISTENER_KICKED,
    BEVT_RECORDING_START,
    BEVT_RECORDING_STOP,
    BEVT_PAUSE,
    BEVT_RESUME,
    BEVT_LOCK,
    BEVT_UNLOCK
} bevt_type_t;

/* flag bits */
#define BFLAG_RUNNING       (1U << 0)
#define BFLAG_DESTRUCT      (1U << 1)
#define BFLAG_LOCKED        (1U << 2)
#define BFLAG_RECORDING     (1U << 3)
#define BFLAG_PAUSE         (1U << 4)
#define BFLAG_DYNAMIC       (1U << 5)

#define MFLAG_RUNNING       (1U << 0)
#define MFLAG_ITHREAD       (1U << 1)
#define MFLAG_KICKED        (1U << 2)
#define MFLAG_GHOST         (1U << 3)

/* all data structures (broadcast_obj, member_obj, ring_frame, etc.)
   as defined in §5 above */

/* function prototypes */
broadcast_obj_t *broadcast_find(const char *name);
broadcast_obj_t *broadcast_create(const char *name, const char *profile_name,
                                   switch_memory_pool_t *parent_pool);
switch_status_t  broadcast_destroy(broadcast_obj_t *b);

switch_status_t  broadcast_member_add(broadcast_obj_t *b, member_obj_t *m);
switch_status_t  broadcast_member_del(broadcast_obj_t *b, member_obj_t *m);

switch_status_t  broadcast_set_speaker(broadcast_obj_t *b, member_obj_t *m);
switch_status_t  broadcast_clear_speaker(broadcast_obj_t *b);

void *SWITCH_THREAD_FUNC broadcast_producer_run(switch_thread_t *thread, void *obj);
void *SWITCH_THREAD_FUNC broadcast_speaker_input_run(switch_thread_t *thread, void *obj);
void                     broadcast_listener_run(member_obj_t *m);

switch_status_t  broadcast_record_start(broadcast_obj_t *b, const char *target);
switch_status_t  broadcast_record_stop(broadcast_obj_t *b, const char *target);

void enqueue_event(broadcast_obj_t *b, bevt_type_t type, uint32_t member_id);
void *SWITCH_THREAD_FUNC broadcast_event_emitter_run(switch_thread_t *thread, void *obj);

#endif /* MOD_BROADCAST_H */
```

### 28.2 `mod_broadcast.c`

```c
#include "mod_broadcast.h"

/* module global state */
struct broadcast_globals broadcast_globals;

SWITCH_MODULE_LOAD_FUNCTION(mod_broadcast_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_broadcast_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_broadcast_runtime);

SWITCH_MODULE_DEFINITION(mod_broadcast, mod_broadcast_load,
                         mod_broadcast_shutdown, mod_broadcast_runtime);

/* dialplan application: broadcast */
SWITCH_STANDARD_APP(broadcast_app_function)
{
    char *params = NULL, *name = NULL, *profile_name = NULL;
    member_obj_t *m;
    broadcast_obj_t *b;
    bmember_role_t role = BMEMBER_ROLE_LISTENER;

    /* parse data string: name[@profile][+key=val,...] */
    parse_data_string(data, &name, &profile_name, &params);

    /* find or create broadcast */
    b = broadcast_find(name);
    if (!b) {
        b = broadcast_create(name, profile_name ? profile_name : "default", NULL);
        if (!b) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                              SWITCH_LOG_ERROR, "broadcast create failed\n");
            return;
        }
    }

    /* allocate member */
    m = switch_core_session_alloc(session, sizeof(*m));
    m->session = session;
    m->channel = switch_core_session_get_channel(session);
    m->pool = switch_core_session_get_pool(session);
    m->broadcast = b;
    m->uuid = switch_core_strdup(m->pool, switch_core_session_get_uuid(session));

    /* parse role */
    if (params && strstr(params, "role=speaker")) role = BMEMBER_ROLE_SPEAKER;
    m->role = role;

    /* setup codecs */
    setup_member_codecs(b, m);

    /* add to broadcast */
    if (role == BMEMBER_ROLE_SPEAKER) {
        broadcast_set_speaker(b, m);
    } else {
        broadcast_member_add(b, m);
    }

    /* set channel variables */
    switch_channel_set_variable(m->channel, "broadcast_name", b->name);
    switch_channel_set_variable_printf(m->channel, "broadcast_member_id",
                                       "%u", m->id);

    /* run the appropriate loop (this BLOCKS until member leaves) */
    if (role == BMEMBER_ROLE_LISTENER) {
        broadcast_listener_run(m);
    } else {
        /* speaker runs input thread separately; main thread waits */
        broadcast_speaker_main_wait(m);
    }

    /* cleanup */
    if (role == BMEMBER_ROLE_SPEAKER) {
        broadcast_clear_speaker(b);
    } else {
        broadcast_member_del(b, m);
    }

    cleanup_member_codecs(m);
}

/* API function: broadcast <subcommand> */
SWITCH_STANDARD_API(broadcast_api_function)
{
    char *argv[10];
    int argc;
    char *mycmd = strdup(cmd ? cmd : "");

    argc = switch_separate_string(mycmd, ' ', argv, 10);

    if (argc < 1) {
        stream->write_function(stream, "-ERR usage\n");
    } else if (!strcasecmp(argv[0], "list")) {
        broadcast_api_list(stream);
    } else if (!strcasecmp(argv[0], "reload")) {
        broadcast_api_reload(stream);
    } else if (argc >= 2) {
        broadcast_obj_t *b = broadcast_find(argv[0]);
        if (!b) {
            stream->write_function(stream, "-ERR no such broadcast\n");
        } else if (!strcasecmp(argv[1], "info")) {
            broadcast_api_info(b, stream);
        } else if (!strcasecmp(argv[1], "listeners")) {
            broadcast_api_listeners(b, stream);
        } else if (!strcasecmp(argv[1], "set_speaker") && argc >= 3) {
            broadcast_api_set_speaker(b, argv[2], stream);
        } /* ... etc ... */
    }

    free(mycmd);
    return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_broadcast_load)
{
    switch_application_interface_t *app_interface;
    switch_api_interface_t *api_interface;

    memset(&broadcast_globals, 0, sizeof(broadcast_globals));
    broadcast_globals.pool = pool;
    broadcast_globals.running = 1;

    switch_core_hash_init(&broadcast_globals.broadcast_hash);
    switch_thread_rwlock_create(&broadcast_globals.hash_rwlock, pool);

    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    if (broadcast_config_load() != SWITCH_STATUS_SUCCESS) {
        return SWITCH_STATUS_GENERR;
    }

    SWITCH_ADD_APP(app_interface, "broadcast",
                   "Join a broadcast",
                   "One-to-many audio broadcast",
                   broadcast_app_function,
                   "<name>[@profile][+key=val,...]",
                   SAF_NONE);

    SWITCH_ADD_API(api_interface, "broadcast",
                   "Broadcast control commands",
                   broadcast_api_function,
                   "broadcast <command> [args]");

    /* reserve event subclasses */
    switch_event_reserve_subclass("broadcast::create");
    switch_event_reserve_subclass("broadcast::destroy");
    switch_event_reserve_subclass("broadcast::speaker-set");
    switch_event_reserve_subclass("broadcast::speaker-clear");
    switch_event_reserve_subclass("broadcast::speaker-talking");
    switch_event_reserve_subclass("broadcast::speaker-silent");
    switch_event_reserve_subclass("broadcast::listener-join");
    switch_event_reserve_subclass("broadcast::listener-leave");
    switch_event_reserve_subclass("broadcast::listener-resync");
    switch_event_reserve_subclass("broadcast::listener-kicked");
    switch_event_reserve_subclass("broadcast::recording-start");
    switch_event_reserve_subclass("broadcast::recording-stop");
    switch_event_reserve_subclass("broadcast::pause");
    switch_event_reserve_subclass("broadcast::resume");
    switch_event_reserve_subclass("broadcast::lock");
    switch_event_reserve_subclass("broadcast::unlock");
    switch_event_reserve_subclass("broadcast::heartbeat");

    broadcast_event_emitter_start();

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                      "mod_broadcast v1.0 loaded\n");
    return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_broadcast_shutdown)
{
    broadcast_globals.running = 0;

    /* tear down all broadcasts */
    broadcast_destroy_all();

    broadcast_event_emitter_stop();

    switch_event_free_subclass("broadcast::create");
    switch_event_free_subclass("broadcast::destroy");
    /* ... free all subclasses ... */

    switch_core_hash_destroy(&broadcast_globals.broadcast_hash);

    return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_RUNTIME_FUNCTION(mod_broadcast_runtime)
{
    while (broadcast_globals.running) {
        switch_yield(5000000);
        broadcast_runtime_heartbeat();
    }
    return SWITCH_STATUS_TERM;
}
```

### 28.3 `broadcast_producer.c` (key function)

```c
#include "mod_broadcast.h"

void *SWITCH_THREAD_FUNC broadcast_producer_run(switch_thread_t *thread, void *obj)
{
    broadcast_obj_t *b = (broadcast_obj_t *)obj;
    switch_core_timer_t timer;
    ring_frame_t local_frame;
    uint8_t silence[BROADCAST_MAX_FRAME_BYTES];
    uint32_t silence_len;

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                      "broadcast '%s' producer starting\n", b->name);

    if (switch_core_timer_init(&timer, "timerfd", b->interval_ms,
                               b->samples_per_frame, b->pool) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "broadcast '%s' timer init failed\n", b->name);
        return NULL;
    }

    memset(silence, 0, sizeof(silence));
    silence_len = b->bytes_per_frame;
    b->producer_running = 1;

    while (b->producer_running && !b->destroyed) {
        switch_time_t t_start = switch_micro_time_now();
        switch_core_timer_next(&timer);

        /* snapshot speaker */
        member_obj_t *spk = NULL;
        switch_thread_rwlock_rdlock(b->speaker_lock);
        spk = b->current_speaker;
        switch_thread_rwlock_unlock(b->speaker_lock);

        /* assemble frame */
        local_frame.datalen = 0;
        local_frame.silence = 1;

        if (spk && !(b->cflags & BFLAG_PAUSE)) {
            switch_mutex_lock(spk->audio_in_mutex);
            switch_size_t want = b->bytes_per_frame;
            switch_size_t got = switch_buffer_read(spk->audio_buffer,
                                                    local_frame.data, want);
            switch_mutex_unlock(spk->audio_in_mutex);

            if (got == want) {
                local_frame.datalen = (uint32_t)got;
                local_frame.silence = 0;
            }
        }

        if (local_frame.silence) {
            /* apply silence policy */
            apply_silence_policy(b, local_frame.data, &local_frame.datalen);
        }

        local_frame.samples = b->samples_per_frame;

        /* publish */
        uint64_t seq = __atomic_load_n(&b->producer_seq.counter, __ATOMIC_RELAXED);
        ring_frame_t *slot = &b->ring[seq % b->ring_size];

        slot->datalen = local_frame.datalen;
        slot->samples = local_frame.samples;
        slot->silence = local_frame.silence;
        memcpy(slot->data, local_frame.data, local_frame.datalen);
        __atomic_store_n(&slot->seq, seq, __ATOMIC_RELEASE);

        __atomic_store_n(&b->producer_seq.counter, seq + 1, __ATOMIC_RELEASE);

        /* stats */
        switch_atomic_inc(&b->stat_ticks);
        if (local_frame.silence) switch_atomic_inc(&b->stat_speaker_silent_ticks);

        switch_time_t t_end = switch_micro_time_now();
        if (t_end - t_start > b->interval_ms * 1000 * 2) {
            switch_atomic_inc(&b->stat_missed_ticks);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                              "broadcast '%s' producer tick over budget: %lu us\n",
                              b->name, t_end - t_start);
        }
    }

    switch_core_timer_destroy(&timer);
    b->producer_running = 0;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                      "broadcast '%s' producer stopped\n", b->name);
    return NULL;
}
```

### 28.4 `Makefile.am`

```makefile
include $(top_srcdir)/build/modmake.rulesam

MODNAME = mod_broadcast

mod_LTLIBRARIES = mod_broadcast.la

mod_broadcast_la_SOURCES = \
    mod_broadcast.c \
    broadcast_core.c \
    broadcast_member.c \
    broadcast_producer.c \
    broadcast_listener.c \
    broadcast_speaker.c \
    broadcast_record.c \
    broadcast_events.c \
    broadcast_api.c \
    broadcast_config.c

mod_broadcast_la_CFLAGS  = $(AM_CFLAGS)
mod_broadcast_la_LDFLAGS = -avoid-version -module -no-undefined -shared
mod_broadcast_la_LIBADD  = $(switch_builddir)/libfreeswitch.la
```

---

## 29. Glossary

**Broadcast.** A named audio session with one speaker and many listeners managed by `mod_broadcast`. Equivalent in operator parlance to "a room" or "a session."

**Speaker.** The single member of a broadcast whose audio is fanned out to listeners. Reassignable at runtime.

**Listener.** A member of a broadcast who receives audio but does not contribute. Always implicitly muted on the input path.

**Producer.** The thread within a broadcast that reads from the speaker's audio buffer and writes to the ring buffer. One per broadcast.

**Consumer.** Any thread that reads from the broadcast's ring buffer. Listener bridge threads and recording writer threads are consumers.

**Ring buffer.** The fixed-size circular array of audio frames shared between producer and consumers. Indexed by monotonic sequence number modulo ring size.

**SPMC.** Single-producer multiple-consumer. The concurrency pattern used by the ring.

**Producer seq.** The monotonic frame counter maintained by the producer. Atomic; published with release semantics; observed by consumers with acquire semantics.

**Consumer seq.** Each consumer's local frame counter, tracking which frame it will read next.

**Lag.** The difference between producer seq and a consumer's seq at a moment in time. Healthy lag is 0–2; sustained lag >5 indicates clock drift or thread starvation.

**Resync.** The action a consumer takes when its lag exceeds MAX_LAG_FRAMES: snap consumer seq forward to current producer seq, log an event, continue.

**Tick.** One iteration of the producer or consumer loop, gated by `switch_core_timer_next`. Typically 20 ms.

**Frame.** One unit of audio in the ring. Contains samples for one tick interval at the broadcast's configured rate and channels.

**Silence policy.** The producer's behavior when no speaker is assigned or speaker is silent. One of: zero PCM, comfort noise, music-on-hold playback.

**Profile.** A named bundle of configuration parameters in `broadcast.conf.xml`, applied to broadcasts that reference it.

**Ghost.** A member flagged so they do not count toward listener count and do not trigger join/leave announcements. Used for system-controlled bridges (e.g., recording sinks if implemented as ghosts).

**Recording consumer.** A non-channel consumer of the ring, writing frames to a file or stream.

**Event emitter.** The single module-wide thread that drains per-broadcast event rings and calls `switch_event_fire`.

---

## 30. References

### 30.1 FreeSWITCH Source

- `src/mod/applications/mod_conference/` — reference implementation of the conference model this module is designed to replace for one-to-many use cases.
- `src/switch_core_timer.c` — timer subsystem used by producer and consumer threads.
- `src/switch_buffer.c` — `switch_buffer_t` implementation used for speaker audio buffer.
- `src/switch_thread_pool.c` — APR thread management.
- `src/include/switch_apr.h` — synchronization primitives.

### 30.2 Concurrency Patterns

- "The LMAX Disruptor: A High Performance Inter-Thread Messaging Library." Martin Thompson et al. (2011). The canonical reference for high-performance SPMC ring buffers.
- "Linux Kernel Lock-Free Ring Buffer." kernel.org documentation, `Documentation/trace/ring-buffer-design.rst`.
- "Memory Barriers: A Hardware View for Software Hackers." Paul E. McKenney. Foundational reading on memory ordering.

### 30.3 FreeSWITCH Issue References

- Issue #2676 — `mod_conference` deadlock under heavy listener join/leave. Background motivation for this module's lock discipline.
- Issue #1661 — `mod_conference` deadlock with `energy all` and member leave. Background for separating control commands onto their own mutex.
- Issue #1437 — `mod_conference` SENDONLY audio direction bug. Background for not using SDP direction as a participation mechanism.

### 30.4 Related Modules

- `mod_conference` — N-to-N mixer; complementary to `mod_broadcast`.
- `mod_local_stream` — file-based one-to-many; alternative for non-interactive broadcast.
- `mod_shout` — Icecast streaming; useful as a recording target.
- `mod_av` — video and advanced audio formats; future video extension dependency.

---

**End of specification, v1.0.**

This document is intended to be sufficient for an experienced FreeSWITCH C developer to implement `mod_broadcast` without further architectural decisions required. Sections 28 (Skeleton Source Layout) and 21.7 (Benchmarking Methodology) should be treated as starting points; details such as exact CLI parser semantics, full event header sets, and config XML edge cases will be refined during implementation against the FreeSWITCH 1.10.12 codebase.
