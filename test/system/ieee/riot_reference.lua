-- SPDX-License-Identifier: CC0-1.0
-- Record MAME's independent 6532 observations, without loading a Commodore ROM.
local machine = manager.machine
local space = assert(machine.devices[":maincpu"]).spaces["program"]
local root = assert(os.getenv("ECONOPET_RIOT_REFERENCE"))
local input = assert(io.open(root .. "/stimulus.txt", "r"))
local output = assert(io.open(root .. "/observations.txt", "w"))
local resume_at = 0
local completed = false

-- Process a transaction batch at one scheduler boundary, stopping only at explicit waits.
emu.register_frame_done(function()
    if completed or machine.time:as_double() < resume_at then return end
    for line in input:lines() do
        local operation, reg, value = line:match("(%u+)%s*(%d*)%s*(%d*)")
        if operation == "W" then
            space:write_u8(0x0280 + tonumber(reg), tonumber(value))
            output:write(line .. "\n")
        elseif operation == "R" then
            local actual = space:read_u8(0x0280 + tonumber(reg))
            output:write(string.format("R %d %d %d\n",
                tonumber(reg), actual, tonumber(value)))
        elseif operation == "WAIT" then
            output:write("WAIT\n")
            resume_at = machine.time:as_double() + 1
            return
        else
            error("invalid independent RIOT stimulus: " .. line)
        end
    end
    output:write("DONE\n")
    output:close()
    input:close()
    completed = true
    machine:exit()
end)
