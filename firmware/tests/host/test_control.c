// Control law: closed-loop runs against the plant model, plus the safety features and the config table.
#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "control.h"
#include "plant_sim.h"

typedef struct {
    double mean_room, rms, starts_h, med_burn_min, short_pct, ret, cond_pct, gas_kwh;
    int burns;
} kpi_t;

static int cmpd(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

// 12 h warm-up + 48 h measured, 1 s steps. room_ok_until_h < 0: sensor always OK.
static kpi_t closed_loop(const ctrl_config_t *cfg, float t_out, float swing, float water_l, float sp, bool print,
                         const char *label)
{
    plant_params_t pp;
    plant_default_params(&pp);
    pp.t_out = t_out;
    pp.out_swing = swing;
    pp.water_l = water_l;
    pp.dhw_draws = false;
    plant_state_t s;
    plant_init(&s, &pp, sp, 0);
    ctrl_t c;
    ctrl_init(&c, cfg);
    const double warm = 12 * 3600, total = 60 * 3600;
    double sum = 0, sq = 0, n = 0, ret_sum = 0, fire = 0, cond = 0, burn_start = 0, gas0 = 0;
    double *burns = calloc(4000, sizeof(double));
    int nb = 0;
    bool last_flame = false;
    uint32_t starts0 = 0;
    for (double t = 0; t < total; t += 1) {
        ctrl_in_t in = {.now_s = t, .heat_mode = true, .target_c = sp, .room_ok = true, .room_c = s.room_c,
                        .boiler_ok = true, .flame = s.flame, .dhw_active = s.dhw_active, .flow_valid = true,
                        .flow_c = s.flow_c, .mod_valid = true, .modulation = s.modulation};
        const ctrl_out_t *o = ctrl_step(&c, &in);
        plant_inputs_t pin = {.ch_enable = o->ch_enable, .target_c = o->tset, .dhw_enable = false};
        plant_step(&s, &pp, &pin, 1);
        if (t == warm) {
            starts0 = s.ch_starts;
            gas0 = s.gas_kwh;
        }
        if (s.flame && !last_flame) {
            burn_start = t;
        }
        if (!s.flame && last_flame && t > warm && nb < 4000) {
            burns[nb++] = t - burn_start;
        }
        last_flame = s.flame;
        if (t >= warm) {
            double dev = s.room_c - sp;
            sum += s.room_c;
            sq += dev * dev;
            n++;
            if (s.flame) {
                fire++;
                ret_sum += s.return_c;
                if (s.return_c < 55) {
                    cond++;
                }
            }
        }
    }
    qsort(burns, nb, sizeof(double), cmpd);
    int shorts = 0;
    for (int i = 0; i < nb; i++) {
        shorts += burns[i] < 600;
    }
    kpi_t k = {
        .mean_room = sum / n, .rms = sqrt(sq / n), .starts_h = (s.ch_starts - starts0) / 48.0,
        .med_burn_min = nb ? burns[nb / 2] / 60 : 0, .short_pct = nb ? 100.0 * shorts / nb : 0,
        .ret = fire ? ret_sum / fire : 0, .cond_pct = fire ? 100 * cond / fire : 0, .gas_kwh = s.gas_kwh - gas0,
        .burns = nb,
    };
    free(burns);
    if (print) {
        printf("   %-26s room %.2f rms %.2f | starts/h %.2f median burn %.1f min short %.0f%% | return %.1f "
               "cond %.0f%% | gas %.0f kWh\n",
               label, k.mean_room, k.rms, k.starts_h, k.med_burn_min, k.short_pct, k.ret, k.cond_pct, k.gas_kwh);
    }
    return k;
}

static void test_closed_loop(void)
{
    ctrl_config_t pi;
    ctrl_default_config(&pi);
    ctrl_config_t plain = pi;  // PI without the low-load manager (strategy B of room-sim.html)
    plain.hold_on = false;
    plain.duty_on = false;
    ctrl_config_t onoff = pi;
    onoff.strategy = CTRL_STRATEGY_ONOFF;
    onoff.onoff_flow_c = 65;
    onoff.onoff_hyst_k = 0.25f;

    // This model house needs ~58 C flow at −10 C: with the 55 C default cap the room falls short (as ch. 10
    // says: allow up to 60 C only if the reference room can't reach the setpoint on design days).
    kpi_t capped = closed_loop(&pi, -10, 3, 80, 21, true, "PI 55 C cap  -10 C, 80 L");
    CHECK(capped.mean_room < 20.5, "55 C cap is respected on a design day (room %.2f)", capped.mean_room);
    ctrl_config_t pi60 = pi;
    pi60.flow_max_c = 60;
    kpi_t cold = closed_loop(&pi60, -10, 3, 80, 21, true, "PI 60 C cap  -10 C, 80 L");
    CHECK(fabs(cold.mean_room - 21) < 0.3 && cold.rms < 0.4, "cold day holds 21 C (mean %.2f rms %.2f)",
          cold.mean_room, cold.rms);
    CHECK(cold.starts_h <= 3.0, "cold day starts/h %.2f", cold.starts_h);

    kpi_t mid = closed_loop(&pi, 5, 4, 80, 21, true, "PI+low-load   +5 C, 80 L");
    CHECK(fabs(mid.mean_room - 21) < 0.3, "+5 C holds 21 C (mean %.2f)", mid.mean_room);
    CHECK(mid.starts_h <= 3.0 && mid.ret < 45, "+5 C: starts/h %.2f, return %.1f", mid.starts_h, mid.ret);

    kpi_t mild = closed_loop(&pi, 10, 4, 30, 21, true, "PI+low-load  +10 C, 30 L");
    kpi_t mild_plain = closed_loop(&plain, 10, 4, 30, 21, true, "plain PI     +10 C, 30 L");
    kpi_t mild_onoff = closed_loop(&onoff, 10, 4, 30, 21, true, "on/off 65 C  +10 C, 30 L");
    CHECK(fabs(mild.mean_room - 21) < 0.4, "+10 C holds 21 C (mean %.2f)", mild.mean_room);
    CHECK(mild.starts_h <= 3.05, "+10 C small system: starts/h %.2f <= 3", mild.starts_h);
    CHECK(mild.med_burn_min >= 9.5 && mild.short_pct < 20, "+10 C: median burn %.1f min, short %.0f%%",
          mild.med_burn_min, mild.short_pct);
    CHECK(mild_plain.starts_h > mild.starts_h * 1.5, "plain PI cycles much more (%.2f vs %.2f starts/h)",
          mild_plain.starts_h, mild.starts_h);
    CHECK(mild_onoff.ret > mild.ret + 8, "on/off 65 C returns hotter (%.1f vs %.1f)", mild_onoff.ret, mild.ret);
    CHECK(mild.gas_kwh < mild_onoff.gas_kwh, "PI+low-load uses less gas than on/off (%.0f vs %.0f kWh)", mild.gas_kwh,
          mild_onoff.gas_kwh);
}

static const ctrl_out_t *step(ctrl_t *c, double t, bool heat, float sp, bool ok, float room)
{
    ctrl_in_t in = {.now_s = t, .heat_mode = heat, .target_c = sp, .room_ok = ok, .room_c = room, .boiler_ok = true,
                    .flow_valid = true, .flow_c = 30, .mod_valid = true};
    return ctrl_step(c, &in);
}

static void test_sensor_loss(void)
{
    ctrl_config_t cfg;
    ctrl_default_config(&cfg);
    ctrl_t c;

    // Fixed fallback: 40 % of each 30 min at 42 C
    ctrl_init(&c, &cfg);
    step(&c, 0, true, 21, true, 19);
    int on = 0;
    for (int t = 1; t <= 1800; t++) {
        const ctrl_out_t *o = step(&c, t, true, 21, false, 0);
        CHECK(o->state == CTRL_ST_SENSOR_LOSS, "state sensor lost");
        if (o->ch_enable) {
            on++;
            if (on == 1) {
                CHECK(NEAR(o->tset, 42, 0.01), "fallback flow %.1f", o->tset);
            }
        }
    }
    CHECK(on >= 700 && on <= 740, "fallback duty 40 %% (%d s of 1800)", on);

    // Heating off (the 0.1 behaviour)
    cfg.loss_mode = CTRL_LOSS_HEAT_OFF;
    ctrl_init(&c, &cfg);
    step(&c, 0, true, 21, true, 18);
    const ctrl_out_t *o = step(&c, 1, true, 21, false, 0);
    CHECK(!o->ch_enable && o->state == CTRL_ST_OFF, "loss_mode 1: heating off");

    // Feature off: the last decision stays
    cfg.loss_mode = CTRL_LOSS_DISABLED;
    ctrl_init(&c, &cfg);
    o = step(&c, 0, true, 21, true, 18);
    bool before = o->ch_enable;
    float tset = o->tset;
    o = step(&c, 1, true, 21, false, 0);
    CHECK(o->ch_enable == before && NEAR(o->tset, tset, 0.01), "loss_mode 0: unchanged");

    // Sensor back: PI resumes
    cfg.loss_mode = CTRL_LOSS_FIXED;
    ctrl_init(&c, &cfg);
    for (int t = 0; t < 100; t++) {
        step(&c, t, true, 21, false, 0);
    }
    o = step(&c, 100, true, 21, true, 19);
    CHECK(o->state != CTRL_ST_SENSOR_LOSS, "sensor back: normal control (%s)", ctrl_state_name(o->state));
}

static void test_frost_and_off(void)
{
    ctrl_config_t cfg;
    ctrl_default_config(&cfg);
    ctrl_t c;
    ctrl_init(&c, &cfg);
    const ctrl_out_t *o = step(&c, 0, false, 21, true, 12);
    CHECK(!o->ch_enable, "mode off, 12 C: no heat");
    o = step(&c, 1, false, 21, true, 5.5f);
    CHECK(o->ch_enable && o->state == CTRL_ST_FROST && NEAR(o->tset, 35, 0.01), "frost floor at 5.5 C");
    o = step(&c, 2, false, 21, true, 6.2f);
    CHECK(o->ch_enable, "frost latch until 6.5 C");
    o = step(&c, 3, false, 21, true, 6.6f);
    CHECK(!o->ch_enable, "frost released at 6.6 C");
    cfg.frost_on = false;
    ctrl_init(&c, &cfg);
    o = step(&c, 0, false, 21, true, 4);
    CHECK(!o->ch_enable, "frost floor off: no heat at 4 C");
}

static void test_ufh_and_onoff(void)
{
    ctrl_config_t cfg;
    ctrl_default_config(&cfg);
    cfg.ufh_on = true;
    cfg.ufh_mixer_c = 38;
    cfg.duty_on = false;
    ctrl_t c;
    ctrl_init(&c, &cfg);
    const ctrl_out_t *o = step(&c, 0, true, 21, true, 20.9f);  // small error: ~36 C requested
    CHECK(o->ch_enable && o->tset >= 43 - 0.01, "UFH floor: tset %.1f >= mixer + 5", o->tset);

    ctrl_default_config(&cfg);
    cfg.strategy = CTRL_STRATEGY_ONOFF;
    ctrl_init(&c, &cfg);
    o = step(&c, 0, true, 21, true, 20.7f);
    CHECK(o->ch_enable && NEAR(o->tset, 50, 0.01) && o->state == CTRL_ST_ONOFF, "on/off: on below setpoint − hyst");
    o = step(&c, 1, true, 21, true, 21.1f);
    CHECK(o->ch_enable, "on/off: stays on inside the band");
    o = step(&c, 2, true, 21, true, 21.3f);
    CHECK(!o->ch_enable, "on/off: off above setpoint + hyst");
}

static void test_dhw_freeze(void)
{
    ctrl_config_t cfg;
    ctrl_default_config(&cfg);
    ctrl_t c;
    ctrl_init(&c, &cfg);
    for (int t = 0; t < 600; t++) {
        ctrl_in_t in = {.now_s = t, .heat_mode = true, .target_c = 21, .room_ok = true, .room_c = 20,
                        .boiler_ok = true, .dhw_active = true, .flame = true};
        ctrl_step(&c, &in);
    }
    CHECK(NEAR(c.i, 0, 1e-6), "integral frozen during DHW (I=%.3f)", c.i);
    cfg.freeze_on_dhw = false;
    ctrl_init(&c, &cfg);
    for (int t = 0; t < 600; t++) {
        ctrl_in_t in = {.now_s = t, .heat_mode = true, .target_c = 21, .room_ok = true, .room_c = 20,
                        .boiler_ok = true, .dhw_active = true, .flame = true};
        ctrl_step(&c, &in);
    }
    CHECK(c.i > 0.5f, "freeze off: integral grows (I=%.3f)", c.i);
}

static void test_config_table(void)
{
    ctrl_config_t cfg;
    ctrl_default_config(&cfg);
    CHECK(ctrl_config_count() >= 25, "fields %d", ctrl_config_count());
    CHECK(ctrl_config_set(&cfg, "pi_kp", "15") && NEAR(cfg.pi_kp, 15, 1e-6), "set kp");
    CHECK(!ctrl_config_set(&cfg, "pi_kp", "99"), "out of range refused");
    CHECK(!ctrl_config_set(&cfg, "pi_kp", "abc"), "garbage refused");
    CHECK(!ctrl_config_set(&cfg, "nope", "1"), "unknown name refused");
    CHECK(ctrl_config_set(&cfg, "duty_on", "off") && !cfg.duty_on, "bool off");
    CHECK(ctrl_config_set(&cfg, "loss_mode", "1") && cfg.loss_mode == 1, "loss mode");
    char v[16];
    int idx = -1;
    for (int i = 0; i < ctrl_config_count(); i++) {
        if (strcmp(ctrl_config_name(i), "pi_kp") == 0) {
            idx = i;
        }
    }
    CHECK(ctrl_config_get(&cfg, idx, v, sizeof(v)) && strcmp(v, "15.00") == 0, "get kp '%s'", v);
    cfg.pi_kp = 1000;
    cfg.flow_min_c = 70;
    ctrl_sanitize(&cfg);
    CHECK(cfg.pi_kp == 40 && cfg.flow_min_c <= cfg.flow_max_c, "sanitize clamps");
}

static void test_set_integral(void)
{
    ctrl_config_t c;
    ctrl_default_config(&c);
    ctrl_t k;
    ctrl_init(&k, &c);
    ctrl_set_integral(&k, 4.5f);
    CHECK(NEAR(k.i, 4.5, 1e-6), "restored integral %.2f", k.i);
    ctrl_set_integral(&k, 99);
    CHECK(NEAR(k.i, c.pi_i_limit_k, 1e-6), "clamped to the limit %.2f", k.i);
    ctrl_set_integral(&k, -99);
    CHECK(NEAR(k.i, -c.pi_i_limit_k, 1e-6), "clamped low %.2f", k.i);
    // Starts from before a restart count towards the starts-per-hour limit
    ctrl_init(&k, &c);
    ctrl_note_start(&k, -1800);
    ctrl_note_start(&k, -600);
    ctrl_note_start(&k, -4000);  // older than an hour: not counted
    ctrl_in_t in = {.now_s = 10, .heat_mode = true, .room_ok = true, .boiler_ok = true, .room_c = 21, .target_c = 21};
    const ctrl_out_t *o = ctrl_step(&k, &in);
    CHECK(o->starts_last_h == 2, "restored starts in the last hour: %u", o->starts_last_h);
}

// The user changes the setpoint: the burner decision follows within seconds, not after the slew ramp or a duty wait.
static void test_setpoint_step(void)
{
    ctrl_config_t c;
    ctrl_default_config(&c);
    ctrl_t k;
    ctrl_init(&k, &c);
    ctrl_in_t in = {.heat_mode = true, .room_ok = true, .boiler_ok = true, .room_c = 20.4f, .flow_valid = true,
                    .flow_c = 25};
    const ctrl_out_t *o = NULL;
    int t = 0;
    for (; t < 7200; t++) {  // satisfied at 20.0 for 2 h: idle
        in.now_s = t;
        in.target_c = 20.0f;
        o = ctrl_step(&k, &in);
    }
    CHECK(!o->ch_enable, "satisfied: idle (%s)", ctrl_state_name(o->state));
    in.target_c = 21.5f;
    for (int s = 0; s < 5; s++, t++) {
        in.now_s = t;
        o = ctrl_step(&k, &in);
    }
    CHECK(o->ch_enable && o->tset > c.flow_min_c, "raised 1.5 K: heats within 5 s (tset %.1f, %s)", o->tset,
          ctrl_state_name(o->state));
    for (int s = 0; s < 1800; s++, t++) {  // heat for 30 min, room unchanged
        in.now_s = t;
        in.flame = o->ch_enable;
        in.flow_c = o->ch_enable ? 45 : 25;
        o = ctrl_step(&k, &in);
    }
    in.target_c = 19.0f;
    for (int s = 0; s < 5; s++, t++) {
        in.now_s = t;
        o = ctrl_step(&k, &in);
    }
    CHECK(!o->ch_enable, "lowered below the room: stops within 5 s (%s, tset %.1f)", ctrl_state_name(o->state),
          o->tset);
    for (int s = 0; s < 3600; s++, t++) {
        in.now_s = t;
        o = ctrl_step(&k, &in);
        if (o->ch_enable) {
            break;
        }
    }
    CHECK(!o->ch_enable, "room 1.4 K above the setpoint: no duty burn for 1 h");
}

int main(void)
{
    test_setpoint_step();
    test_set_integral();
    test_config_table();
    test_sensor_loss();
    test_frost_and_off();
    test_ufh_and_onoff();
    test_dhw_freeze();
    test_closed_loop();
    TEST_DONE();
}
