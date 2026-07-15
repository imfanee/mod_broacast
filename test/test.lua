local f = io.open("/tmp/broadcast_lua_test.log", "a")
if f then
    f:write("Lua DTMF binding executed successfully at " .. os.time() .. "\n")
    f:close()
end
