# `mod_broadcast` — High-Scale One-to-Many Audio Broadcasting for FreeSWITCH

`mod_broadcast` is a high-performance, lock-free, one-to-many live audio broadcasting module for FreeSWITCH (1.10+). It is designed to scale to tens of thousands of concurrent listener legs with ultra-low latency and CPU utilization.

---

## 1. Overview & Purpose

In traditional VoIP applications, conferencing modules (such as `mod_conference`) employ an **$O(N^2)$ personalised mixing topology** where every participant's incoming audio is mixed and encoded for every other participant. This creates a severe CPU and thread bottleneck, typically capping standard hardware at ~150 concurrent sessions.

`mod_broadcast` targets **one-to-many audio scenarios** (such as lectures, sermons, town halls, webinars, and public announcements) where a single active speaker's audio is distributed to thousands of passive listeners. 

### Core Architectural Features:
* **Single-Producer / Multi-Consumer (SPMC) Pipeline**: A single dedicated producer thread reads audio frames from the active speaker (or a playback file) and writes them to a shared, lock-free ring buffer.
* **Write-Only Listener Hot Path**: Each listener runs a lightweight timer-driven loop that reads from their private cursor in the ring buffer and writes directly to their session. There is **zero shared mutation**, no per-listener decode or mix, and no listener-to-listener lock contention. 
* **$O(1)$ Scaling**: Adding listeners incurs only the minimal kernel cost of socket UDP transmission (`sendto`), allowing a single server to scale to tens of thousands of sessions.

---

## 2. Architecture & Lifecycle Invariants

```
               +--------------------------------------+
               |            ACTIVE SPEAKER            |
               | (Live SIP Session / Loopback / File) |
               +-------------------+------------------+
                                   |
                                   | L16 Audio
                                   v
               +--------------------------------------+
               |           PRODUCER THREAD            |
               |    Read, Timestamp & Energy Detect   |
               +-------------------+------------------+
                                   |
                                   v Write Frame
       +------------------------------------------------------+
       |       LOCK-FREE SINGLE-PRODUCER MULTI-CONSUMER       |
       |             CIRCULAR RING BUFFER (L16)               |
       |       [F0] [F1] [F2] [F3] [F4] ... [F_RingSize]       |
       +-------+-------------------+------------------+-------+
               |                   |                  |
   Read Cursor | [cs_1]            | [cs_2]           | [cs_N]
               v                   v                  v
       +---------------+   +---------------+   +---------------+
       |  LISTENER 1   |   |  LISTENER 2   |   |  LISTENER N   |
       | Write Leg (1) |   | Write Leg (2) |   | Write Leg (N) |
       +---------------+   +---------------+   +---------------+
```

* **Single Active Speaker**: Only one speaker can write to the broadcast room at any moment. If another speaker attempts to join, they are rejected (or barge/override if configured).
* **Speaker Promotion Invariant**: To prevent use-after-free and TOCTOU races, `current_speaker` is only ever published/registered by the speaker's **own session thread**. Foreign API/CLI threads merely raise a promotion signal (`MFLAG_ROLE_TRANSITION_TO_SPEAKER`) and clear `MFLAG_RUNNING` to cause the listener loop to exit and transition.
* **Housekeeping and Auto-Heal**: A background housekeeping thread runs periodically to prune inactive rooms, release references, and detect "wedged" listeners (consumers whose cursors have stalled for >5 seconds under network congestion), hanging them up with a `MEDIA_TIMEOUT` cause.
* **Lock Hierarchy**: To ensure deadlock-free operation across the module, all lock acquisitions must strictly follow this rank ordering:
  ```
  config_rwlock > control_mutex > speaker_lock > listener_lock > recording_mutex > audio_in_mutex
  ```

---

## 3. Installation & Building

`mod_broadcast` compiles cleanly under standard FreeSWITCH source trees.

### 3.1 Standard Compilation
From your FreeSWITCH source root directory:
```sh
make mod_broadcast
sudo make mod_broadcast-install
```

### 3.2 Optional Build Flags
* **Producer Tick Histogram**: Compile with histogram buckets enabled to track tick processing latency:
  ```sh
  make mod_broadcast mod_broadcast_la_CFLAGS="-DBROADCAST_ENABLE_HISTOGRAM \$(AM_CFLAGS)"
  ```
* **Strict Warning Checks**: Enable compiler warnings-as-errors for compliance checks:
  ```sh
  make mod_broadcast mod_broadcast_la_CFLAGS="-Wall -Wextra -Werror -Wno-unused-parameter \$(AM_CFLAGS)"
  ```

---

## 4. Configuration (`broadcast.conf.xml`)

The module is configured via `autoload_configs/broadcast.conf.xml`. 

```xml
<configuration name="broadcast.conf" description="mod_broadcast configuration">
  <settings>
    <param name="default-profile" value="default"/>
    <param name="max-broadcasts" value="500"/>
    <param name="event-ring-size" value="1024"/>
  </settings>

  <profiles>
    <profile name="default">
      <param name="rate" value="8000"/>
      <param name="interval" value="20"/>
      <param name="channels" value="1"/>
      <param name="ring-size" value="50"/>
      <param name="max-lag-frames" value="10"/>
      <param name="max-listeners" value="30000"/>
      <param name="timer-name" value="timerfd"/>
      <param name="energy-detection" value="true"/>
      <param name="silence-policy" value="zero"/>
      <param name="caller-controls" value="default"/>
    </profile>
  </profiles>

  <caller-controls>
    <group name="default">
      <control action="hangup" digits="0"/>
      <control action="leave" digits="9"/>
      <control action="mute-toggle" digits="*6"/>
    </group>
  </caller-controls>
</configuration>
```

### Profile Parameters Reference:
| Param | Default | Description |
|---|---|---|
| `rate` | `8000` | Audio sample rate (Hz). |
| `interval` | `20` | Audio packetization interval (ms). |
| `ring-size` | `50` | Number of audio frames retained in the circular ring. |
| `max-lag-frames` | `10` | Cursors exceeding this threshold trigger a snap-forward (`listener-resync`). |
| `max-listeners` | `2000` | The maximum sessions allowed to join a single broadcast room. |
| `silence-policy` | `zero` | Behaviour when no speaker is active: `zero` (digital silence), `cn` (comfort noise), or `moh:<file>` (Music on Hold). |
| `timer-name` | `timerfd` | System timer to pace the loop (recommend `timerfd` or `soft`). |
| `energy-detection` | `true` | Enables energy-based speaker talk/silence detection. |

---

## 5. Usage Guide

### 5.1 Dialplan Integration
Call the `broadcast` application with parameters formatted as `<room_name>+<param>=<value>+...`:

```xml
<!-- Join room "stressroom" as a listener -->
<extension name="join_listener">
  <condition field="destination_number" expression="^5001$">
    <action application="answer"/>
    <action application="broadcast" data="stressroom+role=listener+tag=my-listener-tag"/>
  </condition>
</extension>

<!-- Join room "stressroom" as the speaker -->
<extension name="join_speaker">
  <condition field="destination_number" expression="^5000$">
    <action application="answer"/>
    <action application="broadcast" data="stressroom+role=speaker"/>
  </condition>
</extension>
```

### 5.2 CLI / API Subcommands
Dispatched via `broadcast <room_name> <subcommand> [args]`:
* `info`: Displays room statistics, speaker details, and listener counts.
* `listeners`: Lists all active listeners with UUIDs and lag metrics.
* `stats`: Prints module-wide counters.
* `set_speaker <member_id>` / `promote_listener <member_id>`: Gracefully promotes a listener to the speaker role.
* `clear_speaker`: Unsets the active speaker, falling back to the silence policy.
* `record [path]` / `norecord`: Starts or stops recording the broadcast audio.
* `play <file>` / `stop_play`: Streams a WAV file directly to the SPMC ring buffer (takes precedence over live speaker).
* `destroy [grace_ms] [announcement_file]`: Destroys the room. If `grace_ms` is supplied, it plays the announcement file to listeners while blocking new joins before hanging up sessions.

---

## 6. Caller Controls (DTMF Actions)

Users can trigger actions dynamically by pressing DTMF digits. The following actions are supported:
* `hangup`: Hangs up the session.
* `leave`: Exits the room and returns the channel to the dialplan.
* `request-speaker`: Fires a custom request event onto the FS event bus.
* `mute-toggle`: Atomically toggles speaker muting (zeros out read frames).
* `vol-up` / `vol-down` / `vol-reset`: Adjusts read/write volume indexes.
* `execute-app`: Executes an arbitrary dialplan application (e.g., `playback`).
* `lua` / `js`: Runs an external script against the session (requires `mod_lua` / `mod_v8`).
* `transfer`: Transfers the call to another dialplan extension.
* `event`: Fires a custom `broadcast::dtmf-action` event on the bus.

---

## 7. Exported Variables & Custom Events

### 7.1 Channel Variables
The following variables are written to CDRs and hangup hooks:
* `broadcast_name`: Room name joined.
* `broadcast_role`: Active role (`speaker` / `listener`).
* `broadcast_final_role`: Role at teardown (tracks mid-call promotions).
* `broadcast_ended_by_self`: `true` if the call ended voluntarily (BYE or DTMF leave).
* `broadcast_kicked_by`: Logs initiator of termination (`admin`, `wedged`, or `destroy`).
* `broadcast_resync_count`: Number of times the listener fell behind and had to snap forward.

### 7.2 Custom Event Subclasses
Subscribe via `event plain CUSTOM broadcast::<subclass>`:
* `broadcast::create` / `broadcast::destroy`
* `broadcast::listener-join` / `broadcast::listener-leave`
* `broadcast::speaker-set` / `broadcast::speaker-clear`
* `broadcast::destroy-grace`: Triggered when entering grace periods.
* `broadcast::dtmf-action`: Fired when a DTMF-event action is matched.
* `broadcast::producer-stall`: Fired if the producer thread loop drifts.

---

## 8. High-Scale Benchmarks & Performance Tuning

`mod_broadcast` has been benchmarked across VM clusters and physical hardware. The results prove that the ceiling is **network interface / UDP throughput bound**, not CPU or application memory bound.

### 8.1 Benchmark Results

| Hardware Environment | Call Routing Path | Peak Active Listeners | Bottleneck & Limits |
|---|---|---|---|
| 16-Core / 30 GB VM | Single Link (eth0) | **~2,400** | Virtio vNIC TX queue saturation (~121,000 pps ceiling). |
| 16-Core / 30 GB VM | Split-Link (Public + Private NICs) | **~5,150** | Co-exhaustion of split queues (~257,000 pps total). |
| **Physical Server (32 Cores)** | **Direct Route** | **22,400** | **Physical NIC bandwidth / UDP packet congestion (560,000 pps).** |

### 8.2 System Performance at 22,400 Listeners
At peak load of **22,400 concurrent listeners** on a physical target server, the system metrics remained fully stable:
* **Missed Producer Ticks**: `0` (Zero missed frames).
* **Average Producer Latency**: `0 us` (Processing completed in less than 1 microsecond).
* **Core CPU SoftIRQ**: `< 0.5%` max utilization.
* **Network Throughput**: **560,000 UDP packets per second** egress. Network saturation at this density causes minor packet queue latency (manifesting as sluggish SSH/signaling), while the host's CPU and RAM still possess significant headroom (~40,000+ theoretical capability).

### 8.3 Kernel & Network OS Optimizations
To support high concurrency, apply the following tunings to `/etc/sysctl.d/99-rtp-tuning.conf`:

```ini
# Maximize socket read and write buffers
net.core.rmem_max = 134217728
net.core.wmem_max = 134217728

# Adjust UDP memory allocations
net.ipv4.udp_mem = 8388608 12582912 16777216

# Increase network interface backlogs
net.core.netdev_max_backlog = 300000
net.core.netdev_budget = 600
net.core.somaxconn = 4096

# Expand local ephemeral ports to avoid port exhaustion
net.ipv4.ip_local_port_range = 1024 65535
```

Additionally, maximize FreeSWITCH's internal handles in `/etc/freeswitch/autoload_configs/switch.conf.xml`:
```xml
<param name="max-sessions" value="30000"/>
<param name="sessions-per-second" value="3000"/>
```

Tune Sofia socket buffer parameters to prevent packet drops during registration bursts in `/etc/freeswitch/sip_profiles/external.xml`:
```xml
<param name="socket-send-buffer-size" value="16777216"/>
<param name="socket-recv-buffer-size" value="16777216"/>
```

---

## 9. Advanced Scenarios & Code Examples

A suite of advanced deployment scenarios, including Lua scripts and dialplan snippets, is located in the `examples/` directory:

### 9.1 Scenario A: One-to-Many Live Broadcast
* **Description**: Standard one-to-many setup where a speaker (configured with volume adjustment and mute keys) broadcasts to thousands of passive listeners.
* **Code Links**:
  * [scenario_one_to_many.xml](examples/scenario_one_to_many.xml) - Dialplan mapping speaker (7000) and listener (7001) entries.
  * [scenario_one_to_many.lua](examples/scenario_one_to_many.lua) - Script to automate loopback speaker origination and statistics monitoring.

### 9.2 Scenario B: Cascading Rooms (Multi-Server Scale)
* **Description**: Bypasses network card / UDP throughput caps of a single server by establishing one-way audio bridges (cascades) between rooms/servers. A listener leg from `primary_room` is bridged directly as the speaker leg of `secondary_room`.
* **Code Links**:
  * [scenario_cascaded.xml](examples/scenario_cascaded.xml) - Dialplan entry for the loopback cascade bridge.
  * [scenario_cascaded.lua](examples/scenario_cascaded.lua) - Automation script setting up loopback bridging between rooms.

### 9.3 Scenario C: Few-to-Many (Panelists Conference to Mass Broadcast)
* **Description**: Integrates standard `mod_conference` with `mod_broadcast`. A panel of 3-5 active hosts join a conference room (where they talk to and hear each other). The mixed conference stream is bridged as the single speaker leg to a `mod_broadcast` room, scaling to 10,000+ passive listeners at $O(1)$ cost.
* **Code Links**:
  * [scenario_few_to_many.xml](examples/scenario_few_to_many.xml) - Dialplan routing hosts to conference (7200), bridge (7201), and listeners to broadcast (7202).
  * [scenario_few_to_many.lua](examples/scenario_few_to_many.lua) - Lua bridge setup script.

### 9.4 Interactive Transitions (Role Switching)
* **Description**: Allows callers to switch roles interactively:
  * **Speaker to Listener**: Active speaker presses `9` (DTMF leave action), returning control to Lua. Lua queries the user and re-joins them as a listener.
  * **Listener to Speaker**: A listener presses `9` (DTMF leave action), returning control to Lua. Lua prompts for a PIN ("1234") and re-routes them into the broadcast as the active speaker.
* **Code Links**:
  * [interactive_roles.xml](examples/interactive_roles.xml) - Lobby dialplan routing.
  * [interactive_roles.lua](examples/interactive_roles.lua) - State machine Lua script managing transitions and PIN checks.

