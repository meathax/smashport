-- Development-only MAME 0.289 L8 timing witness. Never loaded by SmashTV.exe.
-- Keep tap handles global: MAME removes a tap when Lua collects its handle.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
print(string.format("L8_INIT width=%d height=%d refresh=%.9f pixel_period=%.12f scan_period=%.12f frame_period=%.12f", screen.width, screen.height, screen.refresh, screen.pixel_period, screen.scan_period, screen.frame_period))
local frames, dpywrites, hreads = 0, 0, 0
l8_timing_taps = {}
l8_timing_taps[1] = space:install_write_tap(0xc00000a0, 0xc00000af, "l8_dpyint", function(offset, data, mask)
    if dpywrites < 24 then
        print(string.format("L8_DPYINT write=%d frame=%d emutime=%.9f address=%08x data=%08x mask=%08x pc=%08x", dpywrites, screen:frame_number(), machine.time:as_double(), offset, data, mask, cpu.state["PC"].value))
    end
    dpywrites = dpywrites + 1
end)
l8_timing_taps[2] = space:install_read_tap(0xc00001c0, 0xc00001cf, "l8_hcount", function(offset, data, mask)
    if hreads < 24 then
        print(string.format("L8_HCOUNT read=%d frame=%d emutime=%.9f address=%08x data=%08x mask=%08x pc=%08x", hreads, screen:frame_number(), machine.time:as_double(), offset, data, mask, cpu.state["PC"].value))
    end
    hreads = hreads + 1
end)
emu.register_frame_done(function()
    if frames < 12 then
        print(string.format("L8_FRAME sample=%d frame=%d emutime=%.9f vblank_start_in=%.9f vblank_end_in=%.9f", frames, screen:frame_number(), machine.time:as_double(), screen:time_until_vblank_start():as_double(), screen:time_until_vblank_end():as_double()))
    end
    frames = frames + 1
end, "frame")
