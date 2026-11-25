#include "otb_core.h"
#include <string.h>
#include <math.h>
#include "ot_frame.h"

// ID 17 relative modulation. The Victrix Tera sends a negative f8.8 (-113 %) while it burns at its minimum flame
// (flow steady at the minimum-fire temperature, 2026-11-25): taken as 0 %. Above 100 % is capped (some boilers go
// past 100 during DHW). The raw word stays in mod_raw / mod_odd_raw for checking.
// ID 33 flue gas: the spec has a signed integer (°C); the Victrix Tera sends f8.8 (0x2E00 = 46 °C, seen
// 2026-11-25), which read as s16 would be 11776 °C. Outside -40..250 °C as s16 means f8.8.
float otb_decode_exhaust(uint16_t d)
{
    int16_t s = (int16_t)d;
    return (s >= -40 && s <= 250) ? (float)s : ot_f88(d);
}

float otb_decode_mod(uint16_t d)
{
    float f = ot_f88(d);
    return f < 0 ? 0 : (f > 100 ? 100 : f);
}

#define NEVER INT64_MIN
#define NEG_FOR_UNSUPPORTED 3
#define REPROBE_MS (3600LL * 1000)
#define MIRROR_57_REFRESH_MS (60LL * 1000)
#define DHWSET_RETRY_MS (600LL * 1000)  // rewrite ID 56 when the boiler shows another value (or ignored it)
#define BURNING_PERIOD_MS (30LL * 1000)  // flue gas, fan and flame current while the flame is on
#define CONFIRM_MS (5LL * 1000)  // re-ask soon after a negative answer, to settle support quickly

static const struct {
    uint8_t id;
    bool write;
    uint32_t period_s;
} SLOT_TABLE[] = {
    // Live values first, so they are known soon after a (re)connect
    {OT_ID_TBOILER, false, 4},
    {OT_ID_REL_MOD, false, 4},
    {OT_ID_TRET, false, 10},
    // Configuration record (also refreshed hourly)
    {OT_ID_SLAVE_CONFIG, false, 3600},
    {OT_ID_MASTER_CONFIG, true, 3600},
    {OT_ID_MASTER_OT_VERSION, true, 3600},
    {OT_ID_SLAVE_OT_VERSION, false, 3600},
    {OT_ID_SLAVE_VERSION, false, 3600},
    {OT_ID_MAX_CAP_MIN_MOD, false, 3600},
    {OT_ID_REMOTE_PARAMS, false, 3600},
    {OT_ID_TDHW_BOUNDS, false, 3600},
    {OT_ID_MAXTSET_BOUNDS, false, 3600},
    // Slower values and optional sensors
    {OT_ID_TDHW, false, 30},
    {OT_ID_CH_PRESSURE, false, 60},
    {OT_ID_TOUTSIDE, false, 60},
    {OT_ID_FAULT, false, 10},  // only while the fault bit is set
    {OT_ID_TDHW_SET, false, 300},
    {OT_ID_TEXHAUST, false, 300},   // every BURNING_PERIOD_MS while the flame is on
    {OT_ID_FAN_SPEED, false, 600},  // likewise
    {OT_ID_FLAME_CURRENT, false, 600},  // likewise
    {OT_ID_DHW_FLOW, false, 300},
    {OT_ID_MAXTSET, true, 0},  // the ID 57 mirror, scheduled separately
    {OT_ID_TDHW_SET, true, 0}, // ID 56 write (only with cmd.dhw_set), scheduled separately
    // Counters, hourly
    {OT_ID_BURNER_STARTS, false, 3600},
    {OT_ID_CH_PUMP_STARTS, false, 3600},
    {OT_ID_DHW_PUMP_STARTS, false, 3600},
    {OT_ID_DHW_BURNER_STARTS, false, 3600},
    {OT_ID_BURNER_HOURS, false, 3600},
    {OT_ID_CH_PUMP_HOURS, false, 3600},
    {OT_ID_DHW_PUMP_HOURS, false, 3600},
    {OT_ID_DHW_BURNER_HOURS, false, 3600},
};

const char *otb_link_name(otb_link_t l)
{
    return l == OTB_LINK_UP ? "up" : (l == OTB_LINK_LOST ? "lost" : "unknown");
}

bool otb_cmd_guard(otb_cmd_t *cmd, int64_t age_ms)
{
    if (age_ms <= OTB_CMD_MAX_AGE_MS) {
        return false;
    }
    cmd->ch_enable = false;  // hot water stays as it was: the boiler limits the tank by itself
    cmd->tset = 10;
    return true;
}

void otb_init(otb_core_t *c)
{
    memset(c, 0, sizeof(*c));
    c->flags = (otb_flags_t){.status_ca = true, .mirror_57 = true, .explicit_off = true};
    c->cmd = (otb_cmd_t){.ch_enable = false, .dhw_enable = true, .tset = 10};
    c->nslots = (int)(sizeof(SLOT_TABLE) / sizeof(SLOT_TABLE[0]));
    for (int i = 0; i < c->nslots; i++) {
        c->slots[i] = (otb_slot_t){.id = SLOT_TABLE[i].id, .write = SLOT_TABLE[i].write,
                                   .period_ms = SLOT_TABLE[i].period_s * 1000u, .last_ms = NEVER};
    }
    c->last_reprobe_ms = NEVER;
    c->last_57_ms = NEVER;
    c->last_57_sent = -100;
    c->dhw_write_ms = NEVER;
    c->st.win_minute = -1;
}

// Moves the 5-minute window of frame results to the minute of now (empty buckets for the minutes passed)
static void window_roll(otb_status_t *st, int64_t now)
{
    int64_t m = now / 60000;
    if (st->win_minute < 0 || m - st->win_minute >= 5 || m < st->win_minute) {
        memset(st->win_frames, 0, sizeof(st->win_frames));
        memset(st->win_fails, 0, sizeof(st->win_fails));
    } else {
        for (int64_t k = st->win_minute + 1; k <= m; k++) {
            st->win_frames[k % 5] = 0;
            st->win_fails[k % 5] = 0;
        }
    }
    st->win_minute = m;
}

static void window_sum(otb_status_t *st)
{
    uint32_t f = 0, e = 0;
    for (int i = 0; i < 5; i++) {
        f += st->win_frames[i];
        e += st->win_fails[i];
    }
    st->frames_5m = (uint16_t)(f > 0xFFFF ? 0xFFFF : f);
    st->fails_5m = (uint16_t)(e > 0xFFFF ? 0xFFFF : e);
}

void otb_new_boiler(otb_core_t *c)
{
    otb_flags_t flags = c->flags;
    otb_cmd_t cmd = c->cmd;
    otb_manual_t manual = c->manual;
    bool silent = c->silent;
    otb_init(c);
    c->flags = flags;
    c->cmd = cmd;
    c->manual = manual;
    c->silent = silent;
}

float otb_tset_for(const otb_core_t *c)
{
    float t = c->cmd.tset;
    if (!c->cmd.ch_enable && c->flags.explicit_off) {
        t = 10;
    }
    return fminf(fmaxf(t, 10), 90);
}

static void on_connect(otb_core_t *c, int64_t now)
{
    for (int i = 0; i < c->nslots; i++) {
        c->slots[i].last_ms = NEVER;
    }
    for (int i = 0; i < 128; i++) {
        if (c->support[i] == OTB_ID_UNSUPPORTED) {
            c->support[i] = OTB_ID_UNKNOWN;
        }
        c->neg[i] = 0;
    }
    c->last_57_ms = NEVER;
    c->dhw_write_ms = NEVER;  // a power-cycled boiler may be back on its panel value
    c->dhw_readback_due = false;
    c->last_reprobe_ms = now;
}

static void record_latency(otb_status_t *st, uint16_t l)
{
    st->last_latency_ms = l;
    if (l > st->max_latency_ms) {
        st->max_latency_ms = l;
    }
    st->latency_sum_ms += l;
    st->latency_n++;
}

// Sends one frame. Returns the result; *resp valid when OTB_XFER_OK.
static otb_xfer_result_t send(otb_core_t *c, otb_xfer_fn xfer, void *ctx, uint8_t type, uint8_t id, uint16_t data,
                              uint32_t *resp)
{
    uint16_t lat = 0;
    otb_xfer_result_t r = xfer(ctx, ot_frame(type, id, data), resp, &lat);
    if (r == OTB_XFER_SKIP) {
        return r;
    }
    c->st.frames++;
    if (r == OTB_XFER_OK && ot_id(*resp) != id) {
        r = OTB_XFER_BAD;
    }
    if (c->st.win_minute >= 0) {
        int b = (int)(c->st.win_minute % 5);
        if (c->st.win_frames[b] < 0xFFFF) {
            c->st.win_frames[b]++;
            c->st.win_fails[b] += r != OTB_XFER_OK;
        }
        window_sum(&c->st);
    }
    switch (r) {
    case OTB_XFER_OK:
        c->st.ok++;
        record_latency(&c->st, lat);
        break;
    case OTB_XFER_TIMEOUT:
        c->st.timeouts++;
        break;
    default:
        c->st.bad++;
        break;
    }
    return r;
}

static void note_negative(otb_core_t *c, uint8_t id)
{
    if (id >= 128) {
        return;
    }
    if (c->neg[id] < 255) {
        c->neg[id]++;
    }
    if (c->neg[id] >= NEG_FOR_UNSUPPORTED) {
        c->support[id] = OTB_ID_UNSUPPORTED;
    }
}

static int8_t s8(uint8_t v)
{
    return (int8_t)v;
}

static void parse(otb_core_t *c, uint8_t id, uint16_t d)
{
    otb_status_t *st = &c->st;
    switch (id) {
    case OT_ID_TBOILER: st->flow_c = ot_f88(d); st->valid |= OTB_V_FLOW; break;
    case OT_ID_TRET: st->return_c = ot_f88(d); st->valid |= OTB_V_RET; break;
    case OT_ID_REL_MOD: st->mod_raw = d; st->modulation = otb_decode_mod(d); st->valid |= OTB_V_MOD;
        if (ot_f88(d) < 0 || ot_f88(d) > 100) {
            st->mod_odd_raw = d;
            st->mod_odd++;
        }
        break;
    case OT_ID_CH_PRESSURE: st->pressure_bar = ot_f88(d); st->valid |= OTB_V_PRESS; break;
    case OT_ID_TDHW: st->tank_c = ot_f88(d); st->valid |= OTB_V_TDHW; break;
    case OT_ID_TOUTSIDE: st->outside_c = ot_f88(d); st->valid |= OTB_V_TOUT; break;
    case OT_ID_TEXHAUST: st->exhaust_c = otb_decode_exhaust(d); st->valid |= OTB_V_TEXH; break;
    case OT_ID_FAN_SPEED: st->fan_set_hz = ot_hb(d); st->fan_hz = ot_lb(d); st->valid |= OTB_V_FAN; break;
    case OT_ID_FLAME_CURRENT: st->flame_ua = ot_f88(d); st->valid |= OTB_V_FLAME_I; break;
    case OT_ID_DHW_FLOW: st->dhw_flow = ot_f88(d); st->valid |= OTB_V_DHW_FLOW; break;
    case OT_ID_FAULT: st->fault_flags = ot_hb(d); st->oem_fault = ot_lb(d); st->valid |= OTB_V_FAULT; break;
    case OT_ID_SLAVE_CONFIG: st->slave_config = ot_hb(d); st->member_id = ot_lb(d); st->valid |= OTB_V_CONFIG; break;
    case OT_ID_MAX_CAP_MIN_MOD: st->max_cap_kw = ot_hb(d); st->min_mod_pct = ot_lb(d); st->valid |= OTB_V_CAPS; break;
    case OT_ID_REMOTE_PARAMS: st->remote_params = d; st->valid |= OTB_V_REMOTE; break;
    case OT_ID_TDHW_BOUNDS: st->dhw_max = s8(ot_hb(d)); st->dhw_min = s8(ot_lb(d)); st->valid |= OTB_V_DHW_BOUNDS; break;
    case OT_ID_MAXTSET_BOUNDS: st->ch_max = s8(ot_hb(d)); st->ch_min = s8(ot_lb(d)); st->valid |= OTB_V_CH_BOUNDS; break;
    case OT_ID_TDHW_SET: st->dhw_set_c = ot_f88(d); st->valid |= OTB_V_DHW_SET; break;
    case OT_ID_MAXTSET: st->maxset_c = ot_f88(d); st->valid |= OTB_V_MAXSET; break;
    case OT_ID_SLAVE_OT_VERSION: st->slave_ot_version = ot_f88(d); st->valid |= OTB_V_VERSION; break;
    case OT_ID_SLAVE_VERSION: st->slave_type = ot_hb(d); st->slave_version = ot_lb(d); st->valid |= OTB_V_VERSION; break;
    default:
        if (id >= OT_ID_BURNER_STARTS && id <= OT_ID_DHW_BURNER_HOURS) {
            st->counters[id - OT_ID_BURNER_STARTS] = d;
            st->valid |= OTB_V_COUNTERS;
        }
        break;
    }
}

static uint16_t write_value(const otb_core_t *c, uint8_t id)
{
    switch (id) {
    case OT_ID_MASTER_CONFIG: return 0x0000;  // no master flags, member ID 0
    case OT_ID_MASTER_OT_VERSION: return ot_to_f88(2.2f);
    case OT_ID_MAXTSET: return ot_to_f88(otb_tset_for(c));
    case OT_ID_TDHW_SET: return ot_to_f88(c->cmd.dhw_set);
    default: return 0;
    }
}

static bool slot_eligible(const otb_core_t *c, const otb_slot_t *s)
{
    if (c->support[s->id] == OTB_ID_UNSUPPORTED) {
        return false;
    }
    if (s->id == OT_ID_FAULT) {
        return (c->st.slave_flags & OT_S_FAULT) != 0;
    }
    return s->id != OT_ID_MAXTSET && !(s->write && s->id == OT_ID_TDHW_SET);  // chosen separately
}

static otb_slot_t *find_slot(otb_core_t *c, uint8_t id, bool write)
{
    for (int i = 0; i < c->nslots; i++) {
        if (c->slots[i].id == id && c->slots[i].write == write) {
            return &c->slots[i];
        }
    }
    return NULL;
}

// ID 56: write when the wanted value changed, after a (re)connect, or (every 10 min) when the boiler shows
// another value; read it back in the next cycle.
static otb_slot_t *pick_dhwset(otb_core_t *c, int64_t now)
{
    if (c->cmd.dhw_set <= 0 || c->support[OT_ID_TDHW_SET] == OTB_ID_UNSUPPORTED) {
        return NULL;
    }
    if (c->dhw_readback_due) {
        return find_slot(c, OT_ID_TDHW_SET, false);
    }
    float want = c->cmd.dhw_set;
    bool changed = fabsf(want - c->st.dhw_set_written) >= 0.5f;
    bool differs = (c->st.valid & OTB_V_DHW_SET) && fabsf(want - c->st.dhw_set_c) >= 0.5f;
    if (c->dhw_write_ms == NEVER || changed ||
        ((differs || c->st.dhw_set_verify == OTB_DHWSET_REJECTED) && now - c->dhw_write_ms >= DHWSET_RETRY_MS)) {
        return find_slot(c, OT_ID_TDHW_SET, true);
    }
    return NULL;
}

static otb_slot_t *pick_slot(otb_core_t *c, int64_t now)
{
    // 1. The ID 57 mirror when the value changed or is due for a refresh.
    if (c->flags.mirror_57 && c->support[OT_ID_MAXTSET] != OTB_ID_UNSUPPORTED) {
        float v = otb_tset_for(c);
        if (c->last_57_ms == NEVER || fabsf(v - c->last_57_sent) >= 0.5f || now - c->last_57_ms >= MIRROR_57_REFRESH_MS) {
            for (int i = 0; i < c->nslots; i++) {
                if (c->slots[i].id == OT_ID_MAXTSET) {
                    return &c->slots[i];
                }
            }
        }
    }
    otb_slot_t *d = pick_dhwset(c, now);
    if (d) {
        return d;
    }
    // 2. Never-sent slots in table order, then the most overdue one.
    otb_slot_t *best = NULL;
    int64_t best_over = -1;
    for (int i = 0; i < c->nslots; i++) {
        otb_slot_t *s = &c->slots[i];
        if (!slot_eligible(c, s)) {
            continue;
        }
        if (s->last_ms == NEVER) {
            return s;
        }
        int64_t period = (c->neg[s->id] && c->support[s->id] == OTB_ID_UNKNOWN) ? CONFIRM_MS : (int64_t)s->period_ms;
        // Burner values: often while it burns (the flue gas tells whether it condenses), rarely otherwise
        bool burning_id = s->id == OT_ID_TEXHAUST || s->id == OT_ID_FAN_SPEED || s->id == OT_ID_FLAME_CURRENT;
        if (burning_id && (c->st.slave_flags & OT_S_FLAME) && period > BURNING_PERIOD_MS) {
            period = BURNING_PERIOD_MS;
        }
        int64_t over = now - s->last_ms - period;
        if (over >= 0 && over > best_over) {
            best_over = over;
            best = s;
        }
    }
    return best;
}

bool otb_manual_request(otb_core_t *c, uint8_t type, uint8_t id, uint16_t data)
{
    if (c->manual.pending || id >= 128 || (type != OT_READ_DATA && type != OT_WRITE_DATA)) {
        return false;
    }
    c->manual.type = type;
    c->manual.id = id;
    c->manual.data = data;
    c->manual.pending = true;
    return true;
}

// The one-off frame of this cycle. Reads update the values and the support map like a slot would.
static void run_manual(otb_core_t *c, otb_xfer_fn xfer, void *ctx)
{
    otb_manual_t *m = &c->manual;
    uint32_t resp = 0;
    otb_xfer_result_t r = send(c, xfer, ctx, m->type, m->id, m->data, &resp);
    if (r == OTB_XFER_SKIP) {
        return;  // stays pending for the next cycle
    }
    m->pending = false;
    m->result = (uint8_t)r;
    m->resp = resp;
    m->seq++;
    if (r != OTB_XFER_OK) {
        return;
    }
    uint8_t t = ot_type(resp);
    if (t == OT_READ_ACK || t == OT_WRITE_ACK) {
        c->support[m->id] = OTB_ID_SUPPORTED;
        c->neg[m->id] = 0;
        if (t == OT_READ_ACK) {
            parse(c, m->id, ot_data(resp));
        }
    } else if (t == OT_UNKNOWN_DATA_ID) {
        c->st.unknown_id++;
    } else if (t == OT_DATA_INVALID) {
        c->st.data_invalid++;
    }
}

void otb_cycle(otb_core_t *c, int64_t now, otb_xfer_fn xfer, void *ctx)
{
    otb_status_t *st = &c->st;
    st->cycles++;
    window_roll(st, now);
    window_sum(st);
    if (c->silent) {
        return;  // nothing on the line (service comm-loss test); the link state is judged when talking again
    }

    // Hourly re-probe of unsupported IDs
    if (c->last_reprobe_ms != NEVER && now - c->last_reprobe_ms >= REPROBE_MS) {
        c->last_reprobe_ms = now;
        for (int i = 0; i < 128; i++) {
            if (c->support[i] == OTB_ID_UNSUPPORTED) {
                c->support[i] = OTB_ID_UNKNOWN;
                c->neg[i] = 0;
            }
        }
    }

    // 1. Status (retried once)
    uint8_t mflags = (c->cmd.ch_enable ? OT_M_CH_ENABLE : 0) | (c->cmd.dhw_enable ? OT_M_DHW_ENABLE : 0);
    uint16_t sdata = (uint16_t)(mflags << 8) | (c->flags.status_ca ? OT_IMMERGAS_STATUS_LB : 0);
    uint32_t resp = 0;
    otb_xfer_result_t r = send(c, xfer, ctx, OT_READ_DATA, OT_ID_STATUS, sdata, &resp);
    if (r == OTB_XFER_SKIP) {
        return;
    }
    bool status_ok = r == OTB_XFER_OK && ot_type(resp) == OT_READ_ACK;
    if (!status_ok) {
        st->retries++;
        r = send(c, xfer, ctx, OT_READ_DATA, OT_ID_STATUS, sdata, &resp);
        status_ok = r == OTB_XFER_OK && ot_type(resp) == OT_READ_ACK;
    }
    if (!status_ok) {
        st->fails_in_row++;
        if (st->fails_in_row >= OTB_FAILS_FOR_LOST && st->link != OTB_LINK_LOST) {
            if (st->link == OTB_LINK_UP) {
                st->link_losses++;
            }
            st->link = OTB_LINK_LOST;
        }
        return;  // keep the cadence (status every cycle) but don't spend the cycle on more timeouts
    }
    st->fails_in_row = 0;
    st->slave_flags = ot_lb(ot_data(resp));
    if (st->link != OTB_LINK_UP) {
        st->link = OTB_LINK_UP;
        on_connect(c, now);
    }
    if (c->last_reprobe_ms == NEVER) {
        c->last_reprobe_ms = now;
    }

    // 2. Control setpoint
    float tset = otb_tset_for(c);
    uint16_t tdata = ot_to_f88(tset);
    r = send(c, xfer, ctx, OT_WRITE_DATA, OT_ID_TSET, tdata, &resp);
    if (r == OTB_XFER_SKIP) {
        return;
    }
    st->tset_sent = tset;
    st->tset_acked = r == OTB_XFER_OK && ot_type(resp) == OT_WRITE_ACK && ot_data(resp) == tdata;

    // 3. A one-off frame (service), else one scheduled slot
    if (c->manual.pending) {
        run_manual(c, xfer, ctx);
        return;
    }
    otb_slot_t *s = pick_slot(c, now);
    if (!s) {
        return;
    }
    uint16_t wdata = s->write ? write_value(c, s->id) : 0;
    r = send(c, xfer, ctx, s->write ? OT_WRITE_DATA : OT_READ_DATA, s->id, wdata, &resp);
    if (r == OTB_XFER_SKIP) {
        return;
    }
    s->last_ms = now;
    if (s->id == OT_ID_MAXTSET) {
        c->last_57_ms = now;
        c->last_57_sent = otb_tset_for(c);
    }
    bool dhw_write = s->write && s->id == OT_ID_TDHW_SET;
    if (dhw_write) {
        c->dhw_write_ms = now;
        st->dhw_set_written = c->cmd.dhw_set;
    }
    if (r != OTB_XFER_OK) {
        note_negative(c, s->id);
        return;  // a lost write is retried (dhw_set_written vs the read-back), a lost read-back stays due
    }
    uint8_t t = ot_type(resp);
    if (t == OT_READ_ACK || t == OT_WRITE_ACK) {
        c->support[s->id] = OTB_ID_SUPPORTED;
        c->neg[s->id] = 0;
        if (dhw_write) {
            c->dhw_readback_due = true;  // the ACK echoes our value; only a read shows what the boiler kept
        } else {
            parse(c, s->id, ot_data(resp));
        }
        if (!s->write && s->id == OT_ID_TDHW_SET && c->dhw_readback_due) {
            c->dhw_readback_due = false;
            st->dhw_set_verify = fabsf(st->dhw_set_c - st->dhw_set_written) < 0.5f ? OTB_DHWSET_OK : OTB_DHWSET_IGNORED;
        }
    } else {
        if (dhw_write) {
            st->dhw_set_verify = OTB_DHWSET_REJECTED;
        }
        if (t == OT_UNKNOWN_DATA_ID) {
            st->unknown_id++;
        } else if (t == OT_DATA_INVALID) {
            st->data_invalid++;
        }
        note_negative(c, s->id);
    }
}
