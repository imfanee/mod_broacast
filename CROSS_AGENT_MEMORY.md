# Cross-Agent Memory: `mod_broadcast` Project

This file serves as a persistent cross-agent context and project memory document for any AI coder working on `mod_broadcast` in `/usr/src/freeswitch/`.

---

## 1. Project Context & Design Intent
* **Module**: `mod_broadcast` (FreeSWITCH 1.10.13-dev)
* **Author**: Faisal Hanif
* **Purpose**: High-scale lock-free audio broadcasting (1-to-many fan-out). A single speaker leg broadcasts audio frames to thousands of listener legs in real-time.
* **Core Scalability Invariant**: The listener steady-state hot path is **WRITE-ONLY** and lock-free — O(1) per listener, with no per-listener decode or mix. This avoids the O(N²) scaling cap of `mod_conference`. Do **NOT** add per-listener read/decode/mix in steady state or per-listener shared locks.

---

## 2. 8 Concurrency Crashes Found & Fixed
 Surfaced during aggressive soak/chaos testing under load and resolved in 2026-07:

1. **Shutdown Runtime Race**: Housekeeping accessed destroyed locks during shutdown. *Fix*: Added thread tracking and exit waits.
2. **`b_api_info` UAF**: Introspection formatted pointers from speaker pools outside locks. *Fix*: Copy fields to local buffers under lock.
3. **`b_api_listener_detail` UAF**: Dereferenced member pointers concurrently leaving. *Fix*: Session-pin the member.
4. **Producer-via-finder UAF**: Resolved session pointers without active read locks. *Fix*: Hold `switch_core_session_read_lock` across the op.
5. **Demotion dangling pointer**: Rereplaced speaker pointer was left pointing to a freed member. *Fix*: Publish speaker only from owner thread; clear-on-replacement under lock.
6. **Speaker input-read UAF**: Speaker thread read from a codec/session undergoing teardown. *Fix*: Read-lock session during input loop.
7. **Speaker-promotion TOCTOU**: Foreign API thread set `current_speaker = m` directly, racing session bails. *Fix*: **Invariant — only the member's own thread publishes itself as speaker.**
8. **Emitter shutdown race**: Emitter run-loop accessed freed hash-ring memory. *Fix*: Join emitter thread prior to hash destroy.

---

## 3. Benchmark History & Network Ceilings

### 3.1 16-Core Hetzner VM (July 2026)
* **Single Link (eth0)**: Capped at **~2,400 listeners** (~121,000 pps). Saturated the single virtio TX queue.
* **Two Links (eth0 + enp7s0)**: Split public and private traffic. Hit physical NIC TX caps at **~5,150 delivered listeners** (~257,000 pps). 

### 3.2 Physical Target Server Stress Test (July 11, 2026)
* **Target Hardware**: Physical Server (`37.27.141.216`), 32 Cores, 128 GB RAM.
* **Orchestration**: Load driven from three generators: LOAD1 (`5.161.120.1`, 16 vCPUs), LOAD2 (`5.161.240.94`, 16 vCPUs), and DEV (`5.161.241.68`, 3 vCPUs).
* **Max Concurrency**: Successfully achieved and held **22,400 active listener legs** (11,200 from LOAD1 and 11,200 from LOAD2).
* **Performance**: Missed ticks = 0, average producer latency = 0 us, SoftIRQ CPU under 0.5%.
* **Ceiling**: Saturated physical NIC processing bandwidth, delivering **560,000 UDP packets per second** (PCMU payload). Network latency spikes on SSH and observer legs identified the network layer as the absolute bottleneck, while target CPU/RAM still had significant headroom (~40,000+ theoretical capacity).

---

## 4. Environment & OS Tuning Summary
* **FreeSWITCH Limits**: Core `max-sessions` set to `30000`, `sessions-per-second` to `3000` in `switch.conf.xml`.
* **Sofia Profiles**: Buffer params added to `sip_profiles/external.xml`:
  ```xml
  <param name="socket-send-buffer-size" value="16777216"/>
  <param name="socket-recv-buffer-size" value="16777216"/>
  ```
* **Kernel Sysctl**: `net.core.rmem_max`/`wmem_max` set to 128 MB. Ephemeral port range expanded to `1024 65535`.

---

## 5. Advanced Application Scenarios & Transition Lobbing (July 15, 2026)
Added a suite of functional and structural reference integrations under `/examples/`:
* **Scenario A (One-to-Many)**: Basic deployment dialplan/lua orchestration for single-speaker scaling.
* **Scenario B (Cascaded Rooms)**: Loopback bridging between multiple local/remote rooms to scale past single-interface NIC PPS limits.
* **Scenario C (Few-to-Many)**: Mixed panelist conference fanned out to a mass broadcast audience at $O(1)$ scaling cost.
* **Interactive Roles**: DTMF-driven (`digits="9"` mapped to `leave`) role transitions:
  - Speaker exits → Lua intercepts → re-enters as Listener.
  - Listener exits → Lua intercepts → PIN prompt ("1234") → re-enters as Speaker.

