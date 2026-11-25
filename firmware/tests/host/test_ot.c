// OpenTherm frames, the bus core and the simulated Immergas boiler.
#include <stddef.h>
#include <string.h>
#include "test.h"
#include "ot_frame.h"
#include "ot_sim.h"
#include "otb_core.h"
#include "alerts.h"

static otb_xfer_result_t sim_xfer(void *ctx, uint32_t req, uint32_t *resp, uint16_t *lat)
{
    *lat = 40;
    return ot_sim_respond((ot_sim_t *)ctx, req, resp) ? OTB_XFER_OK : OTB_XFER_TIMEOUT;
}

// Runs n one-second bus cycles; the simulated boiler advances 1 s per cycle.
static void run(otb_core_t *c, ot_sim_t *b, int64_t *now_ms, int n)
{
    for (int i = 0; i < n; i++) {
        otb_cycle(c, *now_ms, sim_xfer, b);
        ot_sim_tick(b, 1.0f);
        *now_ms += 1000;
    }
}

static void test_frames(void)
{
    // Vectors checked in docs/research (opentherm-frame.html)
    CHECK(ot_frame(OT_READ_DATA, 0, 0x0300) == 0x00000300, "status CH+DHW");
    CHECK(ot_frame(OT_READ_DATA, 0, 0x03CA) == 0x000003CA, "Immergas status");
    CHECK(ot_frame(OT_WRITE_DATA, 1, 0x2D00) == 0x10012D00, "write 45 C");
    CHECK(ot_frame(OT_READ_ACK, 25, 0x3480) == 0x40193480, "flow 52.5");
    CHECK(ot_frame(OT_WRITE_ACK, 1, 0x2D00) == 0xD0012D00, "write-ack 45 C");
    CHECK(ot_frame(OT_UNKNOWN_DATA_ID, 19, 0) == 0x70130000, "unknown ID 19");
    CHECK(ot_frame(OT_WRITE_DATA, 1, 0x0A00) == 0x10010A00, "write 10 C");
    CHECK(ot_parity_ok(0x40193480) && !ot_parity_ok(0xC0193480), "parity");
    CHECK(NEAR(ot_f88(0x3480), 52.5, 1e-6), "f8.8 decode");
    CHECK(ot_to_f88(-1.5f) == 0xFE80, "f8.8 negative %04X", ot_to_f88(-1.5f));
    CHECK(ot_to_f88(45.0f) == 0x2D00, "f8.8 encode");
}

static void test_link_and_support(void)
{
    ot_sim_t b;
    ot_sim_init(&b, 20, 8);
    otb_core_t c;
    otb_init(&c);
    int64_t now = 0;
    run(&c, &b, &now, 120);
    CHECK(c.st.link == OTB_LINK_UP, "link up");
    CHECK(c.st.member_id == 27 && (c.st.valid & OTB_V_CONFIG), "member ID %d", c.st.member_id);
    CHECK(c.st.max_cap_kw == 24 && c.st.min_mod_pct == 18, "caps %d/%d", c.st.max_cap_kw, c.st.min_mod_pct);
    CHECK(c.st.ch_max == 85 && c.st.ch_min == 25, "CH bounds %d..%d", c.st.ch_min, c.st.ch_max);
    CHECK(c.support[OT_ID_TRET] == OTB_ID_SUPPORTED, "ID 28 supported");
    CHECK(c.support[OT_ID_TOUTSIDE] == OTB_ID_UNSUPPORTED, "ID 27 unsupported (no outdoor sensor)");
    CHECK(c.support[OT_ID_DHW_FLOW] == OTB_ID_UNSUPPORTED, "ID 19 unsupported");
    CHECK(c.support[OT_ID_CH_PUMP_STARTS] == OTB_ID_UNSUPPORTED, "ID 117 unsupported");
    CHECK(c.st.timeouts == 0 && c.st.bad == 0, "no errors");
    // Every cycle carries a status frame: frames >= 2 per cycle once the link is up
    CHECK(c.st.frames >= 2 * c.st.cycles, "frames %u cycles %u", c.st.frames, c.st.cycles);
    // Re-probe after an hour: unsupported IDs get asked again, then fall back to unsupported
    run(&c, &b, &now, 3700);
    CHECK(c.support[OT_ID_TOUTSIDE] == OTB_ID_UNSUPPORTED, "ID 27 still unsupported after re-probe");
    CHECK(c.st.unknown_id > 9, "re-probe happened (%u UNKNOWN-DATAID)", c.st.unknown_id);
}

static void test_immergas_quirks(void)
{
    // Without 0xCA, writes are ACKed but ignored: the boiler keeps its own value.
    ot_sim_t b;
    ot_sim_init(&b, 18, 8);
    otb_core_t c;
    otb_init(&c);
    c.flags.status_ca = false;
    c.cmd = (otb_cmd_t){.ch_enable = true, .dhw_enable = true, .tset = 45};
    int64_t now = 0;
    run(&c, &b, &now, 10);
    CHECK(c.st.tset_acked, "write ACKed");
    CHECK(NEAR(b.id1, 10, 0.01), "boiler ignored ID 1 without 0xCA (id1=%.1f)", b.id1);
    c.flags.status_ca = true;
    run(&c, &b, &now, 3);
    CHECK(NEAR(b.id1, 45, 0.01), "boiler took ID 1 with 0xCA (id1=%.1f)", b.id1);

    // Heats to ID 57: the mirror makes ID 57 follow ID 1 (refreshed every minute, so the write ignored
    // without 0xCA above is repeated).
    b.q.heats_to_57 = true;
    run(&c, &b, &now, 65);
    bool ch;
    CHECK(NEAR(ot_sim_effective_target(&b, &ch), 45, 0.01) && ch, "mirror: target %.1f", ot_sim_effective_target(&b, &ch));
    c.flags.mirror_57 = false;
    c.cmd.tset = 40;
    run(&c, &b, &now, 5);
    CHECK(NEAR(ot_sim_effective_target(&b, &ch), 45, 0.01), "no mirror: boiler stays at the old ID 57");
    c.flags.mirror_57 = true;
    run(&c, &b, &now, 5);
    CHECK(NEAR(ot_sim_effective_target(&b, &ch), 40, 0.01), "mirror resumed");

    // CH off: only the explicit off (ID 1 = 10) stops a boiler that ignores the CH bit.
    c.flags.explicit_off = false;
    c.cmd.ch_enable = false;
    run(&c, &b, &now, 3);
    ot_sim_effective_target(&b, &ch);
    CHECK(ch, "CH bit alone: boiler keeps heating (reported Immergas behaviour)");
    c.flags.explicit_off = true;
    run(&c, &b, &now, 3);
    ot_sim_effective_target(&b, &ch);
    CHECK(!ch && NEAR(b.id1, 10, 0.01), "explicit off: boiler stops");
}

static void test_misses_and_e31(void)
{
    ot_sim_t b;
    ot_sim_init(&b, 20, 8);
    b.q.miss_pct = 17;  // seen on one Immergas
    otb_core_t c;
    otb_init(&c);
    c.cmd = (otb_cmd_t){.ch_enable = true, .dhw_enable = true, .tset = 45};
    int64_t now = 0;
    run(&c, &b, &now, 6 * 3600);
    CHECK(c.st.link == OTB_LINK_UP && c.st.link_losses == 0, "17 %% misses: link never lost in 6 h (%u losses)",
          c.st.link_losses);
    CHECK(c.st.retries > 0, "status retries used (%u)", c.st.retries);
    CHECK(!b.e31, "no E31 with misses");

    // Master silent for 70 s -> E31 and CH locked; frames resume -> link up again, fault visible, CH still locked
    b.q.miss_pct = 0;
    for (int i = 0; i < 70; i++) {
        ot_sim_tick(&b, 1);
        now += 1000;
    }
    CHECK(b.e31 && b.ch_locked, "E31 after 60 s of silence");
    run(&c, &b, &now, 30);
    CHECK(c.st.link == OTB_LINK_UP, "link back");
    CHECK((c.st.slave_flags & OT_S_FAULT) && c.st.oem_fault == 31, "fault bit and code 31 read (code %d)", c.st.oem_fault);
    bool ch;
    ot_sim_effective_target(&b, &ch);
    CHECK(!ch, "CH stays locked until a power cycle");
    ot_sim_power_cycle(&b);
    run(&c, &b, &now, 10);
    ot_sim_effective_target(&b, &ch);
    CHECK(ch && !b.e31, "power cycle with the master talking: CH back");
}

// Adapter unplugged or boiler off from power-on: the link was never up, it still goes LOST (not a "loss") and the
// link alert fires after link_s, so a dead adapter at start-up is reported too.
static void test_never_answered(void)
{
    ot_sim_t b;
    ot_sim_init(&b, 20, 8);
    b.q.miss_pct = 100;
    otb_core_t c;
    otb_init(&c);
    int64_t now = 0;
    run(&c, &b, &now, 4);
    CHECK(c.st.link == OTB_LINK_UNKNOWN, "4 s without any answer: still unknown");
    run(&c, &b, &now, 1);
    CHECK(c.st.link == OTB_LINK_LOST && c.st.link_losses == 0, "5 s: lost, not counted as a loss");

    alert_config_t k;
    alert_default_config(&k);
    alerts_t a;
    alerts_init(&a, &k);
    alert_in_t in = {.sensor_ok = true, .clock_ok = true, .link_up = false};
    const alert_out_t *o = NULL;
    for (int s = 0; s <= 5 + k.link_s; s++) {
        in.now_s = s;
        in.link_known = s >= 5;  // as the bus reports it
        o = alerts_step(&a, &in);
        if (s == 5 + k.link_s - 1) {
            CHECK(!o->active[ALERT_LINK], "no link alert before link_s");
        }
    }
    CHECK(o->active[ALERT_LINK], "link alert %u s after the bus gave up", k.link_s);
}

// A controller that stops handing over commands must not leave the last "heat" command on the bus forever.
static void test_cmd_guard(void)
{
    otb_cmd_t c = {.ch_enable = true, .dhw_enable = true, .tset = 55, .dhw_set = 50};
    CHECK(!otb_cmd_guard(&c, 0) && !otb_cmd_guard(&c, OTB_CMD_MAX_AGE_MS), "fresh command untouched");
    CHECK(c.ch_enable && c.tset == 55, "fresh command keeps heating");
    CHECK(otb_cmd_guard(&c, OTB_CMD_MAX_AGE_MS + 1), "stale command caught");
    CHECK(!c.ch_enable && c.tset == 10, "stale: heating off, low flow setpoint");
    CHECK(c.dhw_enable && c.dhw_set == 50, "stale: hot water left as it was");

    // On the bus: the simulated boiler stops heating once the stale command is in force
    ot_sim_t b;
    ot_sim_init(&b, 18, 8);
    otb_core_t k;
    otb_init(&k);
    int64_t now = 0;
    k.cmd = (otb_cmd_t){.ch_enable = true, .tset = 55};
    run(&k, &b, &now, 60);
    bool ch;
    ot_sim_effective_target(&b, &ch);
    CHECK(ch, "heating while commands are fresh");
    otb_cmd_guard(&k.cmd, OTB_CMD_MAX_AGE_MS + 1);
    run(&k, &b, &now, 10);
    ot_sim_effective_target(&b, &ch);
    CHECK(!ch, "boiler told to stop after the controller went silent");
}

static void test_link_loss(void)
{
    ot_sim_t b;
    ot_sim_init(&b, 20, 8);
    otb_core_t c;
    otb_init(&c);
    int64_t now = 0;
    run(&c, &b, &now, 10);
    b.q.miss_pct = 100;
    run(&c, &b, &now, 4);
    CHECK(c.st.link == OTB_LINK_UP, "4 s of silence: still up");
    run(&c, &b, &now, 1);
    CHECK(c.st.link == OTB_LINK_LOST && c.st.link_losses == 1, "5 s: lost");
    uint32_t frames = c.st.frames;
    run(&c, &b, &now, 10);
    CHECK(c.st.frames - frames == 20, "keeps sending status (with retry) while lost: %u", c.st.frames - frames);
    b.q.miss_pct = 0;
    run(&c, &b, &now, 1);
    CHECK(c.st.link == OTB_LINK_UP, "reconnect");
}

static void test_plant(void)
{
    // A steady 45 °C flow on a +5 °C day warms a 17 °C house and burns with few starts.
    ot_sim_t b;
    ot_sim_init(&b, 17, 0);
    otb_core_t c;
    otb_init(&c);
    c.cmd = (otb_cmd_t){.ch_enable = true, .dhw_enable = true, .tset = 45};
    int64_t now = 0;
    run(&c, &b, &now, 12 * 3600);
    CHECK(b.plant.room_c > 18.5, "room warmed to %.1f", b.plant.room_c);
    CHECK(b.plant.ch_starts > 0 && b.plant.dhw_burner_s > 60, "starts CH %u, DHW burner %.0f s", b.plant.ch_starts,
          b.plant.dhw_burner_s);
    CHECK(c.st.flow_c > 30 && c.st.flow_c < 60, "flow read %.1f", c.st.flow_c);
    CHECK(c.st.counters[0] == b.plant.starts || c.st.counters[0] + 50 > b.plant.starts, "counter 116 read");
    printf("   plant 12 h: room %.2f C, flow %.1f, return %.1f, starts CH %u DHW %u, gas %.1f kWh\n", b.plant.room_c,
           b.plant.flow_c, b.plant.return_c, b.plant.ch_starts, b.plant.dhw_starts, b.plant.gas_kwh);
}

// Switching from the simulated to the real boiler (#29): the bus task starts with the simulator, so nothing it
// learnt may make an absent real boiler look present.
static void test_new_boiler(void)
{
    ot_sim_t sim;
    ot_sim_init(&sim, 20, 8);
    otb_core_t c;
    otb_init(&c);
    c.flags.mirror_57 = false;
    c.cmd = (otb_cmd_t){.ch_enable = true, .dhw_enable = true, .tset = 50};
    int64_t now = 0;
    run(&c, &sim, &now, 30);
    CHECK(c.st.link == OTB_LINK_UP && c.st.valid && c.st.ok > 0, "simulator: link up, values read");

    otb_new_boiler(&c);
    CHECK(c.st.link == OTB_LINK_UNKNOWN && c.st.valid == 0 && c.st.ok == 0 && c.st.frames == 0,
          "new boiler: link %d valid %x ok %u", c.st.link, c.st.valid, c.st.ok);
    CHECK(c.support[OT_ID_TRET] == OTB_ID_UNKNOWN, "supported IDs forgotten");
    CHECK(!c.flags.mirror_57 && c.cmd.ch_enable && c.cmd.tset == 50, "flags and command kept");

    ot_sim_t none;  // the real bus with nothing connected
    ot_sim_init(&none, 20, 8);
    none.q.miss_pct = 100;
    run(&c, &none, &now, 4);
    CHECK(c.st.link != OTB_LINK_UP, "no answer on the new bus: never up (%d)", c.st.link);
    run(&c, &none, &now, 2);
    CHECK(c.st.link == OTB_LINK_LOST, "then lost");
}

// ID 17 from the Victrix Tera: -113 % at minimum flame (seen 2026-11-25) is read as 0 %, over 100 capped
static void test_modulation_decode(void)
{
    CHECK(NEAR(otb_decode_mod(ot_to_f88(37.5f)), 37.5, 0.01), "normal value kept");
    CHECK(otb_decode_mod(0x8F00) == 0, "negative (-113 %%) = minimum flame, 0 %%");
    CHECK(otb_decode_mod(ot_to_f88(120)) == 100, "above 100 capped");
    CHECK(otb_decode_mod(0) == 0 && otb_decode_mod(ot_to_f88(100)) == 100, "edges");
}

// ID 33 as sent by the Tera (f8.8) and by the spec (s16)
static void test_exhaust_decode(void)
{
    CHECK(NEAR(otb_decode_exhaust(0x2E00), 46.0, 0.01), "Tera f8.8 0x2E00 = 46 C (not 11776)");
    CHECK(NEAR(otb_decode_exhaust(0x2E80), 46.5, 0.01), "f8.8 with a fraction");
    CHECK(otb_decode_exhaust(46) == 46 && otb_decode_exhaust(180) == 180, "spec s16 kept");
    CHECK(otb_decode_exhaust((uint16_t)-5) == -5, "negative s16 kept");
}

// A loose wire (2026-11-25): the boiler still answers now and then, so the link never stays lost long enough for
// "not answering". The 5-minute failed share catches it, and clears once the line is clean again.
static alert_in_t poor_in(const otb_core_t *c, double now_s)
{
    return (alert_in_t){.now_s = now_s, .sensor_ok = true, .clock_ok = true, .link_up = c->st.link == OTB_LINK_UP,
                        .link_known = c->st.link != OTB_LINK_UNKNOWN, .frames_5m = c->st.frames_5m,
                        .fails_5m = c->st.fails_5m};
}

static void test_poor_link(void)
{
    ot_sim_t b;
    ot_sim_init(&b, 20, 8);
    otb_core_t c;
    otb_init(&c);
    alert_config_t k;
    alert_default_config(&k);
    alerts_t a;
    alerts_init(&a, &k);
    int64_t now = 0;
    const alert_out_t *o = NULL;
    for (int s = 0; s < 360; s++) {
        run(&c, &b, &now, 1);
        alert_in_t in = poor_in(&c, now / 1000.0);
        o = alerts_step(&a, &in);
    }
    CHECK(c.st.frames_5m >= 600 && c.st.fails_5m == 0, "clean line: %u frames, %u failed", c.st.frames_5m,
          c.st.fails_5m);
    CHECK(!o->active[ALERT_LINK_POOR], "no alert on a clean line");

    b.q.miss_pct = 50;
    int fired = -1;
    for (int s = 0; s < 400; s++) {
        run(&c, &b, &now, 1);
        alert_in_t in = poor_in(&c, now / 1000.0);
        o = alerts_step(&a, &in);
        if (fired < 0 && o->active[ALERT_LINK_POOR]) {
            fired = s;
        }
    }
    CHECK(fired >= 0 && fired < 180, "half the frames lost (after 5 clean minutes): alert after %d s", fired);
    CHECK(c.st.fails_5m * 100 >= 30u * c.st.frames_5m, "failed share %u of %u", c.st.fails_5m, c.st.frames_5m);
    CHECK(strstr(o->text, "unreliable") != NULL, "headline: %s", o->text);

    b.q.miss_pct = 0;
    for (int s = 0; s < 420; s++) {
        run(&c, &b, &now, 1);
        alert_in_t in = poor_in(&c, now / 1000.0);
        o = alerts_step(&a, &in);
    }
    CHECK(c.st.fails_5m == 0 && !o->active[ALERT_LINK_POOR], "clean again: cleared (%u failed)", c.st.fails_5m);

    // Switched off: never reported
    k.link_poor_on = false;
    alerts_set_config(&a, &k);
    alert_in_t bad = {.now_s = 1e5, .sensor_ok = true, .clock_ok = true, .link_up = true, .link_known = true,
                      .frames_5m = 900, .fails_5m = 800};
    CHECK(!alerts_step(&a, &bad)->active[ALERT_LINK_POOR], "link_poor_on 0: no alert");
    // While "not answering" is active, the unreliable alert stays out of the way
    k.link_poor_on = true;
    alerts_init(&a, &k);
    alert_in_t lost = {.sensor_ok = true, .clock_ok = true, .link_up = false, .link_known = true, .frames_5m = 300,
                       .fails_5m = 300};
    for (int s = 0; s <= k.link_s + 1; s++) {
        lost.now_s = s;
        o = alerts_step(&a, &lost);
    }
    CHECK(o->active[ALERT_LINK] && !o->active[ALERT_LINK_POOR], "lost: only 'not answering'");
}

// Saved alert settings from before the new alert (version 1) keep their values; the new fields get defaults.
static void test_alert_migrate(void)
{
    alert_config_t v1;
    alert_default_config(&v1);
    v1.version = 1;
    v1.link_s = 120;
    v1.clock_s = 900;
    size_t v1_len = offsetof(alert_config_t, clock_s) + sizeof(v1.clock_s);
    alert_config_t out;
    CHECK(alert_config_migrate(&out, &v1, v1_len), "v1 blob accepted");
    CHECK(out.link_s == 120 && out.clock_s == 900 && out.link_poor_on && out.link_poor_pct == 20 &&
              out.version == ALERT_CONFIG_VERSION, "v1 values kept, new defaults (%u %u %d %u)", out.link_s,
          out.clock_s, out.link_poor_on, out.link_poor_pct);
    alert_config_t cur = out;
    cur.link_poor_pct = 35;
    CHECK(alert_config_migrate(&out, &cur, sizeof(cur)) && out.link_poor_pct == 35, "current blob loads");
    cur.version = 7;
    CHECK(!alert_config_migrate(&out, &cur, sizeof(cur)), "unknown version refused");
}

int main(void)
{
    test_exhaust_decode();
    test_modulation_decode();
    test_frames();
    test_link_and_support();
    test_immergas_quirks();
    test_misses_and_e31();
    test_link_loss();
    test_cmd_guard();
    test_never_answered();
    test_poor_link();
    test_alert_migrate();
    test_plant();
    test_new_boiler();
    TEST_DONE();
}
