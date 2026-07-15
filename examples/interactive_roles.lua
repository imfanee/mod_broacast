--[[
  interactive_roles.lua

  Demonstrates dynamic speaker <-> listener transitions.
  Uses DTMF caller controls mapped to "leave" (e.g., digit '9').
  
  Workflow:
  1. User joins the script.
  2. If joining as speaker:
     - User is routed into mod_broadcast as speaker.
     - If the speaker presses '9' (leave), they exit the broadcast.
     - The script gains control back, plays a prompt asking if they want to continue as a listener.
     - If they press '1', they are re-routed into the broadcast as a listener.
  3. If joining as listener:
     - User is routed into mod_broadcast as a listener.
     - If they press '9' (leave), they exit the broadcast.
     - The script collects a PIN code. If they enter "1234", they are promoted to speaker.
     - If PIN fails, they can press '1' to return as a listener or hang up.
]]

local room = "interactive_room"
local profile = "default"

-- Ensure session exists (run from dialplan)
if not session then
    return
end

session:answer()
session:setAutoHangup(false)

-- Determine initial role (default to listener)
local current_role = session:getVariable("start_role") or "listener"

session:consoleLog("info", "[interactive_roles] Session started. Initial role: " .. current_role .. "\n")

while session:ready() do
    if current_role == "speaker" then
        session:consoleLog("info", "[interactive_roles] Entering room '" .. room .. "' as SPEAKER\n")
        
        -- Execute mod_broadcast. Digit '9' must be configured in caller-controls with the 'leave' action.
        session:execute("broadcast", room .. "@" .. profile .. "+role=speaker")
        
        if not session:ready() then break end
        
        session:consoleLog("info", "[interactive_roles] Speaker returned to Lua script. Querying next role...\n")
        session:execute("sleep", "1000")
        
        -- Ask if they want to rejoin as a listener
        -- Plays: "To listen to the conference, press 1..."
        session:execute("play_and_get_digits", "1 1 3 5000 # ivr/ivr-to_listen_to_the_conference_press_1.wav silence_stream://250 choice \\d+")
        
        local choice = session:getVariable("choice")
        if choice == "1" then
            current_role = "listener"
        else
            session:execute("playback", "ivr/ivr-thank_you.wav")
            session:hangup()
            break
        end
        
    else -- listener
        session:consoleLog("info", "[interactive_roles] Entering room '" .. room .. "' as LISTENER\n")
        
        -- Execute mod_broadcast. Digit '9' must be configured in caller-controls with the 'leave' action.
        session:execute("broadcast", room .. "@" .. profile .. "+role=listener")
        
        if not session:ready() then break end
        
        session:consoleLog("info", "[interactive_roles] Listener returned to Lua script. Querying speaker promotion...\n")
        session:execute("sleep", "1000")
        
        -- Prompt to enter speaker PIN number
        session:execute("play_and_get_digits", "4 4 3 5000 # ivr/ivr-enter_pin_number.wav ivr/ivr-you_have_entered_an_invalid_pin.wav pin_entered \\d+")
        
        local pin = session:getVariable("pin_entered")
        if pin == "1234" then
            session:consoleLog("info", "[interactive_roles] Correct PIN. Promoting listener to speaker.\n")
            session:execute("playback", "ivr/ivr-thank_you.wav")
            current_role = "speaker"
        else
            session:consoleLog("info", "[interactive_roles] Invalid PIN. Re-routing as listener.\n")
            session:execute("playback", "ivr/ivr-you_have_entered_an_invalid_pin.wav")
            
            -- Ask if they want to return as listener
            session:execute("play_and_get_digits", "1 1 3 5000 # ivr/ivr-to_listen_to_the_conference_press_1.wav silence_stream://250 choice \\d+")
            local choice = session:getVariable("choice")
            if choice == "1" then
                current_role = "listener"
            else
                session:execute("playback", "ivr/ivr-thank_you.wav")
                session:hangup()
                break
            end
        end
    end
end
