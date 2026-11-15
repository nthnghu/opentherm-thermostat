#include "datalog.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TERA_CAP_KW 24.0f
#define TERA_MIN_KW 4.3f

static int16_t c10(double v)
{
    return (int16_t)lround(fmin(fmax(v * 10, -32000), 32000));
}

static uint8_t u8c(double v)
{
    return (uint8_t)lround(fmin(fmax(v, 0), 255));
}

float datalog_power_kw(float mod_pct, float cap_kw, float min_kw)
{
    if (cap_kw <= 0) {
        cap_kw = TERA_CAP_KW;
        min_kw = TERA_MIN_KW;
    }
    if (min_kw <= 0 || min_kw > cap_kw) {
        min_kw = cap_kw * 0.18f;
    }
    float m = fminf(fmaxf(mod_pct, 0), 100);
    return min_kw + (cap_kw - min_kw) * m / 100;
}

void datalog_init(dl_t *d, dl_rec_t *recs, int nrecs, dl_burn_t *burns, int nburns)
{
    memset(d, 0, sizeof(*d));
    d->recs = recs;
    d->nrecs = nrecs;
    d->burns = burns;
    d->nburns = nburns;
}

static void reset_minute(dl_t *d)
{
    d->n = 0;
    d->sum_room = d->sum_hum = d->sum_target = d->sum_flow = d->sum_ret = d->sum_tank = d->sum_out = 0;
    d->sum_press = d->mod_sum = 0;
    d->n_room = d->n_flow = d->n_ret = d->n_tank = d->n_out = d->n_press = 0;
    d->flame_s = d->dhw_s = d->cond_s = d->min_s = d->ch_starts = d->dhw_starts = 0;
    d->wh = 0;
}

static void end_burn(dl_t *d)
{
    if (d->burns && d->nburns > 0) {
        uint32_t dur = d->sec - d->burn_start;
        dl_burn_t b = {
            .start_s = d->burn_start,
            .dur_s = (uint16_t)(dur > 65535 ? 65535 : dur),
            .dhw = d->burn_dhw,
            .ret_c = d->burn_n ? u8c(d->burn_ret / d->burn_n) : 0,
            .mod = d->burn_n ? u8c(d->burn_mod / d->burn_n) : 0,
        };
        d->burns[d->bhead] = b;
        d->bhead = (d->bhead + 1) % d->nburns;
        if (d->bcount < d->nburns) {
            d->bcount++;
        }
    }
    d->flame = false;
}

bool datalog_tick(dl_t *d, const dl_sample_t *s)
{
    d->sec++;
    if (!d->have_err0) {
        d->ot_err0 = s->ot_errors;
        d->have_err0 = true;
    }

    // Burns (edges on the flame bit); a CH burn that turns into a tank burn counts as DHW
    if (s->flame && !d->flame) {
        d->flame = true;
        d->burn_start = d->sec;
        d->burn_dhw = s->dhw_active;
        d->burn_ret = d->burn_mod = 0;
        d->burn_n = 0;
        if (s->dhw_active) {
            d->dhw_starts++;
            d->total_dhw_starts++;
        } else {
            d->ch_starts++;
            d->total_ch_starts++;
        }
    } else if (!s->flame && d->flame) {
        end_burn(d);
    }
    if (d->flame) {
        d->burn_dhw |= s->dhw_active;
        if (s->ret_valid) {
            d->burn_ret += s->return_c;
        }
        d->burn_mod += s->mod_valid ? s->modulation : 0;
        d->burn_n++;
    }

    // Minute sums
    d->n++;
    if (s->sensor_ok) {
        d->sum_room += s->room_c;
        d->sum_hum += s->humidity;
        d->n_room++;
    }
    d->sum_target += s->target_c;
    if (s->flow_valid) {
        d->sum_flow += s->flow_c, d->n_flow++;
    }
    if (s->ret_valid) {
        d->sum_ret += s->return_c, d->n_ret++;
    }
    if (s->tank_valid) {
        d->sum_tank += s->tank_c, d->n_tank++;
    }
    if (s->out_valid) {
        d->sum_out += s->outside_c, d->n_out++;
    }
    if (s->press_valid) {
        d->sum_press += s->pressure_bar, d->n_press++;
    }
    if (s->flame) {
        float mod = s->mod_valid ? fminf(fmaxf(s->modulation, 0), 100) : 0;  // DHW may report > 100 %
        d->flame_s++;
        d->mod_sum += mod;
        if (s->dhw_active) {
            d->dhw_s++;
        } else {
            if (s->ret_valid && s->return_c < DL_COND_RETURN_C) {
                d->cond_s++;
            }
            if (mod <= DL_MIN_MOD_PCT) {
                d->min_s++;
            }
        }
        double kw = datalog_power_kw(mod, s->cap_kw, s->min_kw);
        d->wh += kw / 3.6;  // kW over 1 s = kW/3600 kWh = kW/3.6 Wh
        d->total_kwh += kw / 3600.0;
        d->total_burner_s++;
    }
    if (d->n < 60) {
        return false;
    }

    // Close the minute
    if (d->recs && d->nrecs > 0) {
        uint32_t err = s->ot_errors - d->ot_err0;
        d->ot_err0 = s->ot_errors;
        dl_rec_t r = {
            .t = s->epoch,
            .up_min = d->sec / 60,
            .room_c100 = d->n_room ? (int16_t)lround(d->sum_room / d->n_room * 100) : DL_NO_VALUE,
            .target_c10 = c10(d->sum_target / d->n),
            .setpoint_c10 = c10(s->setpoint_c),
            .tset_c10 = c10(s->tset_c),
            .req_c10 = c10(s->req_c),
            .flow_c10 = d->n_flow ? c10(d->sum_flow / d->n_flow) : DL_NO_VALUE,
            .ret_c10 = d->n_ret ? c10(d->sum_ret / d->n_ret) : DL_NO_VALUE,
            .tank_c10 = d->n_tank ? c10(d->sum_tank / d->n_tank) : DL_NO_VALUE,
            .out_c10 = d->n_out ? c10(d->sum_out / d->n_out) : DL_NO_VALUE,
            .press_c100 = d->n_press ? (int16_t)lround(d->sum_press / d->n_press * 100) : DL_NO_VALUE,
            .p_c10 = c10(s->p),
            .i_c10 = c10(s->i),
            .humidity = d->n_room ? u8c(d->sum_hum / d->n_room) : 0,
            .mod = d->flame_s ? u8c(d->mod_sum / d->flame_s) : 0,
            .flame_s = (uint8_t)d->flame_s,
            .dhw_s = (uint8_t)d->dhw_s,
            .cond_s = (uint8_t)d->cond_s,
            .ch_starts = (uint8_t)d->ch_starts,
            .dhw_starts = (uint8_t)d->dhw_starts,
            .flags = (s->sensor_ok ? DL_F_SENSOR_OK : 0) | (s->boiler_up ? DL_F_BOILER_UP : 0) |
                     (s->ch_enable ? DL_F_CH_EN : 0) | (s->dhw_enable ? DL_F_DHW_EN : 0) |
                     (s->fault ? DL_F_FAULT : 0) | (s->heat_mode && s->ch_allowed ? DL_F_HEAT_MODE : 0) |
                     (s->epoch ? DL_F_CLOCK : 0) | (s->sim ? DL_F_SIM : 0),
            .ctrl_state = s->ctrl_state,
            .sched_src = s->sched_src,
            .dhw_reason = s->dhw_reason,
            .rssi = s->rssi,
            .ot_err = (uint8_t)(err > 255 ? 255 : err),
            .min_s = (uint8_t)d->min_s,
            .wh = (uint16_t)lround(fmin(d->wh, 65535)),
        };
        d->recs[d->head] = r;
        d->head = (d->head + 1) % d->nrecs;
        if (d->count < d->nrecs) {
            d->count++;
        }
    }
    reset_minute(d);
    return true;
}

int datalog_count(const dl_t *d)
{
    return d->count;
}

bool datalog_get(const dl_t *d, int i, dl_rec_t *out)
{
    if (i < 0 || i >= d->count) {
        return false;
    }
    *out = d->recs[(d->head - d->count + i + d->nrecs) % d->nrecs];
    return true;
}

static int cmp_u16(const void *a, const void *b)
{
    return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

void datalog_kpi(const dl_t *d, float window_h, dl_kpi_t *k)
{
    memset(k, 0, sizeof(*k));
    k->window_h = window_h;
    int want = (int)lroundf(window_h * 60);
    int n = d->count < want ? d->count : want;
    double ch_s = 0, cond = 0, minm = 0, mod_ch = 0, wh = 0, room = 0, target = 0, flow = 0, ret = 0;
    int nroom = 0, nflow = 0, nret = 0;
    for (int j = d->count - n; j < d->count; j++) {
        dl_rec_t r;
        datalog_get(d, j, &r);
        k->ch_starts += r.ch_starts;
        k->dhw_starts += r.dhw_starts;
        k->burner_h += r.flame_s / 3600.0f;
        k->dhw_burner_h += r.dhw_s / 3600.0f;
        int chs = r.flame_s - r.dhw_s;
        ch_s += chs;
        cond += r.cond_s;
        minm += r.min_s;
        // Mean modulation of the minute applies to all its flame seconds; weight CH by its share
        mod_ch += (double)r.mod * chs;
        wh += r.wh;
        k->ot_err += r.ot_err;
        target += r.target_c10 / 10.0;
        if (r.room_c100 != DL_NO_VALUE) {
            double rc = r.room_c100 / 100.0, tc = r.target_c10 / 10.0;
            room += rc;
            nroom++;
            if (r.flags & DL_F_HEAT_MODE) {
                if (rc < tc - DL_COMFORT_BAND_K) {
                    k->comfort_kh += (float)((tc - DL_COMFORT_BAND_K - rc) / 60);
                }
                if (rc > tc + DL_COMFORT_BAND_K) {
                    k->over_kh += (float)((rc - tc - DL_COMFORT_BAND_K) / 60);
                }
            }
        }
        if (r.flow_c10 != DL_NO_VALUE && r.flame_s) {
            flow += r.flow_c10 / 10.0, nflow++;
        }
        if (r.ret_c10 != DL_NO_VALUE && r.flame_s) {
            ret += r.ret_c10 / 10.0, nret++;
        }
    }
    k->hours = n / 60.0f;
    k->ch_starts_h = k->ch_starts / fmaxf(k->hours, 1.0f);  // no extrapolation from a few minutes of data
    k->cond_pct = ch_s > 0 ? (float)(100 * cond / ch_s) : 0;
    k->min_mod_pct = ch_s > 0 ? (float)(100 * minm / ch_s) : 0;
    k->mean_mod = ch_s > 0 ? (float)(mod_ch / ch_s) : 0;
    k->gas_kwh = (float)(wh / 1000);
    k->mean_room = nroom ? (float)(room / nroom) : 0;
    k->mean_target = n ? (float)(target / n) : 0;
    k->mean_flow = nflow ? (float)(flow / nflow) : 0;
    k->mean_return = nret ? (float)(ret / nret) : 0;

    // Completed CH burns that started inside the window
    uint32_t from = d->sec > (uint32_t)(n * 60) ? d->sec - (uint32_t)(n * 60) : 0;
    uint16_t durs[512];
    int nb = 0, shorts = 0;
    for (int j = 0; j < d->bcount && nb < 512; j++) {
        const dl_burn_t *b = &d->burns[(d->bhead - 1 - j + d->nburns) % d->nburns];
        if (b->start_s < from) {
            break;
        }
        if (b->dhw) {
            continue;
        }
        durs[nb++] = b->dur_s;
        shorts += b->dur_s < DL_SHORT_BURN_S;
    }
    k->ch_burns = (uint16_t)nb;
    if (nb) {
        qsort(durs, nb, sizeof(uint16_t), cmp_u16);
        k->median_burn_min = (nb % 2 ? durs[nb / 2] : (durs[nb / 2 - 1] + durs[nb / 2]) / 2.0f) / 60.0f;
        k->short_pct = 100.0f * shorts / nb;
    }
}

const char *datalog_csv_header(void)
{
    return "time,uptime_min,room,humidity,target,setpoint,tset,requested,flow,return,tank,outside,pressure,"
           "modulation,flame_s,dhw_s,cond_s,min_mod_s,ch_starts,dhw_starts,gas_wh,pi_p,pi_i,sensor_ok,boiler_up,"
           "ch_enable,dhw_enable,fault,heating_allowed,clock,simulated,ctrl_state,sched_src,dhw_reason,rssi,ot_errors\n";
}

static int put(char *out, size_t size, size_t *n, const char *fmt, double v, bool valid)
{
    int w = valid ? snprintf(out + *n, size - *n, fmt, v) : snprintf(out + *n, size - *n, ",");
    if (w < 0 || (size_t)w >= size - *n) {
        return -1;
    }
    *n += (size_t)w;
    return 0;
}

int datalog_csv_row(const dl_rec_t *r, char *out, size_t size)
{
    size_t n = 0;
    int w;
    if (r->t) {
        w = snprintf(out, size, "%lld,%lu", (long long)r->t, (unsigned long)r->up_min);
    } else {
        w = snprintf(out, size, ",%lu", (unsigned long)r->up_min);
    }
    if (w < 0 || (size_t)w >= size) {
        return -1;
    }
    n = (size_t)w;
    bool room = r->room_c100 != DL_NO_VALUE;
    if (put(out, size, &n, ",%.2f", r->room_c100 / 100.0, room) || put(out, size, &n, ",%.0f", r->humidity, room) ||
        put(out, size, &n, ",%.1f", r->target_c10 / 10.0, true) ||
        put(out, size, &n, ",%.1f", r->setpoint_c10 / 10.0, true) ||
        put(out, size, &n, ",%.1f", r->tset_c10 / 10.0, true) ||
        put(out, size, &n, ",%.1f", r->req_c10 / 10.0, true) ||
        put(out, size, &n, ",%.1f", r->flow_c10 / 10.0, r->flow_c10 != DL_NO_VALUE) ||
        put(out, size, &n, ",%.1f", r->ret_c10 / 10.0, r->ret_c10 != DL_NO_VALUE) ||
        put(out, size, &n, ",%.1f", r->tank_c10 / 10.0, r->tank_c10 != DL_NO_VALUE) ||
        put(out, size, &n, ",%.1f", r->out_c10 / 10.0, r->out_c10 != DL_NO_VALUE) ||
        put(out, size, &n, ",%.2f", r->press_c100 / 100.0, r->press_c100 != DL_NO_VALUE)) {
        return -1;
    }
    w = snprintf(out + n, size - n, ",%u,%u,%u,%u,%u,%u,%u,%u,%.1f,%.1f,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u,%u,%d,%u\n",
                 r->mod, r->flame_s, r->dhw_s, r->cond_s, r->min_s, r->ch_starts, r->dhw_starts, r->wh,
                 r->p_c10 / 10.0, r->i_c10 / 10.0, !!(r->flags & DL_F_SENSOR_OK), !!(r->flags & DL_F_BOILER_UP),
                 !!(r->flags & DL_F_CH_EN), !!(r->flags & DL_F_DHW_EN), !!(r->flags & DL_F_FAULT),
                 !!(r->flags & DL_F_HEAT_MODE), !!(r->flags & DL_F_CLOCK), !!(r->flags & DL_F_SIM), r->ctrl_state,
                 r->sched_src, r->dhw_reason, r->rssi, r->ot_err);
    if (w < 0 || (size_t)w >= size - n) {
        return -1;
    }
    return (int)(n + (size_t)w);
}
