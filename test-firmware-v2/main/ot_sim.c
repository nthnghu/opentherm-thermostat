// A simple simulated OpenTherm boiler, so the thermostat's control loop and frames run without hardware.
// Flow temperature heats towards the control setpoint while the burner is on and cools towards 22 °C.
#include "opentherm.h"

static bool s_ch_enable;
static float s_tset = 10.0f;
static float s_flow = 22.0f;
static float s_mod;
static bool s_flame;

void ot_sim_tick(float dt)
{
    if (s_ch_enable && s_tset > s_flow - 2.0f && s_tset > 25.0f) {
        s_flame = s_flow < s_tset + 1.0f;
    } else {
        s_flame = false;
    }
    if (s_flame) {
        float m = (s_tset - s_flow) * 8.0f + 20.0f;
        s_mod = m < 0 ? 0 : (m > 100 ? 100 : m);
        s_flow += dt * (0.15f + 0.6f * s_mod / 100.0f);
    } else {
        s_mod = 0;
    }
    s_flow -= dt * (s_flow - 22.0f) * 0.01f;
}

uint32_t ot_sim_respond(uint32_t req)
{
    uint8_t type = ot_type(req), id = ot_id(req);
    uint16_t data = ot_data(req);
    switch (id) {
    case OT_ID_STATUS: {
        s_ch_enable = (data >> 8) & 1;
        uint8_t slave = (s_flame && s_ch_enable ? 0x02 : 0) | (s_flame ? 0x08 : 0);
        return ot_frame(OT_READ_ACK, id, (data & 0xFF00) | slave);
    }
    case OT_ID_TSET:
        if (type == OT_WRITE_DATA) {
            s_tset = ot_f88(data);
            return ot_frame(OT_WRITE_ACK, id, data);
        }
        return ot_frame(OT_READ_ACK, id, ot_to_f88(s_tset));
    case OT_ID_SLAVE_CONFIG:
        return ot_frame(OT_READ_ACK, id, 0x0100);  // DHW present, member id 0
    case OT_ID_REL_MOD:
        return ot_frame(OT_READ_ACK, id, ot_to_f88(s_mod));
    case OT_ID_TBOILER:
        return ot_frame(OT_READ_ACK, id, ot_to_f88(s_flow));
    default:
        return ot_frame(OT_UNKNOWN_DATA_ID, id, data);
    }
}
