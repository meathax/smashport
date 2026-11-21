-- Development-only scenario bot for tools/lockstep/record.lua (STV_LS_BOT), PLAN P5.
-- Plays player 1 with legal inputs only (coin, start, 8-way move and fire),
-- deciding from RAM each frame. record.lua logs every input change so the
-- native build can replay the identical session (stv_l8_lockstep --inputs).
--
-- L8 RAM (bit addresses; resolved from the translated code's source references):
--   GAMSTATE 0x010865A0 (16)  P1DATA 0x01086220  WAVE 0x010EB630 (16)  OBJLST 0x01000040 (32)
-- Object block (DISP.EQU): OLINK +0x00 (32), OXPOS +0x70 (16), OYPOS +0x90 (16), OID +0x150 (16)
-- OID: class 0x8000 enemy, 0x4000 player side; type in bits 8..12 (0x01 player, 0x0C pickup)
--
-- Environment: STV_BOT_SEED (varies door choice), STV_BOT_COINS (coins kept in reserve, default 2)
local GAMSTATE, OBJLST, WAVE = 0x010865A0, 0x01000040, 0x010EB630
-- Object positions are world coordinates; room 1's arena spans about x 505..722,
-- y 378..537 (centre 614,457). Each room is offset in the world, so the centre is
-- re-derived when a new wave starts, from where the player enters (the door
-- opposite the one taken).
local CX, CY = 614, 457
local HALF_W, HALF_H = 109, 80
local DOOR_OFS = {{0, -177}, {206, 0}, {0, 183}, {-214, 0}}   -- top, right, bottom, left (past the walls)
local last_wave, last_door, need_center = -1, 2, false
local probe = nil
local INGAME = 0x10
local seed = tonumber(os.getenv("STV_BOT_SEED") or "1")

local held = {}          -- ":IN0"/":IN1" mask -> 0/1 currently applied
local function want(out, tag, mask, v)
    local key = tag .. mask
    if (held[key] or 0) ~= v then
        held[key] = v
        table.insert(out, {tag = tag, mask = mask, value = v})
    end
end

local function s16(v) if v >= 0x8000 then return v - 0x10000 end return v end

local function objects(space)
    local list, a, n = {}, space:read_u32(OBJLST), 0
    while a ~= 0 and n < 600 do
        local oid = space:read_u16(a + 0x150)
        list[#list + 1] = {x = s16(space:read_u16(a + 0x70)), y = s16(space:read_u16(a + 0x90)), oid = oid}
        a = space:read_u32(a)
        n = n + 1
    end
    return list
end

-- 8-way bits (bit0 up, bit1 down, bit2 left, bit3 right) pointing from (x,y) to (tx,ty)
local function dir_bits(dx, dy, dead)
    local b = 0
    local ax, ay = math.abs(dx), math.abs(dy)
    if dy < -dead and ay * 2 >= ax then b = b | 1 end
    if dy > dead and ay * 2 >= ax then b = b | 2 end
    if dx < -dead and ax * 2 >= ay then b = b | 4 end
    if dx > dead and ax * 2 >= ay then b = b | 8 end
    return b
end

local quiet, exit_try, frame_in_state, last_state = 0, 0, 0, -1
local dbg_px, dbg_py = -1, -1
local credit_phase = 0

return function(f, space, machine)
    local out = {}
    local gs = space:read_u16(GAMSTATE)
    if gs ~= last_state then frame_in_state, last_state = 0, gs else frame_in_state = frame_in_state + 1 end

    -- coins and (re)starts: whenever no game is running, and periodically in play so a
    -- dead player continues. Pulses: coin 6 frames, start 20 frames later for 10 frames.
    credit_phase = (credit_phase + 1) % 240
    local coin = credit_phase < 6 and 1 or 0
    local start = (credit_phase >= 26 and credit_phase < 36) and 1 or 0
    if gs == INGAME and frame_in_state < 600 then coin, start = 0, 0 end
    want(out, ":IN1", 0x0001, coin)
    want(out, ":IN1", 0x0004, start)

    local mv, fire = 0, 0
    if gs == INGAME then
        local objs = objects(space)
        local px, py
        for _, o in ipairs(objs) do
            if (o.oid & 0xC000) == 0x4000 and ((o.oid >> 8) & 0x1F) == 0x01 then px, py = o.x, o.y break end
        end
        local wave = space:read_u16(WAVE)
        if wave ~= last_wave then
            if last_wave >= 0 then need_center = true end
            last_wave = wave
        end
        if px and need_center then
            need_center = false
            local ex = {{0, -HALF_H}, {HALF_W, 0}, {0, HALF_H}, {-HALF_W, 0}}   -- entry door is opposite
            CX, CY = px + ex[last_door][1], py + ex[last_door][2]
            probe = nil
            -- entering through the top/left means the exit taken was bottom/right, and so on
            quiet, exit_try = 0, 0
        end
        if px then
            dbg_px, dbg_py = px, py
            local best, bd, pick, junk, jd = nil, 1e9, nil, nil, 1e9
            local threat = 0
            for _, o in ipairs(objs) do
                local dx, dy = o.x - px, o.y - py
                local d = dx * dx + dy * dy
                if (o.oid & 0x8000) ~= 0 then
                    if d < bd then best, bd = o, d end
                    if d < 60 * 60 then threat = threat + 1 end
                elseif (o.oid & 0xC000) == 0 and ((o.oid >> 8) & 0x1F) == 0x17 and d < jd then
                    junk, jd = o, d                                     -- leftover mine: shoot it
                elseif ((o.oid >> 8) & 0x1F) == 0x0C and (not pick or d < pick.d) then
                    pick = {x = o.x, y = o.y, d = d}
                end
            end
            if best then
                quiet = 0
                fire = dir_bits(best.x - px, best.y - py, 3)
                if bd < 70 * 70 then
                    mv = dir_bits(px - best.x, py - best.y, 0)          -- back away
                elseif pick and pick.d < 120 * 120 and threat == 0 then
                    mv = dir_bits(pick.x - px, pick.y - py, 4)          -- grab a nearby prize
                else
                    mv = dir_bits(CX - px, CY - py, 24)                 -- keep central
                end
                -- sidestep walls: drift along the edge the enemy is not on
                if mv == 0 then mv = ((f // 30) % 2 == 0) and 4 or 8 end
            else
                -- room clear: measure the arena by walking into each wall, then line up
                -- on its centre and push through each door in turn until the wave changes
                quiet = quiet + 1
                if junk then fire = dir_bits(junk.x - px, junk.y - py, 3) end
                -- (re)measure the arena when the room clears and again if still here after a while
                if quiet == 1 or quiet % 3000 == 0 then probe = {i = 1, still = 0, lx = px, ly = py, lo = {}, hi = {}} end
                if probe.i <= 4 then
                    local dirs = {4, 8, 1, 2}                            -- left, right, up, down
                    mv = dirs[probe.i]
                    if px == probe.lx and py == probe.ly then probe.still = probe.still + 1 else probe.still = 0 end
                    probe.lx, probe.ly = px, py
                    if probe.still >= 12 or quiet > probe.i * 400 then
                        if probe.i == 1 then probe.lo.x = px elseif probe.i == 2 then probe.hi.x = px
                        elseif probe.i == 3 then probe.lo.y = py else probe.hi.y = py end
                        probe.i, probe.still = probe.i + 1, 0
                        if probe.i == 5 then
                            CX, CY = (probe.lo.x + probe.hi.x) // 2, (probe.lo.y + probe.hi.y) // 2
                            HALF_W, HALF_H = (probe.hi.x - probe.lo.x) // 2, (probe.hi.y - probe.lo.y) // 2
                            -- a probe blocked by an obstacle gives a short span: fall back to the
                            -- standard arena (about 218 x 160) around the wall that was reached
                            if HALF_W < 90 then CX, HALF_W = probe.lo.x + 109, 109 end
                            if HALF_H < 60 then CY, HALF_H = probe.lo.y + 80, 80 end
                            probe.t0 = quiet
                        end
                    end
                else
                    local k = ((((quiet - probe.t0) // 360) + seed) % 4) + 1
                    last_door = k
                    local tx, ty = CX + DOOR_OFS[k][1] * 2, CY + DOOR_OFS[k][2] * 2
                    if k == 1 or k == 3 then
                        mv = math.abs(px - CX) > 3 and dir_bits(CX - px, 0, 1) or dir_bits(0, ty - py, 0)
                    else
                        mv = math.abs(py - CY) > 3 and dir_bits(0, CY - py, 1) or dir_bits(tx - px, 0, 0)
                    end
                end
                if pick and pick.d < 150 * 150 and quiet < 240 then mv = dir_bits(pick.x - px, pick.y - py, 4) end
            end
        end
    end
    if f % (os.getenv("STV_BOT_LOGEVERY") and tonumber(os.getenv("STV_BOT_LOGEVERY")) or 300) == 0 then
        print(string.format("BOT f=%d state=%02x wave=%d quiet=%d mv=%x fire=%x p=%d,%d", f, gs, space:read_u16(WAVE), quiet, mv, fire, dbg_px, dbg_py))
    end
    for b = 0, 3 do
        want(out, ":IN0", 1 << b, (mv >> b) & 1)
        want(out, ":IN0", 1 << (b + 4), (fire >> b) & 1)
    end
    return out
end
