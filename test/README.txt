mod_broadcast — manual and SIPp test notes
==========================================

Prerequisites
-------------
- FreeSWITCH built/installed with mod_broadcast loaded.
- conf/autoload_configs/broadcast.conf.xml present; reloadxml if needed.
- Dialplan extensions that answer and execute the broadcast application
  (see examples below).
- SIPp 3.x (the `sipp` binary). On Debian/Ubuntu: `apt-get install -y sipp`
  (or `apt install sipp`). If `sipp` is not in PATH, set `SIPP_BIN=/full/path/to/sipp`
  before running `run_sipp_examples.sh`.

Dialplan examples (adjust domain, profiles, room name "loadtest")
-------------------------------------------------------------------
Speaker (destination matches -s value you pass to SIPp, e.g. 888801):

  <extension name="bcast_speaker_sipp">
    <condition field="destination_number" expression="^888801$">
      <action application="answer"/>
      <action application="broadcast" data="loadtest@default+role=speaker"/>
    </condition>
  </extension>

Listener (e.g. 888802):

  <extension name="bcast_listener_sipp">
    <condition field="destination_number" expression="^888802$">
      <action application="answer"/>
      <action application="broadcast" data="loadtest@default+role=listener"/>
    </condition>
  </extension>

Smoke (one speaker, one listener)
---------------------------------
Terminal 1 (from this directory):
  sipp -sf sipp/bcast_speaker.xml -m 1 -r 1 -l 1 -s 888801 127.0.0.1:5060

Terminal 2:
  sipp -sf sipp/bcast_listener.xml -m 1 -r 1 -l 1 -s 888802 127.0.0.1:5060

Replace 127.0.0.1:5060 with your Sofia SIP profile bind:port.

Observe in fs_cli:
  broadcast list
  broadcast loadtest info
  broadcast loadtest stats

Load (many listeners)
---------------------
Start speaker first, then ramp listeners (example 200 calls, 10/sec):

  sipp -sf sipp/bcast_speaker.xml -m 1 -r 1 -l 1 -s 888801 127.0.0.1:5060 &
  sipp -sf sipp/bcast_listener.xml -m 200 -r 10 -l 200 -s 888802 127.0.0.1:5060

Short call / CI-style (edit <pause milliseconds="..."/> in XML or use SIPp -d)
-------------------------------------------------------------------------------
For a few-second run, lower the pause in both XML files or run SIPp with a
short overall duration if your SIPp build supports global timers; otherwise
use a copy of the scenario with pause milliseconds="5000".

API checks
----------
  fs_cli -x "module_exists mod_broadcast"
  fs_cli -x "broadcast version"
  fs_cli -x "broadcast list"
  fs_cli -x "broadcast --json list"

See mod_broadcast_spec.md sections 13, 26, and 21.7 for methodology.
