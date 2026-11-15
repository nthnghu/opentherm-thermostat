// Data log KPIs against the closed loop (control + plant), CSV, and the alerts.
#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "datalog.h"
#include "alerts.h"
#include "control.h"
#include "plant_sim.h"
#include "schedule.h"

#define NREC (7 * 1440)
#define NBURN 2048

static dl_sample_t sample(const plant_state_t *s, const ctrl_out_t *o, float sp, double t)
{
    return (dl_sample_t){
        .epoch = 1790000000 + (int64_t)t, .sensor_ok = true, .room_c = s->room_c, .humidity = 45, .target_c = sp,
        .setpoint_c = sp, .heat_mode = true, .ch_allowed = true, .boiler_up = true, .flame = s->flame,
        .dhw_active = s->dhw_active, .ch_enable = o->ch_enable, .dhw_enable = true, .tset_c = o->tset,
        .req_c = o->req_c, .p = o->p, .i = o->i, .flow_valid = true, .ret_valid = true, .mod_valid = true,
        .flow_c = s->flow_c, .return_c = s->return_c, .modulation = s->modulation, .ctrl_state = o->state,
        .cap_kw = 24, .min_kw = 4.3f,
    };
}

static void test_closed_loop_kpis(void)
{
    dl_rec_t *recs = calloc(NREC, sizeof(dl_rec_t));
    dl_burn_t *burns = calloc(NBURN, sizeof(dl_burn_t));
    dl_t d;
    datalog_init(&d, recs, NREC, burns, NBURN);

    plant_params_t pp;
    plant_default_params(&pp);
    pp.t_out = 10;
    pp.out_swing = 3;
    pp.water_l = 30;
    pp.dhw_draws = false;
    plant_state_t s;
    plant_init(&s, &pp, 21, 0);
    ctrl_config_t cfg;
    ctrl_default_config(&cfg);
    ctrl_t c;
    ctrl_init(&c, &cfg);
    double gas0 = 0, burner0 = 0;
    uint32_t starts0 = 0;
    const double warm = 12 * 3600, total = 36 * 3600;
    for (double t = 0; t < total; t += 1) {
        ctrl_in_t in = {.now_s = t, .heat_mode = true, .target_c = 21, .room_ok = true, .room_c = s.room_c,
                        .boiler_ok = true, .flame = s.flame, .dhw_active = s.dhw_active, .flow_valid = true,
                        .flow_c = s.flow_c, .mod_valid = true, .modulation = s.modulation};
        const ctrl_out_t *o = ctrl_step(&c, &in);
        plant_inputs_t pin = {.ch_enable = o->ch_enable, .target_c = o->tset, .dhw_enable = false};
        plant_step(&s, &pp, &pin, 1);
        dl_sample_t ds = sample(&s, o, 21, t);
        datalog_tick(&d, &ds);
        if (t == warm - 1) {
            gas0 = s.gas_kwh;
            burner0 = s.burner_s;
            starts0 = s.ch_starts;
        }
    }
    CHECK(datalog_count(&d) == 36 * 60, "records %d", datalog_count(&d));
    dl_kpi_t k;
    datalog_kpi(&d, 24, &k);
    double plant_starts_h = (s.ch_starts - starts0) / 24.0;
    printf("   24 h KPIs: starts/h %.2f (plant %.2f), median burn %.1f min, short %.0f%%, cond %.0f%%, "
           "mod %.1f%%, at min %.0f%%, gas est %.1f kWh (plant %.1f), burner %.1f h (plant %.1f), room %.2f, "
           "comfort %.2f Kh\n",
           k.ch_starts_h, plant_starts_h, k.median_burn_min, k.short_pct, k.cond_pct, k.mean_mod, k.min_mod_pct,
           k.gas_kwh, s.gas_kwh - gas0, k.burner_h, (s.burner_s - burner0) / 3600, k.mean_room, k.comfort_kh);
    CHECK(NEAR(k.hours, 24, 0.01), "window %.2f", k.hours);
    CHECK(NEAR(k.ch_starts_h, plant_starts_h, 0.1), "starts match the plant");
    CHECK(k.ch_starts_h <= 3.0 && k.median_burn_min >= 10, "low-load KPIs as in test_control");
    CHECK(k.short_pct < 10, "short %.0f", k.short_pct);
    CHECK(k.cond_pct > 99, "condensing %.0f", k.cond_pct);
    CHECK(NEAR(k.burner_h, (s.burner_s - burner0) / 3600, 0.05), "burner hours");
    CHECK(fabs(k.gas_kwh - (s.gas_kwh - gas0)) / (s.gas_kwh - gas0) < 0.12, "gas estimate within 12 %%");
    CHECK(NEAR(k.mean_room, 21, 0.3) && k.comfort_kh < 0.5, "comfort");
    dl_kpi_t k1;
    datalog_kpi(&d, 1, &k1);
    CHECK(NEAR(k1.hours, 1, 0.01) && k1.ch_starts <= 4, "1 h window: %u starts", k1.ch_starts);
    datalog_kpi(&d, 24 * 30, &k1);
    CHECK(NEAR(k1.hours, 36, 0.01), "window longer than the data");

    // CSV
    dl_rec_t r;
    CHECK(datalog_get(&d, 0, &r) && r.up_min == 1, "oldest");
    char line[400];
    int n = datalog_csv_row(&r, line, sizeof(line));
    int commas = 0, hcommas = 0;
    for (int i = 0; i < n; i++) {
        commas += line[i] == ',';
    }
    for (const char *h = datalog_csv_header(); *h; h++) {
        hcommas += *h == ',';
    }
    CHECK(n > 0 && commas == hcommas && line[n - 1] == '\n', "csv columns %d vs %d", commas, hcommas);
    CHECK(datalog_csv_row(&r, line, 20) < 0, "csv overflow detected");
    free(recs);
    free(burns);
}

static void test_ring_wrap(void)
{
    dl_rec_t recs[10];
    dl_burn_t burns[4];
    dl_t d;
    datalog_init(&d, recs, 10, burns, 4);
    dl_sample_t s = {.sensor_ok = true, .room_c = 20, .target_c = 21, .heat_mode = true, .ch_allowed = true};
    for (int t = 0; t < 25 * 60; t++) {
        s.flame = (t % 300) < 120;  // a 2 min burn every 5 min
        datalog_tick(&d, &s);
    }
    dl_rec_t r;
    CHECK(datalog_count(&d) == 10 && datalog_get(&d, 0, &r) && r.up_min == 16, "wrapped, oldest %u", r.up_min);
    CHECK(!datalog_get(&d, 10, &r), "past the end");
    dl_kpi_t k;
    datalog_kpi(&d, 1, &k);
    CHECK(NEAR(k.hours, 10 / 60.0, 0.001), "only 10 min of data");
    CHECK(NEAR(k.ch_starts_h, k.ch_starts, 0.001), "rate over at least 1 h: %.2f", k.ch_starts_h);
    CHECK(k.ch_burns == 2 && NEAR(k.median_burn_min, 2, 0.01) && k.short_pct == 100, "burns %u", k.ch_burns);
    CHECK(NEAR(k.comfort_kh, 0.5 * 10 / 60.0, 0.001), "comfort deficit %.3f", k.comfort_kh);
}

static void test_alerts(void)
{
    alert_config_t c;
    alert_default_config(&c);
    alerts_t a;
    alerts_init(&a, &c);
    alert_in_t in = {.link_up = true, .link_known = true, .sensor_ok = true, .heating_allowed = true,
                     .room_c = 21, .target_c = 21, .clock_ok = true, .press_valid = true, .pressure_bar = 1.5f};
    double t = 0;
    const alert_out_t *o = NULL;
#define RUN(sec) for (double e = t + (sec); t < e; t += 1) { in.now_s = t; o = alerts_step(&a, &in); }
    RUN(10);
    CHECK(o->count == 0 && o->text[0] == 0, "quiet");

    in.pressure_bar = 0.8f;
    RUN(20);
    CHECK(!o->active[ALERT_PRESSURE], "pressure delay");
    RUN(15);
    CHECK(o->active[ALERT_PRESSURE] && strstr(o->text, "0.8 bar"), "low pressure: %s", o->text);
    in.pressure_bar = 1.5f;
    RUN(30);
    CHECK(o->active[ALERT_PRESSURE], "held while clearing");
    RUN(35);
    CHECK(!o->active[ALERT_PRESSURE], "cleared");

    in.link_up = false;
    RUN(59);
    CHECK(!o->active[ALERT_LINK], "link: not yet");
    RUN(2);
    CHECK(o->active[ALERT_LINK], "link lost after 60 s");
    in.link_up = true;
    RUN(61);

    in.boiler_fault = true, in.fault_valid = true, in.oem_fault = 10;
    in.sensor_ok = false;
    RUN(70);
    CHECK(o->active[ALERT_FAULT] && o->active[ALERT_SENSOR] && o->count == 2, "two alerts");
    CHECK(strcmp(o->text, "Boiler fault (code 10) (+1 more)") == 0, "headline: %s", o->text);
    c.fault_on = false;
    alerts_set_config(&a, &c);
    RUN(1);
    CHECK(!o->active[ALERT_FAULT], "switched off clears at once");
    in.boiler_fault = false, in.sensor_ok = true;
    RUN(61);

    in.ch_starts_1h = 6, in.ch_burns_1h = 5, in.median_burn_1h_min = 2;
    RUN(1);
    CHECK(o->active[ALERT_SHORT_CYCLE], "short cycling");
    in.ch_starts_1h = 2;
    RUN(61);
    CHECK(!o->active[ALERT_SHORT_CYCLE], "short cycling over");

    in.room_c = 19.5f;
    RUN(3 * 3600 - 10);
    CHECK(!o->active[ALERT_COLD], "cold: not yet");
    RUN(20);
    CHECK(o->active[ALERT_COLD], "cold room after 3 h");
    in.heating_allowed = false;
    RUN(61);
    CHECK(!o->active[ALERT_COLD], "not while heating is off");
    in.room_c = 21;

    in.pasteur_result = PASTEUR_ID56_IGNORED;
    RUN(1);
    CHECK(o->active[ALERT_PASTEUR], "pasteurisation");
    in.pasteur_result = PASTEUR_OK;
    RUN(61);
    CHECK(!o->active[ALERT_PASTEUR], "pasteurisation ok");

    // Leak: 0.4 bar lost over a day
    alerts_init(&a, &c);
    t = 0;
    for (int h = 0; h <= 25; h++) {
        in.pressure_bar = 1.6f - 0.4f * h / 25;
        RUN(3600);
    }
    CHECK(o->active[ALERT_LEAK], "pressure falling");

    char v[24];
    CHECK(alert_config_count() > 10 && alert_config_get(&c, 0, v, sizeof(v)), "table");
    CHECK(alert_config_set(&c, "pressure_min_bar", "0.8") && NEAR(c.pressure_min_bar, 0.8, 0.001), "set");
    CHECK(!alert_config_set(&c, "pressure_min_bar", "5"), "range");
}

int main(void)
{
    test_closed_loop_kpis();
    test_ring_wrap();
    test_alerts();
    TEST_DONE();
}
