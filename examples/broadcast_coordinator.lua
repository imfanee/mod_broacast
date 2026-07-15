--[[
  broadcast_coordinator.lua — ESL-oriented reference (~100 lines)
  Subscribes to broadcast::* CUSTOM events, tracks pending speaker requests,
  and exposes trivial grant/deny via stdin commands.

  Usage (conceptual):
    lua broadcast_coordinator.lua

  Requires: luasocket (or swap in inotify/tcp). This stub uses stdin for demo.
  Wire to mod_xml_curl / HTTP by replacing poll_stdin() with your server loop.

  Author reference: see mod_broadcast sources (Faisal Hanif).
]]

local pending = {} -- [broadcast_name] = { {member_id=, uuid=, ts=}, ... }

local function log(msg)
  io.stderr:write(os.date("!%Y-%m-%dT%H:%M:%SZ ") .. msg .. "\n")
end

local function parse_kv(body)
  local t = {}
  for line in string.gmatch(body or "", "[^\r\n]+") do
    local k, v = string.match(line, "^([^:]+):%s*(.*)$")
    if k then t[k] = v end
  end
  return t
end

local function on_event(ev)
  -- Expect ESL-style: Event-Subclass + core fields; adapt to your client.
  local sub = ev["Event-Subclass"] or ev["event-subclass"] or ""
  local name = ev["Broadcast-Name"] or ev["variable_broadcast_name"] or ev["broadcast_name"]
  local mid = tonumber(ev["Broadcast-Member-Id"] or ev["Member-ID"] or ev["member_id"] or "0")
  if sub == "broadcast::speaker-request" and name and mid and mid > 0 then
    pending[name] = pending[name] or {}
    table.insert(pending[name], { member_id = mid, ts = os.time() })
    log(string.format("REQUEST speaker m_%u on %s", mid, name))
  end
end

local function grant(name, member_id)
  -- In real ESL: api:executeString("uuid_broadcast ...") or bgapi broadcast ...
  log(string.format("GRANT (stub) broadcast %s set_speaker m_%u", name, member_id))
end

local function deny(name, member_id)
  log(string.format("DENY (stub) %s m_%u", name, member_id))
end

local function poll_stdin()
  io.stdout:write("commands: grant <name> <m_id> | deny <name> <m_id> | list | quit\n")
  while true do
    io.stdout:write("> ")
    io.flush(io.stdout)
    local line = io.read("*l")
    if not line then break end
    local cmd, a, b = string.match(line, "^(%S+)%s*(%S*)%s*(%S*)")
    if cmd == "quit" then break end
    if cmd == "list" then
      for n, lst in pairs(pending) do
        io.write(n .. ": ")
        for _, r in ipairs(lst) do io.write(string.format("m_%u ", r.member_id)) end
        io.write("\n")
      end
    elseif cmd == "grant" and a ~= "" and b ~= "" then
      grant(a, tonumber(string.match(b, "^m_(%d+)$") or b))
      pending[a] = {}
    elseif cmd == "deny" and a ~= "" and b ~= "" then
      deny(a, tonumber(string.match(b, "^m_(%d+)$") or b))
    end
  end
end

-- Demo: feed a fake event table (replace with ESL inbound_event in production)
on_event({
  ["Event-Subclass"] = "broadcast::speaker-request",
  ["Broadcast-Name"] = "lecture101",
  ["Broadcast-Member-Id"] = "7",
})
poll_stdin()
