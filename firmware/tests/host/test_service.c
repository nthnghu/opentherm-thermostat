// Service routines against the simulated Immergas boiler: each routine must find the quirk the simulator has.
#include <string.h>
#include "test.h"
#include "service.h"
#include "otb_core.h"
#include "ot_sim.h"
#include "ot_frame.h"

static otb_xfer_result_t sim_xfer(void *ctx, uint32_t req, uint32_t *resp, uint16_t *lat)
{
    return ot_sim_respond((ot_sim_t *)ctx, req, resp) ? OTB_XFER_OK : OTB_XFER_TIMEOUT;
}

typedef struct {
    otb_core_t bus;
    ot_sim_t b;
    svc_t s;
    double t;
    otb_flags_t flags;
    const svc_out_t *o;
} rig_t;

static void rig_init(rig_t *r, float t_out)
{
    memset(r, 0, sizeof(*r));
    otb_init(&r->bus);
    ot_sim_init(&r->b, 20, 10);
    r->b.plant_p.dhw_draws = false;
    r->b.plant_p.t_out = t_out;
    r->b.plant_p.out_swing = 0;
    r->flags = r->bus.flags;
    svc_config_t c;
    svc_default_config(&c);
    c.step_min = 8;
    c.silence_s = 70;
    c.recover_min = 3;
    svc_init(&r->s, &c, NULL);
    svc_enter(&r->s, 0);
}

// Runs until the routine ends (or max_s). Returns the elapsed seconds.
static double run_ex(rig_t *r, double max_s, bool until_done)
{
    double start = r->t;
    for (; r->t < start + max_s; r->t += 1) {
        svc_in_t in = {.now_s = r->t, .epoch = 1790000000 + (int64_t)r->t, .sim = true, .bus = &r->bus.st,
                       .manual = &r->bus.manual, .flags = r->flags, .room_ok = true, .room_c = 20, .frost_c = 6,
                       .flow_max_c = 55};
        r->o = svc_step(&r->s, &in);
        if (r->o->manual) {
            otb_manual_request(&r->bus, r->o->m_type, r->o->m_id, r->o->m_data);
        }
        if (r->o->set_flags) {
            r->flags = r->o->flags;
        }
        r->bus.flags = r->flags;
        r->bus.cmd = r->o->override ? r->o->cmd : (otb_cmd_t){.ch_enable = false, .dhw_enable = true, .tset = 10};
        r->bus.silent = r->o->silent;
        otb_cycle(&r->bus, (int64_t)(r->t * 1000), sim_xfer, &r->b);
        ot_sim_tick(&r->b, 1);
        if (until_done && !r->o->running && r->t > start + 2) {
            break;
        }
    }
    return r->t - start;
}

static double run(rig_t *r, double max_s)
{
    return run_ex(r, max_s, true);
}

static const svc_result_t *go(rig_t *r, svc_routine_t k, double max_s)
{
    char why[96];
    bool ok = svc_start(&r->s, k, k == SVC_COMM_LOSS, r->t, why, sizeof(why));
    CHECK(ok, "start %s: %s", svc_routine_name(k), why);
    double d = run(r, max_s);
    const svc_result_t *res = &r->s.rec.r[k];
    printf("   %-10s %-7s %5.0f s  %s\n", svc_routine_name(k), svc_rstate_name(res->state), d, res->text);
    return res;
}

static void warm(rig_t *r)
{
    run(r, 20);  // link up, configuration read
}

static void test_identity_and_flags(void)
{
    rig_t r;
    rig_init(&r, 5);
    warm(&r);
    const svc_result_t *res = go(&r, SVC_IDENTITY, 300);
    CHECK(res->state == SVC_R_DONE && res->v[0] == 27 && res->v[1] == 24 && res->v[2] == 18 && res->sim, "identity");
    CHECK(strstr(res->text, "Sensors: 18 26 28") && strstr(res->text, "19 27 33"), "%s", res->text);

    res = go(&r, SVC_STATUS_CA, 120);
    CHECK(res->state == SVC_R_DONE && res->v[0] == 0 && res->v[1] == 1, "needs 0xCA");
    CHECK(r.flags.status_ca && r.flags.mirror_57, "flags restored");
    svc_apply_t a;
    CHECK(svc_recommend(&r.s.rec, SVC_STATUS_CA, &a) && a.set_ca && a.ca, "recommend ca = 1");
    CHECK(NEAR(r.b.id57, res->v[2], 0.01), "ID 57 restored (%.1f)", r.b.id57);

    r.b.q.need_ca = false;
    res = go(&r, SVC_STATUS_CA, 120);
    CHECK(res->v[0] == 1 && svc_recommend(&r.s.rec, SVC_STATUS_CA, &a) && !a.ca, "works without 0xCA");

    res = go(&r, SVC_WRITES, 120);
    CHECK(res->v[0] == 2 && res->v[1] == 1, "56 ignored, 57 works: %.0f %.0f", res->v[0], res->v[1]);
    r.b.q.dhw_set_writable = true;
    res = go(&r, SVC_WRITES, 120);
    CHECK(res->v[0] == 1 && NEAR(r.b.dhw_set, 50, 0.01), "56 works and restored (%.1f)", r.b.dhw_set);
}

static void test_setpoint(void)
{
    rig_t r;
    rig_init(&r, 0);
    warm(&r);
    const svc_result_t *res = go(&r, SVC_SETPOINT, 3600);
    CHECK(res->state == SVC_R_DONE && res->v[3] == 1, "heats to ID 1 (%.1f %.1f %.1f)", res->v[0], res->v[1], res->v[2]);
    rig_init(&r, 0);
    r.b.q.heats_to_57 = true;
    warm(&r);
    res = go(&r, SVC_SETPOINT, 3600);
    CHECK(res->v[3] == 2, "heats to ID 57");
    svc_apply_t a;
    CHECK(svc_recommend(&r.s.rec, SVC_SETPOINT, &a) && a.set_mirror && a.mirror, "keep the mirror");
}

static void test_ch_off(void)
{
    rig_t r;
    rig_init(&r, 0);
    warm(&r);
    const svc_result_t *res = go(&r, SVC_CH_OFF, 3600);
    CHECK(res->state == SVC_R_DONE && res->v[0] == 0 && res->v[1] == 1, "Immergas: bit alone is not enough");
    svc_apply_t a;
    CHECK(svc_recommend(&r.s.rec, SVC_CH_OFF, &a) && a.explicit_off, "explicit_off");
    rig_init(&r, 0);
    r.b.q.ignores_ch_off = false;
    warm(&r);
    res = go(&r, SVC_CH_OFF, 3600);
    CHECK(res->v[0] == 1, "standard boiler: the bit stops it");
}

static void test_id14(void)
{
    rig_t r;
    rig_init(&r, -5);
    warm(&r);
    const svc_result_t *res = go(&r, SVC_ID14, 3600);
    CHECK(res->state == SVC_R_DONE && res->v[3] == 0, "not honoured");
    rig_init(&r, -5);
    r.b.q.honours_id14 = true;
    warm(&r);
    res = go(&r, SVC_ID14, 3600);
    CHECK(res->state == SVC_R_DONE && res->v[3] == 1, "honoured");
    CHECK(NEAR(r.b.plant_p.max_mod_pct, 100, 0.01), "ID 14 restored to 100");
}

static void test_min_fire_and_cutout(void)
{
    rig_t r;
    rig_init(&r, 5);
    warm(&r);
    const svc_result_t *res = go(&r, SVC_MIN_FIRE, 3 * 3600);
    CHECK(res->state == SVC_R_DONE && res->v[0] > 25 && res->v[0] < 55 && res->v[2] <= 5, "OPV %.1f", res->v[0]);
    svc_apply_t a;
    CHECK(svc_recommend(&r.s.rec, SVC_MIN_FIRE, &a) && a.set_flow_min && NEAR(a.flow_min_c, roundf(res->v[0]), 0.01),
          "apply flow_min");

    res = go(&r, SVC_CUTOUT, 3600);
    CHECK(res->state == SVC_R_DONE && res->v[0] >= 4.5f && res->v[0] <= 6.5f, "cut margin %.1f (sim 5)", res->v[0]);
    CHECK(res->v[1] >= 175 && res->v[1] <= 200, "anti-cycle %.0f s (sim 180)", res->v[1]);
    CHECK(res->v[2] > 100 && res->v[2] <= 200 && res->v[3] > 60, "ramp %.0f s to %.0f %% (sim t5 180)", res->v[2], res->v[3]);

    // Mild house, oversized boiler: the burner cuts out
    rig_init(&r, 18);
    warm(&r);
    res = go(&r, SVC_MIN_FIRE, 3 * 3600);
    printf("   (mild day) %s\n", res->text);
    CHECK(res->state != SVC_R_NONE, "finished");
}

static void test_comm_loss_and_safety(void)
{
    rig_t r;
    rig_init(&r, 5);
    warm(&r);
    char why[96];
    CHECK(!svc_start(&r.s, SVC_COMM_LOSS, false, r.t, why, sizeof(why)), "needs confirmation");
    const svc_result_t *res = go(&r, SVC_COMM_LOSS, 1200);
    CHECK(res->state == SVC_R_DONE && res->v[0] == 1 && res->v[2] == 0 && res->v[3] == 31, "E31, locked");
    CHECK(r.flags.status_ca && !r.bus.silent, "talking again, flags restored");
    ot_sim_power_cycle(&r.b);
    r.s.cfg.silence_s = 30;
    res = go(&r, SVC_COMM_LOSS, 1200);
    CHECK(res->v[0] == 0 && res->v[2] == 1, "30 s: no E31, heating works (%s)", res->text);

    // Cancel and abort on a fault
    rig_init(&r, 0);
    warm(&r);
    CHECK(svc_start(&r.s, SVC_SETPOINT, false, r.t, why, sizeof(why)), "start");
    run(&r, 30);
    svc_cancel(&r.s);
    run(&r, 5);
    CHECK(r.s.rec.r[SVC_SETPOINT].state == SVC_R_ABORTED && !r.o->override, "cancelled");
    CHECK(r.flags.mirror_57, "mirror back on after cancel");
    svc_start(&r.s, SVC_MIN_FIRE, false, r.t, why, sizeof(why));
    run(&r, 60);
    ot_sim_set_fault(&r.b, 10);
    run(&r, 60);
    CHECK(r.s.rec.r[SVC_MIN_FIRE].state == SVC_R_ABORTED && strstr(r.s.rec.r[SVC_MIN_FIRE].text, "code 10"),
          "fault abort: %s", r.s.rec.r[SVC_MIN_FIRE].text);
    ot_sim_set_fault(&r.b, 0);

    // Manual frames and the write whitelist
    CHECK(!svc_manual(&r.s, true, 3, 0, r.t, why, sizeof(why)), "write to ID 3 refused");
    CHECK(svc_manual(&r.s, false, 25, 0, r.t, why, sizeof(why)), "read 25");
    run(&r, 4);
    CHECK(strstr(r.o->manual_text, "read ID 25: READ-ACK"), "%s", r.o->manual_text);
    CHECK(svc_manual(&r.s, true, 57, ot_to_f88(55), r.t, why, sizeof(why)), "write 57");
    run(&r, 4);
    CHECK(NEAR(r.b.id57, 55, 0.01), "57 written");

    // Idle timeout
    run_ex(&r, 31 * 60, false);
    CHECK(!r.o->active, "service mode ends by itself");
    CHECK(!svc_start(&r.s, SVC_IDENTITY, false, r.t, why, sizeof(why)), "refused when off");
}

static void test_config(void)
{
    svc_config_t c;
    svc_default_config(&c);
    char v[24];
    CHECK(svc_config_count() == 6 && svc_config_get(&c, 0, v, sizeof(v)) && strcmp(v, "30") == 0, "table");
    CHECK(svc_config_set(&c, "svc_silence_s", "300") && c.silence_s == 300, "set");
    CHECK(!svc_config_set(&c, "svc_silence_s", "5"), "range");
    CHECK(svc_routine_by_name("min_fire") == SVC_MIN_FIRE && svc_routine_by_name("x") < 0, "names");
}

int main(void)
{
    test_identity_and_flags();
    test_setpoint();
    test_ch_off();
    test_id14();
    test_min_fire_and_cutout();
    test_comm_loss_and_safety();
    test_config();
    TEST_DONE();
}
