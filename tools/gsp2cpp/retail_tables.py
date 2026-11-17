"""Layout of the L8 tables validated by the widescreen runtime."""

TABLES = (
    ("X_FFEC0710", 0xffec0720, "s16", 4, 32, 0),
    ("X_FFEEEA10", 0xffeeea20, "s16", 4, 32, 0),
    ("X_FFF364A0", 0xfff364b0, "s16", 5, 32, 0),
    ("X_FFF171A0", 0xfff171b0, "s16", 5, 32, 0),
    ("X_FFF2CE10", 0xfff2ce20, "s16", 4, 32, 0),
    ("X_FFF2CE90", 0xfff2cea0, "s16", 4, 32, 0),
    ("X_FFF313E0", 0xfff313f0, "s16", 4, 32, 0),
    ("X_FFE40AB0", 0xffe40ac0, "s16", 4, 32, 0),
    ("X_FFE3BBD0", 0xffe3bbe0, "s16", 4, 32, 0),
    ("X_FFE3BC50", 0xffe3bc60, "s16", 4, 32, 0),
    ("X_FFF46350", 0xfff46360, "s16", 3, 32, 0),
    ("X_FFF463B0", 0xfff463c0, "s16", 3, 32, 0),
    ("X_FFEE02C0", 0xffee02d0, "s16", 4, 32, 0),
    ("X_FFEEC900", 0xffeec900, "s16", 9, 32, 0),
    ("X_FFEE0D00", 0xffee0d00, "s16", 4, 32, 0),
    ("X_FFEE0D80", 0xffee0d80, "s16", 4, 32, 0),
    ("X_FFED7320", 0xffed7320, "s16", 4, 16, 0),
    ("X_FFED7360", 0xffed7360, "s16", 4, 16, 0),
    ("X_FFEC8930", 0xffec8940, "s16", 4, 32, 0),
    ("X_FFE4CFC0", 0xffe4cfc0, "s16", 10, 16, 0),
    ("retail_limits", 0xffe7c550, "u32", 3, 32, 0),
    ("formation_ptrs", 0xffe417c0, "u32", 11, 32, 0),
    ("formations", 0xffe4d100, "u32", 48, 32, 16),
    ("logo_smash", 0xffe97900, "u16", 13, 16, 0),
    ("logo_tv", 0xffe979d0, "u16", 13, 16, 0),
    ("hands_x", 0xffe70aa0, "u16", 55, 16, 0),
    ("walk_p1", 0xffea44f0, "u16", 17, 16, 0),
    ("walk_p2", 0xffea4600, "u16", 17, 16, 0),
    ("walk_left", 0xffea5450, "u16", 5, 16, 0),
    ("walk_right", 0xffea4d60, "u16", 5, 16, 0),
    ("movement_scripts", 0xffea3df0, "u32", 5, 32, 0),
    ("prize_x", 0xffe85200, "u8", 27, 8, 0),
    ("expected_FFE1F8A0", 0xffe1f8a0, "s32", 10, 32, 0),
    ("expected_FFE1F9E0", 0xffe1f9e0, "s32", 10, 32, 0),
    ("expected_FFE1FB20", 0xffe1fb20, "s32", 10, 32, 0),
    ("expected_FFE1FC60", 0xffe1fc60, "s32", 10, 32, 0),
    ("POP_ffe13018", 0xffe13018, "u8", 24, 8, 0),
    ("POP_ffe12e98", 0xffe12e98, "u8", 24, 8, 0),
    ("POP_ffe13178", 0xffe13178, "u8", 22, 8, 0),
    ("POP_ffe132c8", 0xffe132c8, "u8", 22, 8, 0),
    ("POP_ffe12f58", 0xffe12f58, "u8", 24, 8, 0),
    ("POP_ffe130d8", 0xffe130d8, "u8", 20, 8, 0),
    ("POP_ffe12d80", 0xffe12d80, "u8", 12, 8, 0),
    ("POP_ffe12e40", 0xffe12e40, "u8", 11, 8, 0),
)

def table_initializers(words, base):
    for name, address, kind, count, stride, group in TABLES:
        bits = int(kind[1:])
        values = []
        for i in range(count):
            offset = address - base + stride * i
            index, shift = divmod(offset, 16)
            value = words[index]
            if shift + bits > 16:
                value |= words[index + 1] << 16
            value = (value >> shift) & ((1 << bits) - 1)
            if kind.startswith("s") and value >= 1 << (bits - 1):
                value -= 1 << bits
            values.append(str(value) + ("u" if kind == "u32" else ""))
        if group:
            values = ["{" + ",".join(values[i:i+group]) + "}" for i in range(0,count,group)]
        yield "#define STV_ROM_" + name + " {" + ",".join(values) + "}"
