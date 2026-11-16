// Schedule layer: weekly program, boost/away/summer, hot water windows/guard/piggy-back, pasteurisation
// against the simulated boiler (ID 56 write check), and the settings table.
#include <string.h>
#include "test.h"
#include "schedule.h"
#include "cfgtable.h"
#include "control.h"
#include "otb_core.h"
#include "ot_sim.h"
#include "ot_frame.h"

#define DAY0 20003  // a Monday (1970-01-01 was a Thursday)

static sched_in_t at(double t)
{
    int32_t day = DAY0 + (int32_t)(t / 86400);
    int m = (int)((t - (day - DAY0) * 86400.0) / 60);
    return (sched_in_t){.now_s = t, .clock_ok = true, .wday = (day + 3) % 7, .min_of_day = m, .day = day,
                        .epoch = (int64_t)day * 86400 + (int64_t)m * 60, .comfort_c = 21};
}

static double hm(int d, int h, int m)
{
    return d * 86400.0 + h * 3600.0 + m * 60.0;
}

static void test_program(void)
{
    sched_config_t c;
    sched_default_config(&c);
    CHECK(sched_level_at(&c, 0, 329) == SCHED_ECO, "Mon 05:29 (from Sun 23:00)");
    CHECK(sched_level_at(&c, 0, 330) == SCHED_COMFORT, "Mon 05:30");
    CHECK(sched_level_at(&c, 0, 1350) == SCHED_ECO, "Mon 22:30");
    CHECK(sched_level_at(&c, 6, 419) == SCHED_ECO && sched_level_at(&c, 6, 420) == SCHED_COMFORT, "Sun 07:00");

    CHECK(sched_prog_parse(&c, 2, "17:00=c 06:00=c 08:00=e 22:30=e"), "parse");
    char buf[120];
    sched_prog_format(&c, 2, buf, sizeof(buf));
    CHECK(strcmp(buf, "06:00=c 08:00=e 17:00=c 22:30=e") == 0, "sorted: %s", buf);
    CHECK(sched_level_at(&c, 2, 600) == SCHED_ECO && sched_level_at(&c, 2, 1100) == SCHED_COMFORT, "Wed");
    CHECK(!sched_prog_parse(&c, 2, "25:00=c"), "bad hour");
    CHECK(!sched_prog_parse(&c, 2, "06:00=x"), "bad level");
    CHECK(!sched_prog_parse(&c, 2, "1:00=c 2:00=e 3:00=c 4:00=e 5:00=c 6:00=e 7:00=c"), "too many");
    sched_prog_format(&c, 2, buf, sizeof(buf));
    CHECK(strcmp(buf, "06:00=c 08:00=e 17:00=c 22:30=e") == 0, "unchanged after errors: %s", buf);
    CHECK(sched_prog_parse(&c, 3, ""), "empty day");
    CHECK(sched_level_at(&c, 3, 720) == SCHED_ECO, "empty Thursday inherits Wed 22:30 Eco");

    sched_config_t e;
    sched_default_config(&e);
    for (int d = 0; d < 7; d++) {
        sched_prog_parse(&e, d, "");
    }
    CHECK(sched_level_at(&e, 4, 100) == SCHED_COMFORT, "empty program = Comfort");

    // Program on: target and next change
    sched_default_config(&c);
    c.prog_on = true;
    sched_t s;
    sched_init(&s, &c);
    sched_in_t in = at(hm(0, 4, 0));  // Mon 04:00
    const sched_out_t *o = sched_step(&s, &in);
    CHECK(o->src == SCHED_SRC_ECO && NEAR(o->target_c, 19, 0.01), "eco %.2f", o->target_c);
    CHECK(o->next_change_min == 90 && o->next_level == SCHED_COMFORT, "next %d", o->next_change_min);
    in = at(hm(0, 12, 0));
    o = sched_step(&s, &in);
    CHECK(o->src == SCHED_SRC_COMFORT && NEAR(o->target_c, 21, 0.01), "comfort");
    in = at(hm(4, 23, 0));  // Fri 23:00 -> Sat 07:00
    o = sched_step(&s, &in);
    CHECK(o->next_change_min == 8 * 60, "Fri night -> Sat 07:00: %d", o->next_change_min);
    in.clock_ok = false;
    o = sched_step(&s, &in);
    CHECK(o->src == SCHED_SRC_CONSTANT && NEAR(o->target_c, 21, 0.01), "no clock: constant Comfort");
    c.prog_on = false;
    sched_set_config(&s, &c);
    in = at(hm(0, 4, 0));
    o = sched_step(&s, &in);
    CHECK(o->src == SCHED_SRC_CONSTANT && NEAR(o->target_c, 21, 0.01), "program off");
}

static void test_boost_away_summer(void)
{
    sched_config_t c;
    sched_default_config(&c);
    c.prog_on = true;
    sched_t s;
    sched_init(&s, &c);
    double t = hm(0, 3, 0);
    sched_in_t in = at(t);
    CHECK(sched_boost(&s, t, true), "boost allowed");
    const sched_out_t *o = sched_step(&s, &in);
    CHECK(o->src == SCHED_SRC_BOOST && NEAR(o->target_c, 22, 0.01) && o->boost_left_s == 3600, "boost");
    in = at(t + 3601);
    o = sched_step(&s, &in);
    CHECK(o->src == SCHED_SRC_ECO && o->boost_left_s == 0, "boost ends");
    c.boost_on = false;
    sched_set_config(&s, &c);
    CHECK(!sched_boost(&s, t, true), "boost refused when switched off");

    // Away until a date
    in = at(hm(1, 12, 0));
    CHECK(sched_away(&s, true, in.epoch + 2 * 86400), "away");
    o = sched_step(&s, &in);
    CHECK(o->src == SCHED_SRC_AWAY && NEAR(o->target_c, 15, 0.01) && o->away, "away target");
    in = at(hm(3, 12, 1));
    o = sched_step(&s, &in);
    CHECK(!o->away && o->src == SCHED_SRC_COMFORT, "away over");
    sched_away(&s, true, 0);
    in = at(hm(3, 12, 2));
    in.clock_ok = false;
    o = sched_step(&s, &in);
    CHECK(o->away, "indefinite away holds without a clock");
    c.away_on = false;
    sched_set_config(&s, &c);
    o = sched_step(&s, &in);
    CHECK(!o->away, "switching the feature off cancels away");

    // Summer: 24 h mean from hourly buckets
    sched_default_config(&c);
    c.summer_on = true;
    sched_init(&s, &c);
    for (double tt = 0; tt < 11 * 3600; tt += 60) {
        in = at(tt);
        in.outside_valid = true;
        in.outside_c = 25;
        o = sched_step(&s, &in);
    }
    CHECK(!o->summer && !o->outside_mean_valid, "not enough data yet");
    for (double tt = 11 * 3600; tt < 14 * 3600; tt += 60) {
        in = at(tt);
        in.outside_valid = true;
        in.outside_c = 25;
        o = sched_step(&s, &in);
    }
    CHECK(o->summer && !o->ch_allowed && o->src == SCHED_SRC_SUMMER, "summer on");
    CHECK(sched_boost(&s, in.now_s, true), "boost");
    o = sched_step(&s, &in);
    CHECK(o->ch_allowed && o->src == SCHED_SRC_BOOST, "boost wins over summer");
    sched_boost(&s, in.now_s, false);
    double tt = 14 * 3600;
    for (; tt < 60 * 3600 && o->summer; tt += 60) {
        in = at(tt);
        in.outside_valid = true;
        in.outside_c = 10;
        o = sched_step(&s, &in);
    }
    CHECK(!o->summer && o->ch_allowed, "summer off after cooling");
    CHECK(o->outside_mean_c < 16.01, "left at mean %.2f (< 18 - 2)", o->outside_mean_c);
    c.summer_on = false;
    sched_set_config(&s, &c);
    in = at(tt);
    o = sched_step(&s, &in);
    CHECK(!o->summer, "summer feature off");
}

static void test_dhw_logic(void)
{
    sched_config_t c;
    sched_default_config(&c);
    sched_t s;
    sched_init(&s, &c);
    sched_in_t in = at(hm(0, 12, 0));
    const sched_out_t *o = sched_step(&s, &in);
    CHECK(o->dhw_enable && o->dhw_reason == DHW_R_ALWAYS && o->dhw_set == 0, "defaults: always, no ID 56");

    c.dhw_mode = DHW_WINDOWS;
    sched_set_config(&s, &c);
    in.tank_valid = true;
    in.tank_c = 48;
    o = sched_step(&s, &in);
    CHECK(!o->dhw_enable && o->dhw_reason == DHW_R_CLOSED, "12:00 closed");
    in = at(hm(0, 5, 30));
    in.tank_valid = true, in.tank_c = 48;
    o = sched_step(&s, &in);
    CHECK(o->dhw_enable && o->dhw_reason == DHW_R_WINDOW, "05:30 window");
    in = at(hm(0, 12, 0));
    in.clock_ok = false;
    o = sched_step(&s, &in);
    CHECK(o->dhw_enable && o->dhw_reason == DHW_R_NOCLOCK, "no clock -> enabled");

    // Guard with hysteresis
    in = at(hm(0, 12, 0));
    in.tank_valid = true, in.tank_c = 39;
    o = sched_step(&s, &in);
    CHECK(o->dhw_enable && o->dhw_reason == DHW_R_GUARD, "guard below 40");
    in.tank_c = 43;
    o = sched_step(&s, &in);
    CHECK(o->dhw_enable, "guard latched to 45");
    in.tank_c = 45;
    o = sched_step(&s, &in);
    CHECK(!o->dhw_enable, "guard released at 45");
    c.guard_on = false;
    sched_set_config(&s, &c);
    in.tank_c = 30;
    o = sched_step(&s, &in);
    CHECK(!o->dhw_enable, "guard switched off");

    // Wrapping window
    c.dhw_win[2] = (sched_window_t){1380, 60};
    sched_set_config(&s, &c);
    in = at(hm(0, 23, 30));
    CHECK(sched_step(&s, &in)->dhw_reason == DHW_R_WINDOW, "23:30 in 23:00-01:00");
    in = at(hm(1, 0, 30));
    CHECK(sched_step(&s, &in)->dhw_reason == DHW_R_WINDOW, "00:30 in 23:00-01:00");

    // Piggy-back: only after a CH burn
    c.piggy_on = true;
    sched_set_config(&s, &c);
    double t = hm(1, 12, 0);
    in = at(t);
    in.flame = true;
    sched_step(&s, &in);
    in = at(t + 600);
    in.flame = false;
    o = sched_step(&s, &in);
    CHECK(o->dhw_enable && o->dhw_reason == DHW_R_PIGGY, "piggy after CH burn");
    in = at(t + 600 + 20 * 60 + 1);
    o = sched_step(&s, &in);
    CHECK(!o->dhw_enable, "piggy over after 20 min");
    in = at(t + 3000);
    in.flame = true, in.dhw_active = true;
    sched_step(&s, &in);
    in = at(t + 3300);
    o = sched_step(&s, &in);
    CHECK(!o->dhw_enable, "no piggy after a DHW burn");

    // DHW boost, away off, mode off
    t = hm(1, 13, 0);
    CHECK(sched_dhw_boost(&s, t, true), "dhw boost");
    in = at(t);
    o = sched_step(&s, &in);
    CHECK(o->dhw_enable && o->dhw_reason == DHW_R_BOOST && o->dhw_boost_left_s == 3600, "dhw boost on");
    sched_dhw_boost(&s, t, false);
    c.away_dhw_off = true;
    c.dhw_mode = DHW_ALWAYS;
    c.guard_on = true;
    sched_set_config(&s, &c);
    sched_away(&s, true, 0);
    in.tank_valid = true, in.tank_c = 50;
    o = sched_step(&s, &in);
    CHECK(!o->dhw_enable && o->dhw_reason == DHW_R_AWAY, "away: DHW off");
    in.tank_c = 35;
    o = sched_step(&s, &in);
    CHECK(o->dhw_enable && o->dhw_reason == DHW_R_GUARD, "away: guard still works");
    c.dhw_mode = DHW_OFF;
    sched_set_config(&s, &c);
    o = sched_step(&s, &in);
    CHECK(!o->dhw_enable && o->dhw_reason == DHW_R_OFF, "mode off");
    CHECK(!sched_dhw_boost(&s, t, true), "no DHW boost in mode off");

    c.dhw_mode = DHW_ALWAYS;
    c.dhw_set_on = true;
    c.dhw_set_c = 48;
    sched_set_config(&s, &c);
    CHECK(NEAR(sched_step(&s, &in)->dhw_set, 48, 0.01), "ID 56 value when the feature is on");
}

/* --------------------------------------------------------- with the bus */

static otb_xfer_result_t sim_xfer(void *ctx, uint32_t req, uint32_t *resp, uint16_t *lat)
{
    return ot_sim_respond((ot_sim_t *)ctx, req, resp) ? OTB_XFER_OK : OTB_XFER_TIMEOUT;
}

typedef struct {
    otb_core_t bus;
    ot_sim_t boiler;
    sched_t s;
    double t;
} rig_t;

static void rig_init(rig_t *r, const sched_config_t *c, bool writable)
{
    otb_init(&r->bus);
    ot_sim_init(&r->boiler, 20, 10);
    r->boiler.plant_p.dhw_draws = false;
    r->boiler.q.dhw_set_writable = writable;
    sched_init(&r->s, c);
    r->t = hm(6, 12, 50);  // Sunday 12:50, pasteurisation default at 13:00
}

static const sched_out_t *rig_run(rig_t *r, double seconds)
{
    const sched_out_t *o = &r->s.out;
    for (double end = r->t + seconds; r->t < end; r->t += 1) {
        otb_cycle(&r->bus, (int64_t)(r->t * 1000), sim_xfer, &r->boiler);
        ot_sim_tick(&r->boiler, 1);
        const otb_status_t *st = &r->bus.st;
        sched_in_t in = at(r->t);
        in.tank_valid = st->valid & OTB_V_TDHW;
        in.tank_c = st->tank_c;
        in.flame = st->slave_flags & OT_S_FLAME;
        in.dhw_active = st->slave_flags & OT_S_DHW_ACTIVE;
        in.dhw_set_valid = st->valid & OTB_V_DHW_SET;
        in.dhw_set_read = st->dhw_set_c;
        in.id56 = st->dhw_set_verify;
        o = sched_step(&r->s, &in);
        r->bus.cmd = (otb_cmd_t){.ch_enable = false, .dhw_enable = o->dhw_enable, .tset = 10, .dhw_set = o->dhw_set};
    }
    return o;
}

static void test_id56_bus(void)
{
    // Not writable (P7 = 2 behaviour): ACKed but the read-back shows the panel value
    rig_t r;
    sched_config_t c;
    sched_default_config(&c);
    c.dhw_set_on = true;
    c.dhw_set_c = 45;
    rig_init(&r, &c, false);
    rig_run(&r, 30);
    CHECK(r.bus.st.dhw_set_verify == OTB_DHWSET_IGNORED, "ignored: %d", r.bus.st.dhw_set_verify);
    CHECK(NEAR(r.bus.st.dhw_set_c, 50, 0.01), "panel value kept %.1f", r.bus.st.dhw_set_c);
    uint32_t writes0 = r.boiler.frames;
    rig_run(&r, 120);
    CHECK(r.boiler.frames - writes0 < 120 * 4, "no hammering");

    rig_init(&r, &c, true);
    rig_run(&r, 30);
    CHECK(r.bus.st.dhw_set_verify == OTB_DHWSET_OK && NEAR(r.boiler.dhw_set, 45, 0.01), "writable: %d %.1f",
          r.bus.st.dhw_set_verify, r.boiler.dhw_set);
    // Panel changed behind our back -> rewritten within 10 min
    r.boiler.dhw_set = 55;
    r.boiler.plant_p.tank_set_c = 55;
    rig_run(&r, 15 * 60);
    CHECK(NEAR(r.boiler.dhw_set, 45, 0.01), "rewritten %.1f", r.boiler.dhw_set);

    // Feature off: never written
    sched_default_config(&c);
    rig_init(&r, &c, true);
    rig_run(&r, 120);
    CHECK(r.bus.st.dhw_set_verify == OTB_DHWSET_UNKNOWN && NEAR(r.boiler.dhw_set, 50, 0.01), "untouched");
}

static void test_pasteurisation(void)
{
    sched_config_t c;
    sched_default_config(&c);
    c.pasteur_on = true;
    rig_t r;

    // Writable: tank to 65, held 30 min, then back to the panel value (50)
    rig_init(&r, &c, true);
    const sched_out_t *o = rig_run(&r, 9 * 60);
    CHECK(!o->pasteur_running, "not before 13:00");
    o = rig_run(&r, 2 * 60);
    CHECK(o->pasteur_running && NEAR(o->dhw_set, 65, 0.01), "started at 13:00");
    double max_tank = 0;
    for (int i = 0; i < 150 && o->pasteur_running; i++) {
        o = rig_run(&r, 60);
        max_tank = fmax(max_tank, r.boiler.plant.tank_c);
    }
    CHECK(!o->pasteur_running && o->pasteur_result == PASTEUR_OK, "result %s", sched_pasteur_name(o->pasteur_result));
    CHECK(max_tank >= 63, "tank reached %.1f", max_tank);
    o = rig_run(&r, 60);
    CHECK(NEAR(r.boiler.dhw_set, 50, 0.01), "restored %.1f", r.boiler.dhw_set);
    CHECK(o->dhw_set == 0, "ID 56 handed back to the panel: %.1f", o->dhw_set);
    r.boiler.dhw_set = 52;  // the user changes the panel afterwards: left alone
    r.boiler.plant_p.tank_set_c = 52;
    rig_run(&r, 20 * 60);
    CHECK(NEAR(r.boiler.dhw_set, 52, 0.01), "panel change kept %.1f", r.boiler.dhw_set);
    o = rig_run(&r, 3 * 3600);
    CHECK(!o->pasteur_running, "once per week, not again the same day");

    // Not writable: the run stops with "id56-ignored" and nothing changes
    rig_init(&r, &c, false);
    o = rig_run(&r, 15 * 60);
    CHECK(!o->pasteur_running && o->pasteur_result == PASTEUR_ID56_IGNORED, "result %s",
          sched_pasteur_name(o->pasteur_result));
    CHECK(NEAR(r.boiler.dhw_set, 50, 0.01), "panel value");

    // Started right after a boot: waits for the ID 56 reading instead of failing
    rig_init(&r, &c, true);
    r.t = hm(2, 10, 0);
    CHECK(sched_pasteur_now(&r.s, true), "manual start at boot");
    o = rig_run(&r, 2);
    CHECK(!o->pasteur_running && o->pasteur_result == PASTEUR_NONE, "waiting for readings");
    o = rig_run(&r, 60);
    CHECK(o->pasteur_running, "started once ID 56 was read");
    // No tank sensor at all: gives up after the wait
    sched_t s2;
    sched_init(&s2, &c);
    sched_pasteur_now(&s2, true);
    sched_in_t in = at(hm(2, 10, 0));
    sched_step(&s2, &in);
    in = at(hm(2, 10, 6));
    o = sched_step(&s2, &in);
    CHECK(!o->pasteur_running && o->pasteur_result == PASTEUR_NO_TANK, "no tank: %s", sched_pasteur_name(o->pasteur_result));

    // Manual start and cancel; timeout
    c.pasteur_hold_min = 30;
    c.pasteur_timeout_min = 30;
    rig_init(&r, &c, true);
    r.t = hm(2, 10, 0);
    rig_run(&r, 60);
    CHECK(sched_pasteur_now(&r.s, true), "manual start");
    o = rig_run(&r, 5);
    CHECK(o->pasteur_running, "running");
    sched_pasteur_now(&r.s, false);
    o = rig_run(&r, 5);
    CHECK(!o->pasteur_running && o->pasteur_result == PASTEUR_CANCELLED, "cancelled");
    sched_pasteur_now(&r.s, true);
    o = rig_run(&r, 31 * 60);
    CHECK(o->pasteur_result == PASTEUR_TIMEOUT, "timeout (30 min hold cannot fit in 30 min): %s",
          sched_pasteur_name(o->pasteur_result));
    c.pasteur_on = false;
    sched_set_config(&r.s, &c);
    CHECK(!sched_pasteur_now(&r.s, true), "refused when switched off");
}

static void test_config_table(void)
{
    sched_config_t c;
    sched_default_config(&c);
    char v[24];
    int n = sched_config_count();
    CHECK(n > 30, "fields %d", n);
    bool seen_time = false;
    for (int i = 0; i < n; i++) {
        CHECK(sched_config_get(&c, i, v, sizeof(v)), "get %d", i);
        if (strcmp(sched_config_name(i), "pasteur_min") == 0) {
            seen_time = strcmp(v, "13:00") == 0;
        }
    }
    CHECK(seen_time, "time formatting");
    CHECK(sched_config_set(&c, "dhw_win1_start", "4:45") && c.dhw_win[0].start == 285, "time parse");
    CHECK(sched_config_set(&c, "dhw_win2_start", "-") && c.dhw_win[1].start == SCHED_UNUSED &&
              c.dhw_win[1].end == SCHED_UNUSED, "window unused");
    CHECK(!sched_config_set(&c, "pasteur_min", "-"), "start time required");
    CHECK(!sched_config_set(&c, "pasteur_c", "90"), "range");
    CHECK(!sched_config_set(&c, "nope", "1"), "name");
    CHECK(sched_config_set(&c, "summer_on", "on") && c.summer_on, "bool");
    sched_config_t bad;
    memset(&bad, 0xFF, sizeof(bad));
    sched_sanitize(&bad);
    CHECK(bad.pasteur_c <= 75 && bad.dhw_mode <= 2 && bad.version == SCHED_CONFIG_VERSION, "sanitize garbage");
}

static const ct_field_t *find(const char *name)
{
    for (int i = 0; i < sched_config_count(); i++) {
        if (strcmp(sched_config_field(i)->name, name) == 0) {
            return sched_config_field(i);
        }
    }
    for (int i = 0; i < ctrl_config_count(); i++) {
        if (strcmp(ctrl_config_field(i)->name, name) == 0) {
            return ctrl_config_field(i);
        }
    }
    return NULL;
}

static void test_step(void)
{
    char o[24];
    ct_step_value(find("eco_setback_k"), "2.00", 1, o, sizeof(o));
    CHECK(strcmp(o, "2.10") == 0, "float 0.5..6 steps 0.1: %s", o);
    ct_step_value(find("eco_setback_k"), "6.00", 1, o, sizeof(o));
    CHECK(strcmp(o, "6.00") == 0, "clamped at max: %s", o);
    ct_step_value(find("flow_max_c"), "55.00", -1, o, sizeof(o));
    CHECK(strcmp(o, "54.50") == 0, "range 50 steps 0.5: %s", o);
    ct_step_value(find("prog_on"), "0", 1, o, sizeof(o));
    CHECK(strcmp(o, "1") == 0, "bool toggles");
    ct_step_value(find("pasteur_min"), "13:00", 1, o, sizeof(o));
    CHECK(strcmp(o, "13:15") == 0, "time +15: %s", o);
    ct_step_value(find("pasteur_min"), "00:00", -1, o, sizeof(o));
    CHECK(strcmp(o, "00:00") == 0, "required time stops at 00:00: %s", o);
    ct_step_value(find("dhw_win1_start"), "00:00", -1, o, sizeof(o));
    CHECK(strcmp(o, "-") == 0, "optional time goes to unused: %s", o);
    ct_step_value(find("dhw_win1_start"), "-", 1, o, sizeof(o));
    CHECK(strcmp(o, "00:00") == 0, "and back: %s", o);
    ct_step_value(find("duty_min_burn_s"), "600", 1, o, sizeof(o));
    CHECK(strcmp(o, "660") == 0, "range over 1000 (seconds) steps 60: %s", o);
}

// Switch points with a fixed temperature next to Comfort and Eco.
static void test_fixed_levels(void)
{
    sched_config_t c;
    sched_default_config(&c);
    CHECK(sched_prog_parse(&c, 0, "06:00=c 08:00=18.5 17:00=t21 22:30=e"), "parse fixed");
    char buf[120];
    sched_prog_format(&c, 0, buf, sizeof(buf));
    CHECK(strcmp(buf, "06:00=c 08:00=18.5 17:00=21 22:30=e") == 0, "format: %s", buf);
    CHECK(sched_prog_parse(&c, 1, buf), "round trip");
    CHECK(memcmp(c.prog[0], c.prog[1], sizeof(c.prog[0])) == 0, "same day after the round trip");
    CHECK(!sched_prog_parse(&c, 2, "06:00=4.5") && !sched_prog_parse(&c, 2, "06:00=31") &&
          !sched_prog_parse(&c, 2, "06:00=21x"), "out of range / junk refused");
    CHECK(sched_prog_parse(&c, 2, "06:00=20.3"), "rounded");
    sched_prog_format(&c, 2, buf, sizeof(buf));
    CHECK(strcmp(buf, "06:00=20.25") == 0, "0.25 steps: %s", buf);
    CHECK(sched_level_from_c(2) == SCHED_FIXED_MIN && sched_level_from_c(40) == SCHED_FIXED_MAX, "clamped");
    CHECK(NEAR(sched_level_temp(SCHED_ECO, 21, 2), 19, 1e-6) && NEAR(sched_level_temp(SCHED_COMFORT, 21, 2), 21, 1e-6) &&
          NEAR(sched_level_temp(sched_level_from_c(17.5f), 21, 2), 17.5, 1e-6), "level temperatures");

    // Sanitising keeps fixed levels, turns junk into Comfort
    c.prog[3][0] = (sched_point_t){600, 80};
    c.prog[3][1] = (sched_point_t){700, 7};
    sched_sanitize(&c);
    CHECK(c.prog[3][0].level == 80 && c.prog[3][1].level == SCHED_COMFORT, "sanitise %u %u", c.prog[3][0].level,
          c.prog[3][1].level);

    // Program step: Monday 08:00-17:00 at 18.5 °C, then 21 °C, dial (Comfort) at 22
    sched_default_config(&c);
    c.prog_on = true;
    sched_prog_parse(&c, 0, "06:00=c 08:00=18.5 17:00=21 22:30=e");
    sched_t s;
    sched_init(&s, &c);
    sched_in_t in = at(hm(0, 12, 0));
    in.comfort_c = 22;
    const sched_out_t *o = sched_step(&s, &in);
    CHECK(o->src == SCHED_SRC_FIXED && NEAR(o->target_c, 18.5, 0.01), "fixed 18.5 (%.2f)", o->target_c);
    CHECK(o->next_change_min == 300 && o->next_level == sched_level_from_c(21), "next: 21 at 17:00 (%d, %u)",
          o->next_change_min, o->next_level);
    in = at(hm(0, 7, 0));
    in.comfort_c = 22;
    o = sched_step(&s, &in);
    CHECK(o->src == SCHED_SRC_COMFORT && NEAR(o->target_c, 22, 0.01), "comfort follows the dial");
    CHECK(strcmp(sched_src_name(SCHED_SRC_FIXED), "program") == 0, "name");
}

// Dial while the program runs: hold until the next switch, then the program again
static void test_override(void)
{
    sched_config_t c;
    sched_default_config(&c);
    sched_t s;
    sched_init(&s, &c);
    CHECK(!sched_override(&s, true, 23), "program off: no override");
    c.prog_on = true;
    sched_prog_parse(&c, 0, "06:00=c 08:00=18.5 17:00=21 22:30=e");
    sched_set_config(&s, &c);
    sched_in_t in = at(hm(0, 12, 0));
    in.comfort_c = 22;
    sched_step(&s, &in);
    CHECK(sched_override(&s, true, 20.5f), "override accepted");
    const sched_out_t *o = sched_step(&s, &in);
    CHECK(o->src == SCHED_SRC_MANUAL && NEAR(o->target_c, 20.5, 0.01), "manual 20.5 (%s %.2f)",
          sched_src_name(o->src), o->target_c);
    CHECK(o->override_until == in.epoch - in.epoch % 60 + 300 * 60, "until 17:00");
    in = at(hm(0, 16, 59));
    in.comfort_c = 22;
    o = sched_step(&s, &in);
    CHECK(o->src == SCHED_SRC_MANUAL, "still manual at 16:59");
    in = at(hm(0, 17, 0));
    in.comfort_c = 22;
    o = sched_step(&s, &in);
    CHECK(o->src == SCHED_SRC_FIXED && NEAR(o->target_c, 21, 0.01) && o->override_until == 0,
          "17:00: program again (%s %.2f)", sched_src_name(o->src), o->target_c);
    // Cancel early; boost wins over an override
    sched_override(&s, true, 19);
    o = sched_step(&s, &in);
    CHECK(o->src == SCHED_SRC_MANUAL, "manual again");
    sched_boost(&s, in.now_s, true);
    o = sched_step(&s, &in);
    CHECK(o->src == SCHED_SRC_BOOST, "boost wins");
    sched_boost(&s, in.now_s, false);
    sched_override(&s, false, 0);
    o = sched_step(&s, &in);
    CHECK(o->src == SCHED_SRC_FIXED, "back to the program");
    // Program switched off: override dropped
    sched_override(&s, true, 25);
    c.prog_on = false;
    sched_set_config(&s, &c);
    o = sched_step(&s, &in);
    CHECK(o->src == SCHED_SRC_CONSTANT && NEAR(o->target_c, 22, 0.01), "program off: the setpoint");
    CHECK(strcmp(sched_src_name(SCHED_SRC_MANUAL), "manual") == 0, "name");
}

int main(void)
{
    test_override();
    test_fixed_levels();
    test_step();
    test_program();
    test_boost_away_summer();
    test_dhw_logic();
    test_id56_bus();
    test_pasteurisation();
    test_config_table();
    TEST_DONE();
}
