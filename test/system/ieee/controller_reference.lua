-- SPDX-License-Identifier: CC0-1.0
-- Submit main-CPU-visible jobs to MAME's unmodified 6504 ROM and GCR electronics.
local machine = manager.machine
local cpu = assert(machine.devices[":ieee8:c4040:un1"])
local space = cpu.spaces["program"]
local root = assert(os.getenv("ECONOPET_CONTROLLER_REFERENCE"))
local input = assert(io.open(root .. "/requests.txt", "r"))
local output = assert(io.open(root .. "/observations.txt", "w"))
local fdc = assert(machine.devices[":ieee8:c4040:uh3"])
local initialized, completed = false, false
local pending, started

-- Map physical shared RAM offsets into the main CPU's four decoded blocks.
local function address(offset)
    return (math.floor(offset / 1024) + 1) * 4096 + offset % 1024
end

-- Install exactly the requested bytes, including independent unused-header sentinels.
local function write_bytes(offset, hex)
    for i = 1, #hex, 2 do
        space:write_u8(address(offset + (i - 1) / 2), tonumber(hex:sub(i, i + 1), 16))
    end
end

-- Preserve every byte rather than projecting the result down to the completion code.
local function read_bytes(offset, size)
    local bytes = {}
    for i = 0, size - 1 do
        bytes[#bytes + 1] = string.format("%02x", space:read_u8(address(offset + i)))
    end
    return table.concat(bytes)
end

-- Poll completion only. Reference timing is neither reproduced nor compared by production.
emu.register_frame_done(function()
    if completed then return end
    local now = machine.time:as_double()
    if now < 2 then return end
    if not initialized then
        assert(space:read_u8(0x1000) == 15, "native controller did not boot")
        -- Reject a different active main/controller ROM even if MAME could boot it.
        local parts = {"901468-14.bin", "901468-15.bin", "901468-16.bin"}
        for part, name in ipairs(parts) do
            local file = assert(io.open(root .. "/" .. name, "rb"))
            local bytes = file:read("*a")
            file:close()
            for i = 1, #bytes do
                assert(space:read_u8(0x5000 + (part - 1) * 4096 + i - 1) == bytes:byte(i),
                    "active main ROM identity mismatch")
            end
        end
        local file = assert(io.open(root .. "/roms/c4040/901466-04.uk3", "rb"))
        local bytes = file:read("*a")
        file:close()
        for i = 1, #bytes do
            assert(fdc.spaces["program"]:read_u8(0x1c00 + i - 1) == bytes:byte(i),
                "active controller ROM identity mismatch")
        end
        -- Park only the DOS CPU after its genuine startup, leaving the 6504 running.
        space:write_u8(0x60, 0x78)
        space:write_u8(0x61, 0x4c)
        space:write_u8(0x62, 0x61)
        space:write_u8(0x63, 0)
        cpu.state["PC"].value = 0x60
        for i = 0,14 do space:write_u8(0x1003 + i, 0) end
        initialized = true
        return
    end
    if pending then
        local result = space:read_u8(0x1003 + pending.slot)
        if result >= 128 then
            assert(now - started < 90, "native controller job timed out: " .. pending.name)
            return
        end
        output:write(string.format("%s %d %d %s %s\n", pending.name, pending.slot, result,
            read_bytes(0x21 + pending.slot * 8, 8),
            read_bytes((pending.slot + 1) * 256, 256)))
        output:flush()
        pending = nil
    end
    local line = input:read("*l")
    if not line then
        output:write("DONE\n")
        output:close()
        input:close()
        completed = true
        machine:exit()
        return
    end
    local name, slot, command, header, buffer =
        line:match("^(%S+) (%d+) (%d+) (%x+) (%x+)$")
    assert(name and #header == 16 and #buffer == 512, "invalid controller request")
    slot, command = tonumber(slot), tonumber(command)
    assert(slot < 15 and command >= 128 and command <= 255)
    write_bytes(0x21 + slot * 8, header)
    write_bytes((slot + 1) * 256, buffer)
    if command & 0xfe == 0xe0 then
        for i = 0,767 do
            space:write_u8(address(0x100 + i), space:read_u8(0x5000 + i))
        end
    end
    space:write_u8(0x1003 + slot, command)
    pending = {name = name, slot = slot}
    started = now
end)
