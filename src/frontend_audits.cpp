#include "stv/frontend.hpp"
namespace stv {
namespace {
// MENU NUMBER_WORK/P_TIME and UTIL HEXTOASC, using explicit u32 arithmetic.
struct AuditText {
    char* data; usize size; usize used{}; bool overflow{};
    void character(char c) {
        if (!data || used + 1 >= size) { overflow = true; return; }
        data[used++] = c; data[used] = 0;
    }
    void text(const char* s) { if (!s) { overflow = true; return; } while (*s) character(*s++); }
    void number(u32 n) {
        char reverse[16]; unsigned count = 0, digits = 0;
        do { reverse[count++] = char('0' + n % 10); n /= 10;
             if (n && ++digits == 3) { reverse[count++] = ','; digits = 0; }
        } while (n);
        while (count) character(reverse[--count]);
    }
    void hundredths(u32 n) { if (n < 10) character('0'); number(n); }
    Status finish() const { return overflow ? Status::fail("Audit text buffer too small or missing source string") : Status{}; }
};
u32 rounded(u32 dividend, u32 divisor) {
    if (!divisor) return 0;
    u32 quotient = dividend / divisor;
    // ROUNDED_A0_INTO_A2 uses a 32-bit left shift of the remainder.
    if (u32((dividend % divisor) << 1) >= divisor) ++quotient;
    return quotient;
}
}
Status ui_audit_text(const UiPersistent& p, const UiAuditRow& row, char* out, usize capacity) {
    if (!out || !capacity) return Status::fail("Missing audit text destination");
    out[0] = 0; AuditText text{out, capacity};
    const auto& a = p.game_audits;
    const u32 starts = a[14] + a[15], continues = a[16] + a[17];
    const u32 plays = starts + continues, elapsed = a[18] + a[19];
    const u32 weighted = a[18] + (a[19] << 1);
    u32 value = 0; bool time = false;
    if (text_equal(row.routine, "0")) {
        if (row.index >= 64) return Status::fail("Audit index outside native state", row.index);
        value = a[row.index];
    } else if (text_equal(row.routine, "STARTS")) value = starts;
    else if (text_equal(row.routine, "CONTINS")) value = continues;
    else if (text_equal(row.routine, "DOPLAYS")) value = plays;
    else if (text_equal(row.routine, "DOHSLEFT")) {
        if (!p.adjustments[14]) { text.text(ui_literal("MENU.ASM:M_HSOFF")); return text.finish(); }
        value = p.high_score_reset;
    } else if (text_equal(row.routine, "DO_P1MIN") || text_equal(row.routine, "DO_P2MIN") || text_equal(row.routine, "DO_HOURS")) {
        u32 ticks = text_equal(row.routine,"DO_P1MIN") ? a[18] : text_equal(row.routine,"DO_P2MIN") ? a[19] : elapsed;
        value = ticks / 12 + (ticks % 12 >= 6 ? 1u : 0u); time = true;
    } else if (text_equal(row.routine, "DO_AVGT") || text_equal(row.routine, "DO_ELAPSE")) {
        value = rounded(u32((text_equal(row.routine,"DO_AVGT") ? weighted : elapsed) * 5u), plays); time = true;
    } else return Status::fail("Untranslated audit formatter");
    if (time) { text.number(value / 60); text.character(':'); text.hundredths(value % 60); }
    else text.number(value);
    return text.finish();
}
Status ui_money_text(const UiPersistent& p, char* out, usize capacity) {
    if (!out || !capacity) return Status::fail("Missing money text destination");
    out[0] = 0; AuditText text{out, capacity};
    if (!p.adjustments[9]) return {}; // PRINT_COL's explicit OFF condition.
    const u32 number = p.adjustments[1];
    if (!number || number > source_ui_coin_preset_count) return Status::fail("Invalid coin preset for collection audit");
    const auto& preset = source_ui_coin_presets[number-1];
    const bool standard = p.adjustments[21] != 0;
    u32 sum = 0;
    for (unsigned chute = 0; chute < 4; ++chute) sum += p.game_audits[1+chute] * (standard ? u32(preset.values[chute]) : 1u);
    const u32 divisor = standard ? p.adjustments[9] : 1u;
    if (standard) text.text(preset.prefix);
    text.number(sum / divisor);
    if (divisor != 1) { text.character('.'); text.hundredths(rounded(u32((sum % divisor)*100u), divisor)); }
    text.text(standard ? preset.suffix : ui_literal("MENU.ASM:COINS_TRAIL"));
    return text.finish();
}
}
