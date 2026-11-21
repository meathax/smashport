-- Development-only per-command sound goldens (PLAN P6). Run with -wavwrite.
-- Holds the game's SOUNDSUP flag at 0 (the game's own "no sounds" switch, so
-- attract music stays silent) and drives each sound command through the main
-- CPU's sound latch exactly as the game does: 0xFD00|cmd (CB1 low: NMI), then
-- 0xFF00|cmd on the next frame. stv_l8_lockstep --snd-commands does the same.
--
-- Environment: STV_CMD_START (first command frame, default 600), STV_CMD_PERIOD
-- (frames per command, default 150), STV_CMD_FROM / STV_CMD_TO (default 0..255)
local machine = manager.machine
local screen = machine.screens[":screen"]
local space = machine.devices[":maincpu"].spaces["program"]
local SOUNDSUP, LATCH = 0x010865F0, 0x01E00000
local start = tonumber(os.getenv("STV_CMD_START") or "600")
local period = tonumber(os.getenv("STV_CMD_PERIOD") or "150")
local first = tonumber(os.getenv("STV_CMD_FROM") or "0")
local last = tonumber(os.getenv("STV_CMD_TO") or "255")
local finish = start + (last - first + 1) * period

emu.register_frame_done(function()
    local f = screen:frame_number()
    if f >= start - 60 then space:write_u16(SOUNDSUP, 0) end
    if f >= start and f < finish then
        local k = (f - start) // period
        local ph = (f - start) % period
        if ph == 0 then space:write_u16(LATCH, 0xFD00 | (first + k)) end
        if ph == 1 then space:write_u16(LATCH, 0xFF00 | (first + k)) end
    end
    if f >= finish then
        print(string.format("STV_CMD_DONE frames=%d", f))
        machine:exit()
    end
end, "stv_snd_commands")
