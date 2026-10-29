-- SPDX-License-Identifier: CC0-1.0
-- Observe an unmodified full-drive MAME run driven by a native PET BASIC program.
local machine = manager.machine
local root = assert(os.getenv("ECONOPET_REFERENCE_OUTPUT"))
local model = assert(os.getenv("ECONOPET_REFERENCE_MODEL"))
local cpu = assert(machine.devices[":f3"])
local space = cpu.spaces["program"]
local marker = 0x7000
local phase = 0x701f
local complete = 255
local deadline = tonumber(os.getenv("ECONOPET_REFERENCE_DEADLINE") or "180")
-- Scratch's media writes include several million PET polling reads.
local event_limit = 6000000
local events = 0
local taps = {}
local finished = false
local started = false
local trace = assert(io.open(root .. "/controller-io.csv", "w"))
local phases = assert(io.open(root .. "/phases.csv", "w"))
trace:write("seconds,direction,address,value\n")
phases:write("seconds,phase\n")

-- Preserve input values and output writes while recording actual PET I/O accesses.
local function observe(direction, address, value)
    events = events + 1
    if events <= event_limit then
        trace:write(string.format("%.9f,%s,%04x,%02x\n",
            machine.time:as_double(), direction, address, value))
    end
end
taps[#taps + 1] = space:install_read_tap(0xe820, 0xe84f, "reference-read",
    function(address, value) observe("read", address, value) end)
taps[#taps + 1] = space:install_write_tap(0xe820, 0xe84f, "reference-write",
    function(address, value) observe("write", address, value) end)
taps[#taps + 1] = space:install_write_tap(phase, phase, "reference-phase",
    function(_, value)
        if started then
            phases:write(string.format("%.9f,%d\n", machine.time:as_double(), value))
        end
    end)
taps[#taps + 1] = space:install_write_tap(marker, marker, "reference-start",
    function(_, value)
        if value == 1 then
            assert(not started, "native controller restarted during observation")
            started = true
            phases:write(string.format("%.9f,0\n", machine.time:as_double()))
        end
    end)

-- Clear only the harness mailbox, leaving both original drive CPUs untouched.
for address = marker, phase do
    space:write_u8(address, 0)
end
local board_image = os.getenv("ECONOPET_REFERENCE_BOARD_IMAGE")
machine.natkeyboard:post("RUN\n" .. (board_image and board_image .. "\n" or ""))

-- Save bounded observation bytes without interpreting the drive's results.
local function save_bytes(path, source, address, count)
    local file = assert(io.open(root .. "/" .. path, "wb"))
    for offset = 0, count - 1 do
        file:write(string.char(source:read_u8(address + offset)))
    end
    file:close()
end

-- Retain completion, device identity and active ROMs before normal image flush/exit.
emu.register_frame_done(function()
    assert(#taps == 4, "reference observation taps must remain alive")
    if finished or (space:read_u8(marker) ~= complete and machine.time:as_double() < deadline) then
        return
    end
    finished = true
    trace:close()
    phases:close()
    save_bytes("capture.bin", space, 0x4000, 0x1c00)
    save_bytes("mailbox.bin", space, marker, 32)
    local screen = assert(io.open(root .. "/screen.txt", "w"))
    for row = 0, 24 do
        local line = ""
        for column = 0, 39 do
            local value = space:read_u8(0x8000 + row * 40 + column) & 0x7f
            if value < 0x20 then
                value = value + 0x40
            elseif value >= 0x60 then
                value = value - 0x20
            end
            line = line .. string.char(value)
        end
        screen:write(line .. "\n")
    end
    screen:close()
    local drive = ":ieee8:c" .. model
    local main = assert(machine.devices[drive .. ":un1"])
    local controller = assert(machine.devices[drive .. ":uh3"])
    local start = model == "4040" and 0xd000 or 0xc000
    save_bytes("active-main.bin", main.spaces["program"], start, 0x10000 - start)
    save_bytes("active-controller.bin", controller.spaces["program"], 0xfc00, 0x400)
    local result = assert(io.open(root .. "/machine.txt", "w"))
    result:write(string.format("seconds=%.9f\nmarker=%d\nio_events=%d\ntrace_complete=%s\n",
        machine.time:as_double(), space:read_u8(marker), events,
        tostring(events <= event_limit)))
    for _, device in ipairs({cpu, main, controller}) do
        result:write(string.format("cpu=%s pc=%04x\n", device.tag, device.state["PC"].value))
    end
    result:close()
    machine:exit()
end)
