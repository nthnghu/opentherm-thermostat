#include "ot_sim.h"
#include <math.h>
#include "ot_frame.h"

void ot_sim_default_quirks(ot_sim_quirks_t *q)
{
    *q = (ot_sim_quirks_t){
        .need_ca = true, .heats_to_57 = false, .ignores_ch_off = true, .dhw_set_writable = false,
        .panel_max_ch = 60, .e31_after_s = 60, .miss_pct = 0,
    };
}

void ot_sim_init(ot_sim_t *b, float room_c, float tod_start_h)
{
    *b = (ot_sim_t){0};
    plant_default_params(&b->plant_p);
    plant_init(&b->plant, &b->plant_p, room_c, tod_start_h);
    ot_sim_default_quirks(&b->q);
    b->id1 = 10;
    b->id57 = b->q.panel_max_ch;
    b->dhw_set = b->plant_p.tank_set_c;
    b->rng = 0x12345678;
}

static uint32_t rnd(ot_sim_t *b)
{
    b->rng = b->rng * 1664525u + 1013904223u;
    return b->rng >> 8;
}

float ot_sim_effective_target(const ot_sim_t *b, bool *ch_on)
{
    bool ch = (b->master_flags & OT_M_CH_ENABLE) != 0;
    if (b->q.ignores_ch_off && !ch && b->id1 > 10.5f) {
        ch = true;  // the reported Immergas behaviour: keeps firing to the last setpoint
    }
    if (b->ch_locked) {
        ch = false;
    }
    float t = b->q.heats_to_57 ? (b->id57_written ? b->id57 : b->q.panel_max_ch) : b->id1;
    t = fminf(t, b->q.panel_max_ch);
    if (ch_on) {
        *ch_on = ch;
    }
    return t;
}

static uint32_t ack(uint8_t type, uint8_t id, uint16_t d)
{
    return ot_frame(type == OT_WRITE_DATA ? OT_WRITE_ACK : OT_READ_ACK, id, d);
}

static uint16_t u16(double v)
{
    return v < 0 ? 0 : (v > 65535 ? 65535 : (uint16_t)v);
}

bool ot_sim_respond(ot_sim_t *b, uint32_t req, uint32_t *resp)
{
    if (!ot_parity_ok(req)) {
        return false;  // a real slave ignores a corrupted frame
    }
    if (b->q.miss_pct && rnd(b) % 100 < b->q.miss_pct) {
        b->missed++;
        return false;
    }
    b->frames++;
    b->last_frame_s = b->plant.t_s;
    uint8_t type = ot_type(req), id = ot_id(req);
    uint16_t d = ot_data(req);
    const plant_state_t *s = &b->plant;
    bool writes_ok = !b->q.need_ca || b->ca_seen;
    switch (id) {
    case OT_ID_STATUS: {
        b->master_flags = ot_hb(d);
        b->ca_seen = ot_lb(d) == OT_IMMERGAS_STATUS_LB;
        uint8_t sf = (s->flame ? OT_S_FLAME : 0) | (s->ch_active ? OT_S_CH_ACTIVE : 0) |
                     (s->dhw_active ? OT_S_DHW_ACTIVE : 0) | (b->fault_flags || b->e31 ? OT_S_FAULT : 0);
        *resp = ot_frame(OT_READ_ACK, id, (uint16_t)(b->master_flags << 8) | sf);
        return true;
    }
    case OT_ID_TSET:
        if (type == OT_WRITE_DATA && writes_ok) {
            b->id1 = ot_f88(d);
        }
        *resp = ack(type, id, type == OT_WRITE_DATA ? d : ot_to_f88(b->id1));
        return true;
    case OT_ID_MAXTSET:
        if (type == OT_WRITE_DATA && writes_ok) {
            b->id57 = fminf(ot_f88(d), b->q.panel_max_ch);
            b->id57_written = true;
        }
        *resp = ack(type, id, type == OT_WRITE_DATA ? d : ot_to_f88(b->id57));
        return true;
    case OT_ID_TDHW_SET:
        if (type == OT_WRITE_DATA && writes_ok && b->q.dhw_set_writable) {
            b->dhw_set = ot_f88(d);
            b->plant_p.tank_set_c = b->dhw_set;
        }
        *resp = ack(type, id, type == OT_WRITE_DATA ? d : ot_to_f88(b->dhw_set));
        return true;
    case OT_ID_MAX_REL_MOD:
        if (type == OT_WRITE_DATA && writes_ok && b->q.honours_id14) {
            b->plant_p.max_mod_pct = fminf(fmaxf(ot_f88(d), 0), 100);
        }
        *resp = ack(type, id, d);
        return true;
    case OT_ID_MASTER_CONFIG:
    case OT_ID_MASTER_OT_VERSION:
    case OT_ID_MASTER_VERSION:
    case OT_ID_TROOM:
    case OT_ID_TROOM_SET:
        *resp = ack(type, id, d);
        return true;
    case OT_ID_SLAVE_CONFIG:  // DHW present, modulating, DHW storage tank; member ID 27 (as reported for Immergas)
        *resp = ack(type, id, 0x0900 | 27);
        return true;
    case OT_ID_FAULT:
        *resp = ack(type, id, (uint16_t)((b->fault_flags ? 0x01 : 0) << 8) | (b->e31 ? 31 : b->oem_fault));
        return true;
    case OT_ID_REMOTE_PARAMS:
        *resp = ack(type, id, 0x0303);
        return true;
    case OT_ID_MAX_CAP_MIN_MOD: {
        uint8_t minmod = (uint8_t)lroundf(b->plant_p.pmin_kw / b->plant_p.pmax_kw * 100);
        *resp = ack(type, id, (uint16_t)((uint8_t)b->plant_p.pmax_kw << 8) | minmod);
        return true;
    }
    case OT_ID_REL_MOD:
        *resp = ack(type, id, ot_to_f88(s->modulation));
        return true;
    case OT_ID_CH_PRESSURE:
        *resp = ack(type, id, ot_to_f88(s->pressure_bar));
        return true;
    case OT_ID_TBOILER:
        *resp = ack(type, id, ot_to_f88(s->flow_c));
        return true;
    case OT_ID_TRET:
        *resp = ack(type, id, ot_to_f88(s->return_c));
        return true;
    case OT_ID_TDHW:
        *resp = ack(type, id, ot_to_f88(s->tank_c));
        return true;
    case OT_ID_TDHW_BOUNDS:
        *resp = ack(type, id, (60 << 8) | 30);
        return true;
    case OT_ID_MAXTSET_BOUNDS:
        *resp = ack(type, id, (85 << 8) | 25);
        return true;
    case OT_ID_OEM_DIAG:
        *resp = ack(type, id, 0);
        return true;
    case OT_ID_BURNER_STARTS:
        *resp = ack(type, id, u16(s->starts));
        return true;
    case OT_ID_DHW_BURNER_STARTS:
        *resp = ack(type, id, u16(s->dhw_starts));
        return true;
    case OT_ID_BURNER_HOURS:
        *resp = ack(type, id, u16(s->burner_s / 3600));
        return true;
    case OT_ID_DHW_BURNER_HOURS:
        *resp = ack(type, id, u16(s->dhw_burner_s / 3600));
        return true;
    case OT_ID_SLAVE_OT_VERSION:
        *resp = ack(type, id, ot_to_f88(2.2f));
        return true;
    case OT_ID_SLAVE_VERSION:
        *resp = ack(type, id, 0x0101);
        return true;
    default:  // includes 19 (DHW flow), 27 (no outdoor sensor), 33, the pump counters
        *resp = ot_frame(OT_UNKNOWN_DATA_ID, id, d);
        return true;
    }
}

void ot_sim_tick(ot_sim_t *b, float dt)
{
    if (!b->e31 && b->plant.t_s - b->last_frame_s > b->q.e31_after_s) {
        b->e31 = true;
        b->ch_locked = true;  // the manual: CH is refused until a power cycle with the remote present
    }
    plant_inputs_t in;
    bool ch;
    in.target_c = ot_sim_effective_target(b, &ch);
    in.ch_enable = ch && !b->e31;
    in.dhw_enable = b->e31 ? true : (b->master_flags & OT_M_DHW_ENABLE) != 0;  // local mode: DHW still works
    plant_step(&b->plant, &b->plant_p, &in, dt);
}

void ot_sim_power_cycle(ot_sim_t *b)
{
    b->e31 = false;
    b->ch_locked = b->plant.t_s - b->last_frame_s > 5;  // no remote seen at power-up -> local mode, no CH
    b->master_flags = 0;
    b->ca_seen = false;
    b->id1 = 10;
    b->id57_written = false;
    b->plant.flame = false;
    b->plant.power_kw = 0;
}

void ot_sim_set_fault(ot_sim_t *b, uint8_t code)
{
    b->oem_fault = code;
    b->fault_flags = code ? 1 : 0;
}
