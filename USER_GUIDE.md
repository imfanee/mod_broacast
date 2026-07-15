# mod_broadcast — User Guide

This document is for **operators, dialplan authors, and integrators** who run FreeSWITCH with `mod_broadcast`. It describes what the module does, how to configure it, how to use the dialplan application and CLI/API, which **CUSTOM** events are emitted, and practical scenarios.

For C API and internal architecture, see `mod_broadcast.h` and `mod_broadcast_spec.md` in the same directory.

---

## 1. What is mod_broadcast?

**mod_broadcast** provides a **one-to-many audio broadcast** on a single FreeSWITCH node:

- One **speaker** (optional) feeds audio into a shared **ring buffer**.
- A **producer** thread ticks at the profile **interval** (e.g. every 20 ms), advances sequence, and applies **silence policy** when the speaker is not sending usable audio.
- Many **listeners** each read from the ring and write linear PCM (L16) to their channel at the same rate/interval.

It is **not** a conference bridge: there is **no mixing of multiple talkers**. Only the designated speaker’s audio is distributed. Use `mod_conference` if you need multi-party talk or floor control.

Typical uses: lectures, sermons, town-hall listen-only lines, training “listen” legs, public-address style distribution.

---

## 2. Core concepts

| Term | Meaning |
|------|--------|
| **Broadcast** | A named session object (hash key) with its own memory pool, ring, producer thread, listener list, optional speaker, optional recording. |
| **Profile** | Named template in `broadcast.conf.xml`. When a broadcast is **created**, the profile is **cloned** into that broadcast’s pool (snapshot). Reload changes **new** broadcasts, not existing snapshots. |
| **Speaker** | At most one current speaker per broadcast. Dialplan `role=speaker` or API `set_speaker` / `promote_listener`. |
| **Listener** | Default role; joins the listener list and runs the listener audio loop until hangup or kick. |
| **Producer** | Per-broadcast thread that drives timing and ring writes. |
| **Caller controls** | Named DTMF binding groups in XML; listeners match digit sequences to actions (hangup, request-speaker, energy-up/down). |

---

## 3. Installation and loading

1. **Build** the module (from your FreeSWITCH source tree):

   ```bash
   make mod_broadcast-install
   ```

   Or ensure `applications/mod_broadcast` is enabled in `modules.conf` and build/install as usual.

2. **Enable** in `conf/autoload_configs/modules.conf.xml` (or equivalent):

   ```xml
   <load module="mod_broadcast"/>
   ```

3. **Configuration** file (installed with the module):

   `conf/autoload_configs/broadcast.conf.xml`

4. After editing XML on disk:

   ```text
   fs_cli> reloadxml
   fs_cli> broadcast reload
   ```

   `broadcast reload` re-reads **only** `broadcast.conf.xml` (profiles, settings, caller-controls). **Active broadcasts keep their cloned profile**; new creates use the new definitions.

---

## 4. Configuration: `broadcast.conf.xml`

**File name (internal):** `broadcast.conf`  
**Path:** `conf/autoload_configs/broadcast.conf.xml`  
**Root element:** `<configuration name="broadcast.conf" ...>`

### 4.1 `<settings>` — module-wide

| Parameter | Default (if omitted in sample) | Description |
|-----------|--------------------------------|-------------|
| `default-profile` | `default` | Profile name used when the dialplan does not specify `@profile`. |
| `max-broadcasts` | `500` | Maximum number of **active** broadcasts (hash entries). Additional `broadcast_create` attempts fail until some are destroyed. |
| `event-ring-size` | `1024` | Per-broadcast queued **non-critical** event ring size (CUSTOM events). Under extreme load, oldest queued events may be dropped; counters are visible in `broadcast <name> stats`. |

### 4.2 `<profiles>` / `<profile>`

Each `<profile name="...">` contains `<param name="..." value="..."/>` entries.

| Parameter | Description |
|-----------|-------------|
| `rate` | Sample rate (Hz), e.g. `8000`, `16000`, `48000`. |
| `interval` | Frame interval in **milliseconds** (e.g. `20`). |
| `channels` | Channel count (typically `1`). |
| `ring-size` | Number of frames in the ring buffer. |
| `max-lag-frames` | Listener-side lag threshold before resync behavior (see listener tuning in code/spec). |
| `speaker-override` | `true` / `false`. If `false`, a second dialplan speaker join may be rejected when a speaker already exists. |
| `silence-window-ms` | For energy/VAD on speaker: how long below threshold before “silent” (ms). |
| `silence-policy` | `zero` \| `cn` \| `moh:filename` — how the producer fills silence when the speaker path is quiet/empty (see sample comments in shipped XML). |
| `moh-sound` | MOH-related sound (used per policy / implementation). |
| `max-listeners` | Cap on listeners for this profile (enforced on `listener` join). |
| `listener-pre-buffer-ms` | Optional “snap forward” pre-buffer for new listeners (see shipped XML comment). |
| `timer-name` | Timer driver name (e.g. `timerfd`). |
| `energy-detection` | `true` / `false` — speaker energy / talking detection (emits talking/silent events when enabled). |
| `comfort-noise-level` | Comfort noise level (0 = off unless policy uses CN). |
| `agc` | Automatic gain control flag (`true` / `false`) — profile field; behavior follows implementation. |
| `enter-sound` / `exit-sound` / `kicked-sound` / `locked-sound` / `speaker-joined-sound` | Optional sound file paths (empty = disabled). |
| `verbose-events` | More chatty event behavior when `true` (implementation-dependent details). |
| `join-event-delay-ms` | Delays join-related events (ms) when non-zero. |
| `caller-controls` | Name of a `<group>` under `<caller-controls>` for DTMF bindings. |
| `auto-record` | `true` / `false` — start recording when broadcast is created (path from `record-template`). |
| `record-template` | File path template for recordings; supports channel variables (e.g. `${name}`, `${strftime(...)}`). |

A **minimal extra profile** example:

```xml
<profile name="webinar">
  <param name="rate" value="16000"/>
  <param name="interval" value="20"/>
  <param name="ring-size" value="80"/>
  <param name="max-listeners" value="5000"/>
  <param name="caller-controls" value="default"/>
</profile>
```

### 4.3 `<caller-controls>` / `<group>` / `<control>`

Maps **digit strings** to **actions** for listeners (exact string match after DTMF buffering logic in the listener).

| `action` value | Behavior (listener) |
|----------------|---------------------|
| `hangup` | Hang up the listener’s channel. |
| `request-speaker` | Logs a request; **does not auto-promote** — use API `set_speaker` / `promote_listener` from an external controller or IVR. |
| `energy-up` | Adjusts internal energy threshold (used with energy features). |
| `energy-down` | Adjusts internal energy threshold. |

Example (from shipped file):

```xml
<group name="lecture-controls">
  <control action="hangup" digits="0"/>
  <control action="request-speaker" digits="1"/>
  <control action="energy-up" digits="*1"/>
  <control action="energy-down" digits="*2"/>
</group>
```

### 4.4 Listener DTMF (caller controls)

While a **listener** is in `broadcast_listener_run`, inbound DTMF digits are collected and matched against the profile’s **caller-controls** group (`broadcast_listener_handle_dtmf`). Digit sequences must match a `<control digits="...">` entry exactly (subject to the module’s DTMF buffer timing). Speaker legs do not use this path in the same way; use API or dialplan for speaker changes.

---

## 5. Dialplan application: `broadcast`

**Application name:** `broadcast`  
**Syntax (data string):**

```text
<name>[@<profile>][+<key=value,...>]
```

### 5.1 Components

- **`name`** (required)  
  Logical broadcast name. If it does not exist, the first join **creates** the broadcast using `profile` (or `default-profile` from settings).

- **`@profile`** (optional)  
  Profile template name **only used on create**. Example: `townhall@lecture`.

- **`+key=value,...`** (optional)  
  Comma-separated parameters after the first `+`.

### 5.2 Parameters (`+` section)

| Key | Values | Default | Description |
|-----|--------|---------|-------------|
| `role` | `listener`, `speaker` | `listener` | Join as listener or speaker. |
| `ghost` | `true`, `false` | `false` | Ghost members are excluded from some stats/events (see implementation). |
| `exclusive` | `true`, `false` | `false` | If `true` and the broadcast **already exists**, join is **rejected** (busy). Does not block first create. |
| `tag` | string | (empty) | Stored on member; may appear on events when applicable. |
| `display_name` | string | (empty) | Stored on member for display-oriented uses. |

### 5.3 Examples

```xml
<!-- Create or join as listener (default profile) -->
<action application="broadcast" data="annual-meeting"/>

<!-- Create with explicit profile -->
<action application="broadcast" data="webinar@high-quality"/>

<!-- Speaker (first speaker wins unless profile allows override) -->
<action application="broadcast" data="webinar@lecture+role=speaker"/>

<!-- Listener with tag -->
<action application="broadcast" data="webinar+role=listener,tag=vip"/>

<!-- First channel creates; second exclusive join fails busy -->
<action application="broadcast" data="private-room+exclusive=true"/>
```

### 5.4 Channel variables set by the module

The module sets variables on the channel while in the app (useful for CDR, logging, ESL):

| Variable | Example / meaning |
|----------|-------------------|
| `broadcast_name` | Broadcast logical name. |
| `broadcast_uuid` | Broadcast instance UUID string. |
| `broadcast_role` | `speaker` or `listener`. |
| `broadcast_join_time` | Join time (epoch seconds, string). |
| `broadcast_profile` | Cloned profile name in use. |
| `broadcast_member_id` | Numeric member id (e.g. `42`). |
| `broadcast_resync_count` | Listener resync count (listener path). |
| `broadcast_total_duration_ms` | Listener bridge duration (listener path). |
| `broadcast_left_reason` | `hangup`, `kicked`, `destroyed`, `error`, etc. |

---

## 6. CLI / API: `broadcast`

Registered API command: **`broadcast`** (same as `fs_cli` command name).

General forms:

```text
broadcast <subcommand> ...
broadcast --json <subcommand> ...
```

JSON mode prefixes `--json` after the word `broadcast` (see implementation): e.g.

```text
broadcast --json list
broadcast --json myroom info
```

### 6.1 Global commands (no broadcast name)

| Command | Description |
|---------|-------------|
| `broadcast version` | Prints module version string. |
| `broadcast reload` | Reloads `broadcast.conf.xml` (profiles, settings, caller-controls). |
| `broadcast list` | Lists broadcasts (text). |
| `broadcast list <filter>` | Lists broadcasts whose name matches filter (implementation: name match). |

### 6.2 Per-broadcast commands

Form: `broadcast <name> <subcommand> [args...]`

| Subcommand | Arguments | Description |
|------------|-----------|-------------|
| `info` | — | Name, UUID, profile, listener count, producer seq, basic stats. |
| `listeners` | — | Lists listeners with lag/resync info (text or JSON). |
| `stats` | — | Tick / resync / join / leave / dropped-event counters. |
| `producer` | — | Producer running flag, sequence, timing averages. |
| `set_speaker` | `<member>` | Sets speaker to member id. Accepts `42` or `m_42`. |
| `promote_listener` | `<member>` | Same as `set_speaker` in current implementation (listener must not be in active bridge restrictions apply — see code). |
| `clear_speaker` | — | Clears current speaker. |
| `kick` | `<member>` | Kicks a **listener** by id. |
| `kick_all` | — | Kicks all listeners. |
| `lock` | — | Sets locked flag; **new listener joins** (`broadcast_listener_add`) are rejected while locked. |
| `unlock` | — | Clears locked flag. |
| `pause` | — | Pause flag + event. |
| `resume` | — | Resume flag + event. |
| `record` | `<path>` | Starts recording to path. |
| `norecord` | — | Stops recording. |
| `destroy` | — | Tears down broadcast (kicks sessions, stops producer, removes from hash). **Destructive.** |
| `dtmf` | `<member> <digits>` | Injects DTMF string to a member’s session. |
| `play` | `<file>` | Plays audio file into broadcast play path (implementation uses file open on broadcast). |
| `stop_play` | — | Stops play file. |

### 6.3 CLI examples

```text
fs_cli> broadcast list
fs_cli> broadcast townhall info
fs_cli> broadcast townhall listeners
fs_cli> broadcast townhall set_speaker m_7
fs_cli> broadcast townhall kick 12
fs_cli> broadcast townhall record /var/spool/freeswitch/townhall.wav
fs_cli> broadcast townhall destroy
```

### 6.4 ESL / HTTP API

Use the standard FreeSWITCH API interface:

```text
api broadcast list
api broadcast myroom info
bgapi broadcast myroom destroy
```

In **ESL**, subscribe to events (see next section) and send `api` / `bgapi` commands as usual.

---

## 7. Events (CUSTOM)

All subclasses use **`SWITCH_EVENT_CUSTOM`** with subclass names of the form:

```text
broadcast::<subclass>
```

Subclasses include:

| Subclass | Typical meaning |
|----------|-----------------|
| `broadcast::create` | Broadcast created. |
| `broadcast::destroy` | Broadcast destroyed. |
| `broadcast::speaker-set` | Speaker assigned. |
| `broadcast::speaker-clear` | Speaker cleared. |
| `broadcast::speaker-talking` | Energy / VAD talking (data in headers). |
| `broadcast::speaker-silent` | Energy / VAD silent. |
| `broadcast::listener-join` | Listener joined. |
| `broadcast::listener-leave` | Listener left. |
| `broadcast::listener-resync` | Listener resynced (lag). |
| `broadcast::listener-kicked` | Listener kicked. |
| `broadcast::recording-start` | Recording started (path in data / headers). |
| `broadcast::recording-stop` | Recording stopped (optional `Reason` header). |
| `broadcast::pause` / `broadcast::resume` | Pause/resume. |
| `broadcast::lock` / `broadcast::unlock` | Lock/unlock. |

### 7.1 Common headers

Most events include:

- `Broadcast-Name`
- `Broadcast-UUID`
- `Broadcast-Profile`
- `Timestamp`
- `Member-ID` (when applicable)

Listener join/leave may add:

- `Member-UUID`
- `Caller-ID-Name`, `Caller-ID-Number`
- `Tag` (if set on member)
- `Listener-Count`

Talking/silent events may add:

- `Broadcast-Event-Data` / `Energy`
- `Silence-Window-Ms`

### 7.2 fs_cli subscription example

```text
event plain CUSTOM broadcast::%
```

Or restrict:

```text
event plain CUSTOM broadcast::listener-join broadcast::listener-leave broadcast::speaker-set broadcast::speaker-clear
```

---

## 8. JSON output (`--json`)

Several commands support machine-readable JSON (one JSON object per line where applicable). Examples from code paths:

- `broadcast --json list`
- `broadcast --json <name> info`
- `broadcast --json <name> listeners`
- `broadcast --json <name> stats`
- `broadcast --json <name> producer`
- `broadcast --json version`
- `broadcast --json reload`

On error, responses include `"result":"error"` and a `"detail"` string where implemented.

---

## 9. Operational notes

1. **Destroy vs joins**  
   `destroy` is cooperative with timeouts (listeners/speaker paths). Do not assume instantaneous teardown under load.

2. **Reload**  
   `broadcast reload` is safe for **running** broadcasts’ audio state because profiles are **cloned at create**. New settings apply to **new** broadcasts.  
   Config lookups during reload are protected so concurrent **create** does not read freed profile memory.

3. **Speaker vs listener**  
   A **listener** is the default and the scalable path. A **speaker** holds the write/read codec path appropriate for capture; only one “current speaker” slot exists unless policy overrides.

4. **Caller-controls `request-speaker`**  
   This is a **hint** (log + hook point); promotion is done via **API** (`set_speaker` / `promote_listener`) or dialplan design.

5. **Recording**  
   Paths must be writable by the FreeSWITCH user. Templates support variable expansion where implemented (`record_template` / `record` command path).

---

## 10. Sample scenarios

### 10.1 Simple listen-only line

1. Configure a profile with modest `max-listeners` and `rate` matching your trunks.  
2. Dialplan for inbound DID:

   ```xml
   <action application="answer"/>
   <action application="broadcast" data="nightline@default"/>
   ```

3. Many callers join `nightline`; no speaker — they hear silence policy output (e.g. zero).

### 10.2 Lecture with operator-assigned speaker

1. Listeners:

   ```xml
   <action application="broadcast" data="lecture2026@lecture"/>
   ```

2. Presenter calls a separate DID:

   ```xml
   <action application="broadcast" data="lecture2026@lecture+role=speaker"/>
   ```

3. Ops console (fs_cli):

   ```text
   broadcast lecture2026 listeners
   broadcast lecture2026 set_speaker m_3
   broadcast lecture2026 clear_speaker
   ```

### 10.3 Auto-record webinar

Profile:

```xml
<param name="auto-record" value="true"/>
<param name="record-template" value="/var/spool/freeswitch/webinar/${uuid}.wav"/>
```

First join creates broadcast; recording starts per implementation when auto-record is enabled and template expands.

### 10.4 Clean shutdown before maintenance

```text
broadcast myroom kick_all
broadcast myroom destroy
```

---

## 11. “Functions” and programmatic integration

- **Dialplan:** only the `broadcast` application is user-facing.  
- **API:** the `broadcast` CLI/API command is the supported control plane.  
- **Events:** CUSTOM `broadcast::*` subclasses for external automation (ESL, HTTP event socket, etc.).  
- **C symbols** (`broadcast_create`, `broadcast_destroy`, etc.) are **module-internal** / developer API, not a stable public SDK for user scripts.

---

## 12. Troubleshooting quick reference

| Symptom | Things to check |
|---------|------------------|
| `-ERR no such broadcast` | Name typo; broadcast destroyed; not created yet (first join creates unless exclusive). |
| `max-broadcasts exceeded` | Raise `max-broadcasts` in settings or destroy idle broadcasts. |
| `speaker already present` | Profile `speaker-override=false` and a speaker exists; use API clear or override policy. |
| No audio | Rate/interval mismatch with devices; silence policy; speaker not set; listener not actually in bridge loop (hangup?). |
| Missing events | `event-ring-size` overflow / drops — check `broadcast <n> stats` for dropped events. |

---

## 13. File reference (shipped)

| Path | Purpose |
|------|---------|
| `conf/autoload_configs/broadcast.conf.xml` | Main configuration (installed with `make mod_broadcast-install`). |
| `USER_GUIDE.md` | This document. |
| `mod_broadcast_spec.md` | Full technical specification (developers). |

---

*Module string: see `BROADCAST_MODULE_VERSION` in `mod_broadcast.h` (`fs_cli> broadcast version`).*
