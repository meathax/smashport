-- Development-only: log the exact emulated time of every HCOUNT read in a frame
-- window (STV_HT_FROM..STV_HT_TO), plus the default coin/start schedule.
local machine = manager.machine
local screen = machine.screens[":screen"]
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local from = tonumber(os.getenv("STV_HT_FROM") or "0")
local to = tonumber(os.getenv("STV_HT_TO") or "0")
local port = machine.ioport.ports[":IN1"]
local n = 0
t1 = space:install_read_tap(0xc00001c0, 0xc00001cf, "ht", function(o, d, m)
    local f = screen:frame_number()
    if f >= from and f <= to then
        local t = machine.time
        print(string.format("HT %d %d %d.%018d %04x pc=%08x", n, f, t.seconds, t.attoseconds, d & 0xffff, cpu.state["PC"].value))
    end
    n = n + 1
    return d
end)
emu.register_frame_done(function()
    local f = screen:frame_number()
    if f == 2600 then port:field(1):set_value(1) elseif f == 2604 then port:field(1):clear_value()
    elseif f == 3150 then port:field(4):set_value(1) elseif f == 3170 then port:field(4):clear_value() end
    if f > to then machine:exit() end
end)
