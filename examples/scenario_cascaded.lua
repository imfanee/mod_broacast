--[[
  scenario_cascaded.lua

  Demonstrates cascading mod_broadcast rooms/hosts for multi-room/multi-server scaling.
  
  Concept:
  - We originate a loopback channel:
    - One side of the loopback joins 'primary_room' as a LISTENER.
    - The other side joins 'secondary_room' as the SPEAKER.
  - Any audio sent to the primary room is fanned out to the loopback listener,
    which instantly feeds it into the secondary room.
  - This allows cascading traffic to different rooms or even remote FreeSWITCH servers
    (via SIP profile bridges) to bypass single-server network interface limitations.
]]

api = freeswitch.API()

local primary_room = "primary_room"
local secondary_room = "secondary_room"

-- 1. Setup the cascade bridge.
-- We originate loopback/7100 (which will execute extension 7100 and join the primary room as a listener)
-- and bridge it directly to join the secondary room as a speaker.
local bridge_uuid = api:executeString("create_uuid")
local cascade_cmd = string.format(
    "bgapi originate {origination_uuid=%s}loopback/7100/default &broadcast(%s+role=speaker)",
    bridge_uuid,
    secondary_room
)

freeswitch.consoleLog("info", "[cascade] Initializing cascade link. Bridge UUID: " .. bridge_uuid .. "\n")
freeswitch.consoleLog("info", "[cascade] Loopback A side: listener in '" .. primary_room .. "'\n")
freeswitch.consoleLog("info", "[cascade] Loopback B side: speaker in '" .. secondary_room .. "'\n")

api:executeString(cascade_cmd)

-- Sleep to let the bridge establish
freeswitch.msleep(2000)

-- Check room configurations to verify cascade
local primary_info = api:executeString("broadcast " .. primary_room .. " info")
local secondary_info = api:executeString("broadcast " .. secondary_room .. " info")

freeswitch.consoleLog("info", "[cascade] Primary Room Listeners: " .. (string.match(primary_info, "Listeners:%s*(%d+)") or "0") .. "\n")
freeswitch.consoleLog("info", "[cascade] Secondary Room Uptime: " .. (string.match(secondary_info, "uptime%s*([%d:]+)") or "unknown") .. "\n")
