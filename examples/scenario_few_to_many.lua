--[[
  scenario_few_to_many.lua

  Orchestrates the Conference-to-Broadcast bridge (Few-to-Many).
  
  Concept:
  - We have a small panel of speakers (e.g., 5 hosts) who need to interact.
    They join a standard FreeSWITCH conference ('speakers_conf') where they can speak and hear each other.
  - We have a mass audience (e.g., 10,000 listeners) who only need to listen.
    They join 'massive_room' via mod_broadcast.
  - This script bridges them by originating a loopback channel:
    - Side A joins the conference (muted, so it doesn't feed loopback noise back).
    - Side B joins the broadcast room as the active SPEAKER.
  - Thus, the mixed output of the conference is fanned out to thousands of listeners
    at $O(1)$ CPU scaling cost.
]]

api = freeswitch.API()

local conf_name = "speakers_conf"
local broadcast_room = "massive_room"

-- 1. Setup the Conference-to-Broadcast bridge.
-- We originate loopback/7201 (which joins the conference)
-- and bridge it to join the broadcast room as the speaker.
local bridge_uuid = api:executeString("create_uuid")
local bridge_cmd = string.format(
    "bgapi originate {origination_uuid=%s}loopback/7201/default &broadcast(%s+role=speaker)",
    bridge_uuid,
    broadcast_room
)

freeswitch.consoleLog("info", "[few_to_many] Bridging conference '" .. conf_name .. "' to broadcast '" .. broadcast_room .. "'...\n")
freeswitch.consoleLog("info", "[few_to_many] Bridge Loopback UUID: " .. bridge_uuid .. "\n")

api:executeString(bridge_cmd)

-- Sleep to let the bridge establish
freeswitch.msleep(2000)

-- Check and log configuration
local bcast_info = api:executeString("broadcast " .. broadcast_room .. " info")
freeswitch.consoleLog("info", "[few_to_many] Broadcast Active Speaker UUID: " .. (string.match(bcast_info, "Speaker:.*uuid=([%w-]+)") or "none") .. "\n")
