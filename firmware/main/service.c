#include "service.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "cfgtable.h"
#include "ot_frame.h"

#define ABORT_AFTER_S   10.0     // link lost or fault for this long -> abort
#define X_RETRY_S       10.0     // a one-off frame not done after this -> ask again
#define MAX_RUN_S       (3 * 3600.0)
#define BURN_WAIT_S     900.0    // waiting for a burn to start
#define MIN_MOD_PCT     5.0f

/* ------------------------------------------------------------- config */

#define F(n, t, lo, hi, h) {"svc_" #n, t, offsetof(svc_config_t, n), lo, hi, h}
static const ct_field_t FIELDS[] = {
    F(idle_min, CT_U16, 5, 240, "service mode ends after this long without activity (min)"),
    F(step_min, CT_U16, 3, 30, "setpoint test: length of each ID 1 step (min)"),
    F(opv_max_min, CT_U16, 10, 120, "minimum-fire test: give up after (min)"),
    F(silence_s, CT_U16, 10, 900, "comm-loss test: how long the thermostat stays silent (s)"),
    F(recover_min, CT_U16, 1, 30, "comm-loss test: wait this long for heating after resuming (min)"),
    F(test_flow_c, CT_FLOAT, 35, 60, "flow setpoint used to start a burn in the tests (°C)"),
};
#define NFIELDS ((int)(sizeof(FIELDS) / sizeof(FIELDS[0])))

void svc_default_config(svc_config_t *c)
{
    *c = (svc_config_t){.version = SVC_CONFIG_VERSION, .idle_min = 30, .step_min = 10, .opv_max_min = 40,
                        .silence_s = 120, .recover_min = 5, .test_flow_c = 45};
}

void svc_sanitize(svc_config_t *c)
{
    ct_sanitize(c, FIELDS, NFIELDS);
    c->version = SVC_CONFIG_VERSION;
}

int svc_config_count(void)
{
    return NFIELDS;
}

const char *svc_config_name(int idx)
{
    return idx >= 0 && idx < NFIELDS ? FIELDS[idx].name : NULL;
}

const struct ct_field *svc_config_field(int idx)
{
    return idx >= 0 && idx < NFIELDS ? &FIELDS[idx] : NULL;
}

const char *svc_config_help(int idx)
{
    return idx >= 0 && idx < NFIELDS ? FIELDS[idx].help : NULL;
}

bool svc_config_get(const svc_config_t *c, int idx, char *out, size_t size)
{
    if (idx < 0 || idx >= NFIELDS) {
        return false;
    }
    ct_get(c, &FIELDS[idx], out, size);
    return true;
}

bool svc_config_set(svc_config_t *c, const char *name, const char *value)
{
    return ct_set(c, FIELDS, NFIELDS, name, value);
}

/* ------------------------------------------------------------- names */

static const struct {
    const char *name, *title, *help;
} ROUTINES[SVC_COUNT] = {
    {"identity", "Identity and sensors",
     "Tests 6+16. Reads IDs 3, 125, 127, 15, 6, 48, 49, 93-95 and probes 18, 19, 26-28, 33-36, 115-123. "
     "About 1 min, no burn."},
    {"status_ca", "Status byte 0xCA",
     "Test 4. Writes ID 57 with ID 0 low byte 0x00 and with 0xCA and reads it back. About 30 s, no burn."},
    {"writes", "ID 56/57 writable",
     "Test 8. Writes the tank setpoint (ID 56) and max CH setpoint (ID 57) by a few degrees, reads back, restores. "
     "About 30 s, no burn."},
    {"setpoint", "Which setpoint it heats to",
     "Test 7. ID 57 = 60, then ID 1 = 30, 45, 40 (svc_step_min each); compares the flow. Burns gas, ~30 min."},
    {"ch_off", "CH-off semantics",
     "Tests 9+15. During a burn clears the CH bit with ID 1 kept, then with ID 1 = 10; times the pump post-run. "
     "Burns gas, ~10 min."},
    {"id14", "ID 14 max modulation",
     "Test 10. During a burn writes ID 14 = 30, 100, 0 % and watches ID 17. Burns gas, ~12 min."},
    {"min_fire", "Minimum-fire flow (OPV)",
     "Test 12. Holds TSet = flow - 3 at minimum modulation until the flow is stable. Use a mild day with all "
     "radiators open. Burns gas, up to svc_opv_max_min."},
    {"cutout", "Cut-out, anti-cycle, ramp",
     "Tests 13+14. Lowers TSet below the flow until the burner stops, then asks for heat and times re-ignition "
     "and the ramp to full power. Burns gas, ~15 min."},
    {"comm_loss", "Comm loss (E31)",
     "Tests 18+20. Stays silent for svc_silence_s, then checks E31 and whether heating works again. The boiler "
     "may need a power cycle afterwards. Needs confirmation."},
};

const char *svc_routine_name(svc_routine_t r)
{
    return r < SVC_COUNT ? ROUTINES[r].name : "?";
}

const char *svc_routine_title(svc_routine_t r)
{
    return r < SVC_COUNT ? ROUTINES[r].title : "?";
}

const char *svc_routine_help(svc_routine_t r)
{
    return r < SVC_COUNT ? ROUTINES[r].help : "?";
}

int svc_routine_by_name(const char *name)
{
    for (int i = 0; i < SVC_COUNT; i++) {
        if (strcmp(name, ROUTINES[i].name) == 0) {
            return i;
        }
    }
    return -1;
}

const char *svc_rstate_name(svc_rstate_t s)
{
    static const char *n[] = {"not run", "done", "failed", "aborted"};
    return s <= SVC_R_ABORTED ? n[s] : "?";
}

/* --------------------------------------------------------------- state */

void svc_init(svc_t *s, const svc_config_t *cfg, const svc_record_t *rec)
{
    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;
    svc_sanitize(&s->cfg);
    if (rec && rec->version == SVC_RECORD_VERSION) {
        s->rec = *rec;
        for (int i = 0; i < SVC_COUNT; i++) {
            s->rec.r[i].text[sizeof(s->rec.r[i].text) - 1] = 0;
        }
    } else {
        s->rec.version = SVC_RECORD_VERSION;
    }
    s->running = -1;
}

void svc_set_config(svc_t *s, const svc_config_t *cfg)
{
    s->cfg = *cfg;
    svc_sanitize(&s->cfg);
}

static void touch(svc_t *s, double now_s)
{
    s->active_until = now_s + s->cfg.idle_min * 60.0;
}

bool svc_enter(svc_t *s, double now_s)
{
    s->active = true;
    touch(s, now_s);
    return true;
}

void svc_exit(svc_t *s)
{
    if (s->running >= 0) {
        s->phase = -1;  // finished as aborted in the next step
    }
    s->active = false;
}

bool svc_start(svc_t *s, svc_routine_t r, bool confirmed, double now_s, char *why, size_t size)
{
    if (!s->active) {
        snprintf(why, size, "service mode is off");
        return false;
    }
    if (s->running >= 0 || s->u_pending || s->u_busy) {
        snprintf(why, size, "busy");
        return false;
    }
    if (r >= SVC_COUNT) {
        snprintf(why, size, "unknown routine");
        return false;
    }
    if (r == SVC_COMM_LOSS && !confirmed) {
        snprintf(why, size, "comm_loss needs confirmation: the boiler may lock heating until it is power-cycled");
        return false;
    }
    s->running = r;
    s->phase = 0;
    s->t0 = s->start_s = now_s;
    s->confirmed = confirmed;
    s->x_busy = false;
    s->x_tries = 0;
    s->bad_since = -1;
    s->silent = false;
    s->have = 0;
    memset(s->f, 0, sizeof(s->f));
    memset(s->i, 0, sizeof(s->i));
    memset(s->t, 0, sizeof(s->t));
    s->cmd = (otb_cmd_t){.ch_enable = false, .dhw_enable = false, .tset = 10};
    s->flags_init = false;      // the bus flags are taken from the first step's input
    s->t[0] = -1;               // wait_burn: no flame yet
    touch(s, now_s);
    snprintf(why, size, "started");
    return true;
}

void svc_cancel(svc_t *s)
{
    if (s->running >= 0) {
        s->phase = -1;
    }
}

bool svc_manual(svc_t *s, bool write, uint8_t id, uint16_t data, double now_s, char *why, size_t size)
{
    if (!s->active || s->running >= 0 || s->u_pending || s->u_busy) {
        snprintf(why, size, !s->active ? "service mode is off" : "busy");
        return false;
    }
    if (id >= 128 || (write && id != OT_ID_TSET && id != OT_ID_MAX_REL_MOD && id != OT_ID_TDHW_SET &&
                      id != OT_ID_MAXTSET)) {
        snprintf(why, size, "writes allowed only to IDs 1, 14, 56, 57");
        return false;
    }
    s->u_pending = true;
    s->u_type = write ? OT_WRITE_DATA : OT_READ_DATA;
    s->u_id = id;
    s->u_data = data;
    touch(s, now_s);
    snprintf(why, size, "queued");
    return true;
}

/* ----------------------------------------------------- routine helpers */

static void next(svc_t *s, const svc_in_t *in)
{
    s->phase++;
    s->t0 = in->now_s;
}

static double el(const svc_t *s, const svc_in_t *in)
{
    return in->now_s - s->t0;
}

static void step_text(svc_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void step_text(svc_t *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->out.step, sizeof(s->out.step), fmt, ap);
    va_end(ap);
}

enum { X_WAIT = 0, X_ACK, X_NEG, X_NONE };

// One-off frame: X_WAIT while in progress, then X_ACK (answer in s->x_data), X_NEG (UNKNOWN-DATAID /
// DATA-INVALID) or X_NONE (no answer twice).
static int xchg(svc_t *s, const svc_in_t *in, uint8_t type, uint8_t id, uint16_t data)
{
    svc_out_t *o = &s->out;
    if (!s->x_busy || in->now_s - s->x_t > X_RETRY_S) {
        o->manual = true;
        o->m_type = type;
        o->m_id = id;
        o->m_data = data;
        s->x_busy = true;
        s->x_seq = in->manual->seq;
        s->x_t = in->now_s;
        return X_WAIT;
    }
    if (in->manual->seq == s->x_seq) {
        return X_WAIT;
    }
    s->x_busy = false;
    if (in->manual->result != OTB_XFER_OK) {
        if (++s->x_tries < 2) {
            return X_WAIT;  // asked again in the next step
        }
        s->x_tries = 0;
        return X_NONE;
    }
    s->x_tries = 0;
    uint8_t t = ot_type(in->manual->resp);
    s->x_data = ot_data(in->manual->resp);
    return t == OT_READ_ACK || t == OT_WRITE_ACK ? X_ACK : X_NEG;
}

static bool flame(const svc_in_t *in)
{
    return (in->bus->slave_flags & OT_S_FLAME) != 0;
}

static float flow(const svc_in_t *in)
{
    return in->bus->flow_c;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void finish(svc_t *s, const svc_in_t *in, svc_rstate_t st, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static void finish(svc_t *s, const svc_in_t *in, svc_rstate_t st, const char *fmt, ...)
{
    svc_result_t *r = &s->rec.r[s->running];
    r->state = (uint8_t)st;
    r->sim = in->sim;
    r->epoch = in->epoch;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->text, sizeof(r->text), fmt, ap);
    va_end(ap);
    s->running = -1;
    s->silent = false;
    s->out.record_changed = true;
    s->out.set_flags = true;
    s->out.flags = s->saved_flags;  // the bus flags as before the routine
    touch(s, in->now_s);
}

// Waits for a burn that has lasted min_s. Returns 1 when there, 0 while waiting, -1 on timeout.
static int wait_burn(svc_t *s, const svc_in_t *in, double min_s)
{
    if (!flame(in)) {
        s->t[0] = -1;
    } else if (s->t[0] < 0) {
        s->t[0] = in->now_s;
    }
    if (s->t[0] >= 0 && in->now_s - s->t[0] >= min_s) {
        return 1;
    }
    return el(s, in) > BURN_WAIT_S ? -1 : 0;
}

/* ------------------------------------------------------------ routines */

static const uint8_t IDENT_IDS[] = {3, 125, 127, 15, 6, 48, 49, 93, 94, 95, 18, 19, 26, 27, 28, 33, 34, 35, 36,
                                    115, 116, 117, 118, 119, 120, 121, 122, 123};
#define N_IDENT ((int)sizeof(IDENT_IDS))

// v: member ID, max capacity kW, min modulation %, slave OT version
static void r_identity(svc_t *s, const svc_in_t *in)
{
    if (s->phase < N_IDENT) {
        uint8_t id = IDENT_IDS[s->phase];
        step_text(s, "reading ID %u (%d of %d)", id, s->phase + 1, N_IDENT);
        int r = xchg(s, in, OT_READ_DATA, id, 0);
        if (r == X_WAIT) {
            return;
        }
        if (r == X_ACK) {
            s->have |= 1u << s->phase;
            s->data[s->phase] = s->x_data;
        }
        next(s, in);
        return;
    }
#define HAS(k) ((s->have >> (k)) & 1)
    svc_result_t *res = &s->rec.r[SVC_IDENTITY];
    res->v[0] = HAS(0) ? ot_lb(s->data[0]) : -1;
    res->v[1] = HAS(3) ? ot_hb(s->data[3]) : -1;
    res->v[2] = HAS(3) ? ot_lb(s->data[3]) : -1;
    res->v[3] = HAS(1) ? ot_f88(s->data[1]) : -1;
    char has[80] = "", miss[80] = "";
    size_t nh = 0, nm = 0;
    for (int k = 10; k < N_IDENT; k++) {
        char *b = HAS(k) ? has : miss;
        size_t *n = HAS(k) ? &nh : &nm;
        int w = snprintf(b + *n, sizeof(has) - *n, "%s%u", *n ? " " : "", IDENT_IDS[k]);
        if (w > 0 && *n + (size_t)w < sizeof(has)) {
            *n += (size_t)w;
        }
    }
    char caps[48] = "caps ?";
    if (HAS(3)) {
        snprintf(caps, sizeof(caps), "%u kW, min %u %%", ot_hb(s->data[3]), ot_lb(s->data[3]));
    }
    finish(s, in, HAS(0) ? SVC_R_DONE : SVC_R_FAILED,
           "member %d, OT %.1f, type %d v%d, %s, DHW %d-%d, CH %d-%d. Sensors: %s. Not answered: %s",
           HAS(0) ? ot_lb(s->data[0]) : -1, HAS(1) ? ot_f88(s->data[1]) : 0.0f, HAS(2) ? ot_hb(s->data[2]) : -1,
           HAS(2) ? ot_lb(s->data[2]) : -1, caps, HAS(5) ? (int8_t)ot_lb(s->data[5]) : -1,
           HAS(5) ? (int8_t)ot_hb(s->data[5]) : -1, HAS(6) ? (int8_t)ot_lb(s->data[6]) : -1,
           HAS(6) ? (int8_t)ot_hb(s->data[6]) : -1, nh ? has : "none", nm ? miss : "none");
#undef HAS
}

// v: write works without 0xCA (1/0), with 0xCA (1/0), original ID 57, recommended ca flag (1/0, -1 = no change)
static void r_status_ca(svc_t *s, const svc_in_t *in)
{
    svc_result_t *res = &s->rec.r[SVC_STATUS_CA];
    float *orig = &s->f[0], *tv = &s->f[1];
    int r;
    s->flags.mirror_57 = false;
    switch (s->phase) {
    case 0:
        step_text(s, "reading ID 57");
        if ((r = xchg(s, in, OT_READ_DATA, OT_ID_MAXTSET, 0)) == X_WAIT) {
            return;
        }
        if (r != X_ACK) {
            finish(s, in, SVC_R_FAILED, "ID 57 not supported: repeat with the writes routine (ID 56)");
            return;
        }
        *orig = ot_f88(s->x_data);
        *tv = fabsf(*orig - 50) < 1 ? 45 : 50;
        next(s, in);
        return;
    case 1:
    case 5:
        s->flags.status_ca = s->phase == 5;
        step_text(s, "status low byte %s", s->flags.status_ca ? "0xCA" : "0x00");
        if (el(s, in) >= 3) {
            next(s, in);
        }
        return;
    case 2:
    case 6:
        step_text(s, "writing ID 57 = %.0f with %s", *tv, s->flags.status_ca ? "0xCA" : "0x00");
        if (xchg(s, in, OT_WRITE_DATA, OT_ID_MAXTSET, ot_to_f88(*tv)) != X_WAIT) {
            next(s, in);
        }
        return;
    case 3:
    case 7:
        step_text(s, "reading ID 57 back");
        if ((r = xchg(s, in, OT_READ_DATA, OT_ID_MAXTSET, 0)) == X_WAIT) {
            return;
        }
        s->i[s->phase == 7] = r == X_ACK && fabsf(ot_f88(s->x_data) - *tv) < 0.5f;
        next(s, in);
        return;
    case 4:
    case 8:
        step_text(s, "restoring ID 57 = %.0f", *orig);
        if (xchg(s, in, OT_WRITE_DATA, OT_ID_MAXTSET, ot_to_f88(*orig)) == X_WAIT) {
            return;
        }
        if (s->phase == 4) {
            next(s, in);
            return;
        }
        res->v[0] = s->i[0];
        res->v[1] = s->i[1];
        res->v[2] = *orig;
        if (s->i[1] && !s->i[0]) {
            res->v[3] = 1;
            finish(s, in, SVC_R_DONE, "Writes take effect only with 0xCA: keep flag ca = 1.");
        } else if (s->i[0]) {
            res->v[3] = -1;
            finish(s, in, SVC_R_DONE, "Writes take effect without 0xCA too: ca = 1 is harmless, 0 is standard.");
        } else {
            res->v[3] = -1;
            finish(s, in, SVC_R_DONE, "ID 57 writes never take effect (ACK only). Check P7 = 2 on the boiler.");
        }
        return;
    }
}

static const char *wr_name(int c)
{
    static const char *n[] = {"not supported", "works", "ignored (ACK only)", "rejected"};
    return c >= 0 && c <= 3 ? n[c] : "?";
}

// v: ID 56 result, ID 57 result (0 unsupported, 1 works, 2 ignored, 3 rejected), original 56, original 57
static void r_writes(svc_t *s, const svc_in_t *in)
{
    svc_result_t *res = &s->rec.r[SVC_WRITES];
    s->flags.mirror_57 = false;
    int which = s->phase / 4;  // 0 = ID 56, 1 = ID 57
    uint8_t id = which ? OT_ID_MAXTSET : OT_ID_TDHW_SET;
    float *orig = &s->f[which], *tv = &s->f[2 + which];
    int r;
    if (which > 1) {
        res->v[0] = s->i[0];
        res->v[1] = s->i[1];
        res->v[2] = s->f[0];
        res->v[3] = s->f[1];
        finish(s, in, SVC_R_DONE, "ID 56 (tank setpoint) write: %s. ID 57 (max CH setpoint) write: %s.",
               wr_name(s->i[0]), wr_name(s->i[1]));
        return;
    }
    switch (s->phase % 4) {
    case 0:
        step_text(s, "reading ID %u", id);
        if ((r = xchg(s, in, OT_READ_DATA, id, 0)) == X_WAIT) {
            return;
        }
        if (r != X_ACK) {
            s->i[which] = 0;
            s->phase = (which + 1) * 4;  // skip this ID
            s->t0 = in->now_s;
            return;
        }
        *orig = ot_f88(s->x_data);
        *tv = which ? (*orig > 40 ? *orig - 5 : *orig + 5) : (*orig >= 55 ? *orig - 2 : *orig + 2);
        next(s, in);
        return;
    case 1:
        step_text(s, "writing ID %u = %.0f", id, *tv);
        if ((r = xchg(s, in, OT_WRITE_DATA, id, ot_to_f88(*tv))) == X_WAIT) {
            return;
        }
        if (r != X_ACK) {
            s->i[which] = 3;
            s->phase = (which + 1) * 4;
            s->t0 = in->now_s;
            return;
        }
        next(s, in);
        return;
    case 2:
        step_text(s, "reading ID %u back", id);
        if ((r = xchg(s, in, OT_READ_DATA, id, 0)) == X_WAIT) {
            return;
        }
        s->i[which] = r == X_ACK && fabsf(ot_f88(s->x_data) - *tv) < 0.5f ? 1 : 2;
        next(s, in);
        return;
    case 3:
        step_text(s, "restoring ID %u = %.0f", id, *orig);
        if (xchg(s, in, OT_WRITE_DATA, id, ot_to_f88(*orig)) != X_WAIT) {
            next(s, in);
        }
        return;
    }
}

// v: mean flow at ID 1 = 30, 45, 40; verdict (1 = heats to ID 1, 2 = heats to ID 57, 0 = unclear)
static void r_setpoint(svc_t *s, const svc_in_t *in)
{
    static const float STEPS[3] = {30, 45, 40};
    svc_result_t *res = &s->rec.r[SVC_SETPOINT];
    s->flags.mirror_57 = false;
    if (s->phase == 0) {
        step_text(s, "writing ID 57 = 60");
        int r = xchg(s, in, OT_WRITE_DATA, OT_ID_MAXTSET, ot_to_f88(60));
        if (r == X_WAIT) {
            return;
        }
        if (r != X_ACK) {
            finish(s, in, SVC_R_FAILED, "ID 57 write refused: the test needs ID 57");
            return;
        }
        next(s, in);
        return;
    }
    double step_s = s->cfg.step_min * 60.0;
    int k = s->phase - 1;
    if (k < 3) {
        s->cmd = (otb_cmd_t){.ch_enable = true, .dhw_enable = false, .tset = STEPS[k]};
        step_text(s, "ID 1 = %.0f: flow %.1f, %d s left", STEPS[k], flow(in), (int)(step_s - el(s, in)));
        if (el(s, in) >= step_s - 120) {  // mean over the last 2 minutes of the step
            s->f[k] += flow(in);
            s->f[4 + k] += 1;
        }
        if (el(s, in) >= step_s) {
            next(s, in);
        }
        return;
    }
    float f30 = s->f[0] / fmaxf(s->f[4], 1), f45 = s->f[1] / fmaxf(s->f[5], 1), f40 = s->f[2] / fmaxf(s->f[6], 1);
    res->v[0] = f30;
    res->v[1] = f45;
    res->v[2] = f40;
    if (f30 >= 50) {
        res->v[3] = 2;
        finish(s, in, SVC_R_DONE,
               "Heats to ID 57 (flow %.1f / %.1f / %.1f at ID 1 = 30 / 45 / 40): keep the ID 57 mirror on.", f30,
               f45, f40);
    } else if (f45 - f30 >= 7) {
        res->v[3] = 1;
        finish(s, in, SVC_R_DONE,
               "Heats to ID 1 (flow %.1f / %.1f / %.1f at ID 1 = 30 / 45 / 40): the ID 57 mirror is not needed.",
               f30, f45, f40);
    } else {
        res->v[3] = 0;
        finish(s, in, SVC_R_DONE, "Unclear (flow %.1f / %.1f / %.1f at ID 1 = 30 / 45 / 40). Repeat on a colder day.",
               f30, f45, f40);
    }
}

// v: CH bit alone stops the burner (1/0), ID 1 = 10 stops it (1/0, -1 = not needed), pump post-run s (-1 = ?)
static void r_ch_off(svc_t *s, const svc_in_t *in)
{
    svc_result_t *res = &s->rec.r[SVC_CH_OFF];
    float test = in->flow_max_c;  // far from the cut-out, so a normal cut-out is not mistaken for "off"
    bool ch_active = (in->bus->slave_flags & OT_S_CH_ACTIVE) != 0;
    switch (s->phase) {
    case 0:
        s->cmd = (otb_cmd_t){.ch_enable = true, .dhw_enable = false, .tset = test};
        step_text(s, "waiting for a 2-minute burn (flow %.1f)", flow(in));
        int w = wait_burn(s, in, 120);
        if (w < 0) {
            finish(s, in, SVC_R_FAILED, "No burn within 15 min at TSet %.0f", test);
        } else if (w > 0) {
            s->t[1] = s->t[2] = -1;
            next(s, in);
        }
        return;
    case 1:
    case 2: {
        bool b = s->phase == 2;
        s->flags.explicit_off = b;
        s->cmd = (otb_cmd_t){.ch_enable = false, .dhw_enable = false, .tset = test};  // ID 1 stays 45 in phase 1
        step_text(s, b ? "CH bit cleared and ID 1 = 10: flame %d" : "CH bit cleared, ID 1 kept: flame %d", flame(in));
        if (!flame(in) && s->t[1] < 0) {
            s->t[1] = in->now_s;  // flame off
        }
        if (flame(in)) {
            s->t[1] = -1;
        }
        if (s->t[1] >= 0 && ch_active) {
            s->t[2] = in->now_s;  // CH still active (pump) after the flame went off
        }
        if (el(s, in) < 180) {
            return;
        }
        // Off = the flame went out within 45 s of the change and stayed out
        bool off = s->t[1] >= 0 && s->t[1] - s->t0 <= 45;
        float post = s->t[1] >= 0 ? (s->t[2] >= s->t[1] ? (float)(s->t[2] - s->t[1]) : 0) : -1;
        if (!b) {
            res->v[0] = off;
            res->v[2] = post;
            if (off) {
                res->v[1] = -1;
                finish(s, in, SVC_R_DONE,
                       "Clearing the CH bit stops the burner (ID 1 kept). explicit_off is not needed (harmless). "
                       "Pump post-run about %.0f s.", post);
                return;
            }
            s->t[1] = s->t[2] = -1;
            next(s, in);
            return;
        }
        res->v[1] = off;
        res->v[2] = post;
        if (off) {
            finish(s, in, SVC_R_DONE,
                   "The CH bit alone does NOT stop the burner; with ID 1 = 10 it does: keep explicit_off = 1. "
                   "Pump post-run about %.0f s.", post);
        } else {
            finish(s, in, SVC_R_DONE, "The burner kept running even with ID 1 = 10. Check the boiler settings.");
        }
        return;
    }
    }
}

// v: modulation at ID 14 = 30, 100, 0; honoured (1/0)
static void r_id14(svc_t *s, const svc_in_t *in)
{
    static const float LIM[4] = {30, 100, 0, 100};
    svc_result_t *res = &s->rec.r[SVC_ID14];
    float test = in->flow_max_c;
    s->cmd = (otb_cmd_t){.ch_enable = true, .dhw_enable = false, .tset = test};
    if (s->phase == 0) {
        step_text(s, "waiting for a burn past the ignition ramp (flow %.1f)", flow(in));
        int w = wait_burn(s, in, 200);
        if (w < 0) {
            finish(s, in, SVC_R_FAILED, "No burn within 15 min at TSet %.0f", test);
        } else if (w > 0) {
            next(s, in);
        }
        return;
    }
    int k = (s->phase - 1) / 2;
    bool writing = (s->phase - 1) % 2 == 0;
    if (k < 4 && writing) {
        step_text(s, "writing ID 14 = %.0f %%", LIM[k]);
        int r = xchg(s, in, OT_WRITE_DATA, OT_ID_MAX_REL_MOD, ot_to_f88(LIM[k]));
        if (r == X_WAIT) {
            return;
        }
        if (r != X_ACK) {
            res->v[3] = 0;
            finish(s, in, SVC_R_DONE, "ID 14 is not supported (write refused).");
            return;
        }
        next(s, in);
        return;
    }
    if (k == 3) {
        bool honoured = s->f[1] - s->f[0] >= 15 && s->f[2] <= s->f[0] + 5;
        res->v[0] = s->f[0];
        res->v[1] = s->f[1];
        res->v[2] = s->f[2];
        res->v[3] = honoured;
        finish(s, in, SVC_R_DONE, "ID 14 %s (modulation %.0f / %.0f / %.0f %% at limit 30 / 100 / 0 %%).",
               honoured ? "is honoured" : "is NOT honoured", s->f[0], s->f[1], s->f[2]);
        return;
    }
    step_text(s, "ID 14 = %.0f %%: modulation %.0f %%", LIM[k], in->bus->modulation);
    if (!flame(in)) {
        finish(s, in, SVC_R_FAILED, "The burner stopped during the test (flow %.1f). Repeat on a colder day.",
               flow(in));
        return;
    }
    if (el(s, in) >= 60) {
        s->f[k] += in->bus->modulation;
        s->f[8 + k] += 1;
    }
    if (el(s, in) >= 120) {
        s->f[k] /= fmaxf(s->f[8 + k], 1);
        next(s, in);
    }
}

// v: OPV (stable flow at minimum fire), return, modulation, minutes
static void r_min_fire(svc_t *s, const svc_in_t *in)
{
    svc_result_t *res = &s->rec.r[SVC_MIN_FIRE];
    if (s->phase == 0) {
        s->cmd = (otb_cmd_t){.ch_enable = true, .dhw_enable = false, .tset = clampf(s->cfg.test_flow_c, 25, in->flow_max_c)};
        step_text(s, "waiting for a burn (flow %.1f)", flow(in));
        int w = wait_burn(s, in, 120);
        if (w < 0) {
            finish(s, in, SVC_R_FAILED, "No burn within 15 min");
        } else if (w > 0) {
            s->i[0] = 0;  // samples
            s->t[1] = -1;
            next(s, in);
        }
        return;
    }
    // Hold TSet = flow - 3 (as the minimum-fire hold does), never above the configured maximum
    s->cmd.tset = clampf(flow(in) - 3, 25, in->flow_max_c);
    if (!flame(in)) {
        if (s->t[1] < 0) {
            s->t[1] = in->now_s;
        } else if (in->now_s - s->t[1] > 60) {
            finish(s, in, SVC_R_FAILED,
                   "The burner cut out at flow %.1f: the house takes less than the minimum power now. "
                   "Repeat on a colder day.", flow(in));
            return;
        }
    } else {
        s->t[1] = -1;
    }
    // One sample per minute; stable = 5 samples within 0.6 K at minimum modulation
    double e = el(s, in);
    if (e >= 60.0 * (s->i[0] + 1)) {
        memmove(s->f, s->f + 1, 4 * sizeof(float));
        s->f[4] = flow(in);
        s->i[0]++;
    }
    float lo = 999, hi = -999, mean = 0;
    for (int k = 0; k < 5; k++) {
        lo = fminf(lo, s->f[k]);
        hi = fmaxf(hi, s->f[k]);
        mean += s->f[k] / 5;
    }
    bool at_min = in->bus->modulation <= MIN_MOD_PCT;
    step_text(s, "flow %.1f, modulation %.0f %%, spread %.1f K over 5 min", flow(in), in->bus->modulation,
              s->i[0] >= 5 ? hi - lo : 99.0f);
    if (s->i[0] >= 5 && hi - lo <= 0.6f && at_min) {
        res->v[0] = mean;
        res->v[1] = in->bus->return_c;
        res->v[2] = in->bus->modulation;
        res->v[3] = (float)(e / 60);
        finish(s, in, SVC_R_DONE,
               "Minimum-fire flow %.1f °C (return %.1f, modulation %.0f %%, after %.0f min). "
               "Apply sets flow_min_c = %.0f.", mean, in->bus->return_c, in->bus->modulation, e / 60, roundf(mean));
        return;
    }
    if (e >= s->cfg.opv_max_min * 60.0) {
        finish(s, in, SVC_R_FAILED, "Not stable after %u min (flow %.1f, modulation %.0f %%).", s->cfg.opv_max_min,
               flow(in), in->bus->modulation);
    }
}

// v: cut-out margin K, anti-cycle s, time to the modulation peak s, peak modulation %
static void r_cutout(svc_t *s, const svc_in_t *in)
{
    svc_result_t *res = &s->rec.r[SVC_CUTOUT];
    float hi = fminf(60, in->flow_max_c);
    switch (s->phase) {
    case 0:
        s->cmd = (otb_cmd_t){.ch_enable = true, .dhw_enable = false, .tset = hi};
        step_text(s, "waiting for a 3-minute burn (flow %.1f)", flow(in));
        int w = wait_burn(s, in, 180);
        if (w < 0) {
            finish(s, in, SVC_R_FAILED, "No burn within 15 min");
        } else if (w > 0) {
            s->f[0] = flow(in) - 1;  // TSet, lowered 1 K every 20 s
            s->t[1] = in->now_s;
            next(s, in);
        }
        return;
    case 1:
        if (in->now_s - s->t[1] >= 20) {
            s->f[0] -= 1;
            s->t[1] = in->now_s;
        }
        s->f[0] = fmaxf(s->f[0], 20);
        if (flame(in)) {
            s->f[4] = flow(in);   // last flow and TSet seen while still burning
            s->f[5] = s->cmd.tset;
        }
        s->cmd.tset = s->f[0];
        step_text(s, "lowering TSet: %.0f, flow %.1f", s->f[0], flow(in));
        if (!flame(in)) {
            s->f[1] = s->f[4] - s->f[5];  // margin at the cut-out
            next(s, in);
        } else if (el(s, in) > 900) {
            finish(s, in, SVC_R_FAILED, "The burner did not cut out (TSet %.0f, flow %.1f)", s->f[0], flow(in));
        }
        return;
    case 2:
        s->cmd.tset = hi;
        step_text(s, "asking for %.0f °C: waiting for re-ignition, %d s", hi, (int)el(s, in));
        if (flame(in)) {
            s->f[2] = (float)el(s, in);
            s->f[3] = 0;
            s->f[6] = 0;  // time of the peak
            next(s, in);
        } else if (el(s, in) > 1200) {
            finish(s, in, SVC_R_FAILED, "No re-ignition within 20 min after the cut-out (margin %.1f K)", s->f[1]);
        }
        return;
    case 3:
        // Ramp: time until the modulation stops rising (peak), or reaches 90 %
        if (in->bus->modulation >= s->f[3] + 1) {
            s->f[3] = in->bus->modulation;
            s->f[6] = (float)el(s, in);
        }
        step_text(s, "ramp: modulation %.0f %% after %d s", in->bus->modulation, (int)el(s, in));
        if (in->bus->modulation >= 90 || el(s, in) >= 600 || !flame(in) || (el(s, in) > 60 && el(s, in) - s->f[6] > 60)) {
            res->v[0] = s->f[1];
            res->v[1] = s->f[2];
            res->v[2] = s->f[6];
            res->v[3] = s->f[3];
            finish(s, in, SVC_R_DONE,
                   "Cut-out at TSet + %.1f K; re-ignition after %.0f s (anti-cycle); modulation rose to %.0f %% in %.0f s.",
                   s->f[1], s->f[2], s->f[3], s->f[6]);
        }
        return;
    }
}

// v: E31 seen (1/0), silence s, CH back without a power cycle (1/0), OEM fault code
static void r_comm_loss(svc_t *s, const svc_in_t *in)
{
    svc_result_t *res = &s->rec.r[SVC_COMM_LOSS];
    switch (s->phase) {
    case 0:
        s->silent = true;
        step_text(s, "silent: %d of %u s", (int)el(s, in), s->cfg.silence_s);
        if (el(s, in) >= s->cfg.silence_s) {
            s->silent = false;
            next(s, in);
        }
        return;
    case 1:
        step_text(s, "talking again, waiting for an answer");
        if (in->bus->link == OTB_LINK_UP && in->bus->fails_in_row == 0 && el(s, in) >= 3) {
            next(s, in);
        } else if (el(s, in) > 60) {
            finish(s, in, SVC_R_FAILED, "No answer for 60 s after the silence. Check the boiler display.");
        }
        return;
    case 2: {
        step_text(s, "reading the fault code (ID 5)");
        int r = xchg(s, in, OT_READ_DATA, OT_ID_FAULT, 0);
        if (r == X_WAIT) {
            return;
        }
        bool fault = (in->bus->slave_flags & OT_S_FAULT) != 0;
        s->i[0] = r == X_ACK ? ot_lb(s->x_data) : -1;
        s->i[1] = fault && s->i[0] == 31;
        s->i[2] = fault;
        s->t[0] = -1;
        next(s, in);
        return;
    }
    case 3:
        s->cmd = (otb_cmd_t){.ch_enable = true, .dhw_enable = false, .tset = clampf(s->cfg.test_flow_c, 25, in->flow_max_c)};
        step_text(s, "asking for heat: %d of %u s", (int)el(s, in), s->cfg.recover_min * 60);
        if (flame(in) && !(in->bus->slave_flags & OT_S_DHW_ACTIVE)) {
            s->i[3] = 1;
        }
        if (s->i[3] || el(s, in) >= s->cfg.recover_min * 60.0) {
            res->v[0] = s->i[1];
            res->v[1] = s->cfg.silence_s;
            res->v[2] = s->i[3];
            res->v[3] = s->i[0];
            finish(s, in, SVC_R_DONE, "After %u s of silence: %s. Heating after resuming: %s.", s->cfg.silence_s,
                   s->i[1] ? "E31 (fault 31)" : (s->i[2] ? "a fault, code shown in v[3]" : "no fault"),
                   s->i[3] ? "works without a power cycle"
                           : "LOCKED - power-cycle the boiler (and the fallback must allow for this)");
        }
        return;
    }
}

/* --------------------------------------------------------------- step */

const svc_out_t *svc_step(svc_t *s, const svc_in_t *in)
{
    svc_out_t *o = &s->out;
    o->manual = false;
    o->set_flags = false;
    o->record_changed = false;
    o->override = false;
    o->silent = false;

    if (s->active && s->running < 0 && !s->u_busy && !s->u_pending && in->now_s >= s->active_until) {
        s->active = false;  // idle timeout
    }

    // User manual frame
    if (s->u_pending && !in->manual->pending) {
        o->manual = true;
        o->m_type = s->u_type;
        o->m_id = s->u_id;
        o->m_data = s->u_data;
        s->u_pending = false;
        s->u_busy = true;
        s->u_seq = in->manual->seq;
    } else if (s->u_busy && in->manual->seq != s->u_seq) {
        s->u_busy = false;
        const otb_manual_t *m = in->manual;
        uint8_t t = ot_type(m->resp);
        if (m->result != OTB_XFER_OK) {
            snprintf(o->manual_text, sizeof(o->manual_text), "%s ID %u: no answer", s->u_type == OT_WRITE_DATA ? "write" : "read",
                     s->u_id);
        } else {
            snprintf(o->manual_text, sizeof(o->manual_text), "%s ID %u: %s data 0x%04X (f8.8 %.2f, hb %u, lb %u)",
                     s->u_type == OT_WRITE_DATA ? "write" : "read", s->u_id, ot_type_name(t), ot_data(m->resp),
                     ot_f88(ot_data(m->resp)), ot_hb(ot_data(m->resp)), ot_lb(ot_data(m->resp)));
        }
    }

    o->active = s->active;
    o->idle_left_s = s->active ? (int)fmax(0, s->active_until - in->now_s) : 0;
    if (s->running < 0) {
        o->running = false;
        o->step[0] = 0;
        return o;
    }

    // First step of a routine: remember the bus flags to restore them afterwards
    if (!s->flags_init) {
        s->flags_init = true;
        s->saved_flags = in->flags;
        s->flags = in->flags;
    }
    touch(s, in->now_s);
    o->running = true;
    o->routine = (uint8_t)s->running;
    o->elapsed_s = (int)(in->now_s - s->start_s);

    // Aborts
    if (s->phase < 0) {
        finish(s, in, SVC_R_ABORTED, "Cancelled");
        o->running = false;
        return o;
    }
    bool comm = s->running == SVC_COMM_LOSS;
    bool bad = !comm && (in->bus->link != OTB_LINK_UP || (in->bus->slave_flags & OT_S_FAULT));
    if (bad) {
        if (s->bad_since < 0) {
            s->bad_since = in->now_s;
        } else if (in->now_s - s->bad_since >= ABORT_AFTER_S) {
            if (in->bus->link != OTB_LINK_UP) {
                finish(s, in, SVC_R_ABORTED, "Aborted: the boiler stopped answering");
            } else {
                finish(s, in, SVC_R_ABORTED, "Aborted: boiler fault (code %u)", in->bus->oem_fault);
            }
            o->running = false;
            return o;
        }
    } else {
        s->bad_since = -1;
    }
    if (in->room_ok && in->room_c < in->frost_c) {
        finish(s, in, SVC_R_ABORTED, "Aborted: room below the frost limit (%.1f °C)", in->room_c);
        o->running = false;
        return o;
    }
    if (in->now_s - s->start_s > MAX_RUN_S) {
        finish(s, in, SVC_R_ABORTED, "Aborted: time limit");
        o->running = false;
        return o;
    }

    switch (s->running) {
    case SVC_IDENTITY: r_identity(s, in); break;
    case SVC_STATUS_CA: r_status_ca(s, in); break;
    case SVC_WRITES: r_writes(s, in); break;
    case SVC_SETPOINT: r_setpoint(s, in); break;
    case SVC_CH_OFF: r_ch_off(s, in); break;
    case SVC_ID14: r_id14(s, in); break;
    case SVC_MIN_FIRE: r_min_fire(s, in); break;
    case SVC_CUTOUT: r_cutout(s, in); break;
    case SVC_COMM_LOSS: r_comm_loss(s, in); break;
    default: finish(s, in, SVC_R_FAILED, "unknown routine"); break;
    }
    if (s->running < 0) {
        o->running = false;  // finished this step: flags restored by finish()
        return o;
    }
    o->override = true;
    o->cmd = s->cmd;
    o->set_flags = true;
    o->flags = s->flags;
    o->silent = s->silent;
    return o;
}

/* ------------------------------------------------------------- apply */

bool svc_recommend(const svc_record_t *rec, svc_routine_t r, svc_apply_t *a)
{
    memset(a, 0, sizeof(*a));
    if (r >= SVC_COUNT || rec->r[r].state != SVC_R_DONE) {
        return false;
    }
    const float *v = rec->r[r].v;
    switch (r) {
    case SVC_STATUS_CA:
        if (v[1] && !v[0]) {
            a->set_ca = true, a->ca = true;
        } else if (v[0]) {
            a->set_ca = true, a->ca = false;
        }
        break;
    case SVC_SETPOINT:
        if (v[3] == 2) {
            a->set_mirror = true, a->mirror = true;
        } else if (v[3] == 1) {
            a->set_mirror = true, a->mirror = false;
        }
        break;
    case SVC_CH_OFF:
        a->set_explicit_off = true;
        a->explicit_off = !v[0];
        break;
    case SVC_MIN_FIRE:
        a->set_flow_min = true;
        a->flow_min_c = roundf(v[0]);
        break;
    default:
        break;
    }
    return a->set_ca || a->set_mirror || a->set_explicit_off || a->set_flow_min;
}
