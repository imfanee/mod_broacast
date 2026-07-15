--[[
  scenario_one_to_many.lua

  Demonstrates automated orchestration of a standard One-to-Many broadcast.
  Uses the FreeSWITCH API to originate a speaker leg (e.g. playing Music on Hold)
  and monitors the broadcast listener counts.
]]

local room_name = "one_to_many_room"
local moh_file = "local_stream://default"

api = freeswitch.API()

-- 1. originate loopback speaker playing MOH into the broadcast room
local speaker_uuid = api:executeString("create_uuid")
local originate_cmd = string.format(
    "bgapi originate {origination_uuid=%s}loopback/9999/default &broadcast(%s+role=speaker)",
    speaker_uuid,
    room_name
)

freeswitch.consoleLog("info", "[one_to_many] Launching speaker loopback... UUID: " .. speaker_uuid .. "\n")
api:executeString(originate_cmd)

-- Sleep to let the speaker join
freeswitch.msleep(1500)

-- 2. Monitor room stats periodically
for i = 1, 10 do
    freeswitch.msleep(2000)
    
    local info = api:executeString("broadcast " .. room_name .. " info")
    local listeners = string.match(info, "Listeners:%s*(%d+)") or "0"
    local uptime = string.match(info, "uptime%s*([%d:]+)") or "unknown"
    local avg_tick = string.match(info, "avg tick:%s*(%d+)%s*us") or "0"
    
    freeswitch.consoleLog("info", string.format(
        "[one_to_many] Room: %s | Active Listeners: %s | Uptime: %s | Avg Tick: %s us\n",
        room_name, listeners, uptime, avg_tick
    ))
end

-- 3. Gracefully tear down room
freeswitch.consoleLog("info", "[one_to_many] Automated demo complete. Tearing down room...\n")
api:executeString("broadcast " .. room_name .. " destroy 2000")
