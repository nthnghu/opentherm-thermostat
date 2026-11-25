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

static void hour_reset(dl_t *d, uint32_t t)
{
    d->h_t = t;
    d->h_n = d->h_nroom = d->h_nflow = 0;
    d->h_room = d->h_target = d->h_flow = d->h_wh = d->h_heat_s = d->h_dhw_s = 0;
    d->h_min = INT16_MAX;
    d->h_max = INT16_MIN;
    d->h_starts = 0;
}

void datalog_init(dl_t *d, dl_rec_t *recs, int nrecs, dl_burn_t *burns, int nburns, dl_hour_t *hours, int nhours)
{
    memset(d, 0, sizeof(*d));
    d->recs = recs;
    d->nrecs = nrecs;
    d->burns = burns;
    d->nburns = nburns;
    d->hours = hours;
    d->nhours = nhours;
    hour_reset(d, 0);
}

// Records stay in time order. When the clock was set back (an NTP correction), a new record that is not later
// than the newest stored one is left out until the clock has passed it: a gap as long as the step. A step back by
// more than DL_BACKSTEP_MAX_S means the stored times were wrong (the clock had been set far ahead): those records
// are dropped and the log starts again. The same rule applies when records are restored from flash.
static bool put_rec(dl_t *d, const dl_rec_t *r)
{
    if (!d->recs || d->nrecs <= 0) {
        return false;
    }
    if (r->t > 0 && d->count > 0) {
        uint32_t last_t = d->recs[(d->head - 1 + d->nrecs) % d->nrecs].t;
        if (last_t > 0 && r->t <= last_t) {
            if (last_t - r->t <= DL_BACKSTEP_MAX_S) {
                d->skipped++;
                return false;
            }
            d->count = 0;
        }
    }
    d->recs[d->head] = *r;
    d->head = (d->head + 1) % d->nrecs;
    if (d->count < d->nrecs) {
        d->count++;
    }
    d->rec_seq++;
    return true;
}

static void put_burn(dl_t *d, const dl_burn_t *b)
{
    if (!d->burns || d->nburns <= 0) {
        return;
    }
    d->burns[d->bhead] = *b;
    d->bhead = (d->bhead + 1) % d->nburns;
    if (d->bcount < d->nburns) {
        d->bcount++;
    }
    d->burn_seq++;
}

static void put_hour(dl_t *d, const dl_hour_t *h)
{
    if (!d->hours || d->nhours <= 0) {
        return;
    }
    if (h->t > 0 && d->hcount > 0) {  // time order, as for the minute records
        uint32_t last_t = d->hours[(d->hhead - 1 + d->nhours) % d->nhours].t;
        if (last_t > 0 && h->t <= last_t) {
            if (last_t - h->t <= DL_BACKSTEP_MAX_S) {
                return;
            }
            d->hcount = 0;
        }
    }
    d->hours[d->hhead] = *h;
    d->hhead = (d->hhead + 1) % d->nhours;
    if (d->hcount < d->nhours) {
        d->hcount++;
    }
    d->hour_seq++;
}

void datalog_restore_rec(dl_t *d, const dl_rec_t *r)
{
    put_rec(d, r);
}

void datalog_restore_burn(dl_t *d, const dl_burn_t *b)
{
    dl_burn_t c = *b;
    c.start_s = 0;  // seconds since start belong to the previous run
    put_burn(d, &c);
}

void datalog_restore_hour(dl_t *d, const dl_hour_t *h)
{
    put_hour(d, h);
}

// Closes the summary of the hour in progress (only hours with the clock set are kept).
static void hour_close(dl_t *d)
{
    if (d->h_t && d->h_n) {
        dl_hour_t h = {
            .t = d->h_t,
            .room_c10 = d->h_nroom ? c10(d->h_room / d->h_nroom) : DL_NO_VALUE,
            .room_min_c10 = d->h_nroom ? d->h_min : DL_NO_VALUE,
            .room_max_c10 = d->h_nroom ? d->h_max : DL_NO_VALUE,
            .target_c10 = c10(d->h_target / d->h_n),
            .flow_c10 = d->h_nflow ? c10(d->h_flow / d->h_nflow) : DL_NO_VALUE,
            .wh = (uint16_t)lround(fmin(d->h_wh, 65535)),
            .heat_min = u8c(d->h_heat_s / 60),
            .dhw_min = u8c(d->h_dhw_s / 60),
            .ch_starts = (uint8_t)(d->h_starts > 255 ? 255 : d->h_starts),
            .minutes = (uint8_t)d->h_n,
        };
        put_hour(d, &h);
    }
}

// Adds a finished minute to the hour in progress; a new clock hour closes the previous one.
static void hour_add(dl_t *d, const dl_rec_t *r)
{
    uint32_t hour = r->t > 0 ? (uint32_t)(r->t - 1) / 3600 * 3600 : 0;  // t is the end of the minute
    if (hour != d->h_t) {
        hour_close(d);
        hour_reset(d, hour);
    }
    if (!hour) {
        return;
    }
    d->h_n++;
    d->h_target += r->target_c10 / 10.0;
    if (r->room_c100 != DL_NO_VALUE) {
        int16_t rc = (int16_t)lround(r->room_c100 / 10.0);
        d->h_room += r->room_c100 / 100.0;
        d->h_nroom++;
        d->h_min = rc < d->h_min ? rc : d->h_min;
        d->h_max = rc > d->h_max ? rc : d->h_max;
    }
    if (r->flow_c10 != DL_NO_VALUE && r->flame_s) {
        d->h_flow += r->flow_c10 / 10.0;
        d->h_nflow++;
    }
    d->h_wh += r->wh;
    d->h_heat_s += r->flame_s - r->dhw_s;
    d->h_dhw_s += r->dhw_s;
    if (r->mark != DL_MARK_SVC) {  // service-test starts are not the house's cycling
        d->h_starts += r->ch_starts;
    }
}

static void reset_minute(dl_t *d)
{
    d->n = 0;
    d->sum_room = d->sum_hum = d->sum_target = d->sum_flow = d->sum_ret = d->sum_tank = d->sum_out = 0;
    d->sum_press = d->mod_sum = 0;
    d->n_room = d->n_flow = d->n_ret = d->n_tank = d->n_out = d->n_press = 0;
    d->flame_s = d->dhw_s = d->cond_s = d->min_s = d->ch_starts = d->dhw_starts = 0;
    d->svc = false;
    d->wh = 0;
}

static void end_burn(dl_t *d)
{
    if (d->burns && d->nburns > 0) {
        uint32_t dur = d->sec - d->burn_start;
        dl_burn_t b = {
            .start_s = d->burn_start,
            .t = d->burn_t,
            .dur_s = (uint16_t)(dur > 65535 ? 65535 : dur),
            .dhw = d->burn_dhw,
            .ret_c = d->burn_n ? u8c(d->burn_ret / d->burn_n) : 0,
            .mod = d->burn_n ? u8c(d->burn_mod / d->burn_n) : 0,
            .mark = d->burn_svc ? DL_MARK_SVC : 0,
        };
        put_burn(d, &b);
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
        d->burn_t = s->epoch > 0 ? (uint32_t)s->epoch : 0;
        d->burn_dhw = s->dhw_active;
        d->burn_svc = s->svc;
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
    d->svc |= s->svc;
    if (d->flame) {
        d->burn_dhw |= s->dhw_active;
        d->burn_svc |= s->svc;
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
            .mark = d->svc ? DL_MARK_SVC : 0,
        };
        if (put_rec(d, &r)) {
            hour_add(d, &r);
        }
    }
    reset_minute(d);
    return true;
}

int datalog_count(const dl_t *d)
{
    return d->count;
}

int datalog_burn_count(const dl_t *d)
{
    return d->bcount;
}

bool datalog_get_burn(const dl_t *d, int i, dl_burn_t *out)
{
    if (i < 0 || i >= d->bcount) {
        return false;
    }
    *out = d->burns[(d->bhead - d->bcount + i + d->nburns) % d->nburns];
    return true;
}

int datalog_hour_count(const dl_t *d)
{
    return d->hcount;
}

bool datalog_get_hour(const dl_t *d, int i, dl_hour_t *out)
{
    if (i < 0 || i >= d->hcount) {
        return false;
    }
    *out = d->hours[(d->hhead - d->hcount + i + d->nhours) % d->nhours];
    return true;
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
    // With the clock set, the window is by time: records restored from before a restart (or a gap while the
    // device was off) only count when they fall inside it.
    int64_t now_t = 0;
    if (d->count) {
        dl_rec_t last;
        if (datalog_get(d, d->count - 1, &last)) {
            now_t = last.t;
        }
    }
    if (now_t > 0) {
        int64_t from_t = now_t - (int64_t)lroundf(window_h * 3600);
        n = 0;
        for (int j = d->count - 1; j >= 0; j--) {
            dl_rec_t r;
            if (!datalog_get(d, j, &r)) {
                break;
            }
            if (r.t <= from_t || (r.t == 0 && n > 0)) {
                break;
            }
            n++;
        }
    }
    double ch_s = 0, cond = 0, minm = 0, mod_ch = 0, wh = 0, room = 0, target = 0, flow = 0, ret = 0;
    int nroom = 0, nflow = 0, nret = 0;
    for (int j = d->count - n; j < d->count; j++) {
        dl_rec_t r;
        if (!datalog_get(d, j, &r)) {
            continue;
        }
        // Service mode (tests): gas, burner hours and the room still count; starts, burn quality and comfort not
        bool svc = r.mark == DL_MARK_SVC;
        k->burner_h += r.flame_s / 3600.0f;
        k->dhw_burner_h += r.dhw_s / 3600.0f;
        wh += r.wh;
        k->ot_err += r.ot_err;
        target += r.target_c10 / 10.0;
        if (svc) {
            k->svc_min++;
            if (r.room_c100 != DL_NO_VALUE) {
                room += r.room_c100 / 100.0;
                nroom++;
            }
            continue;
        }
        k->ch_starts += r.ch_starts;
        k->dhw_starts += r.dhw_starts;
        int chs = r.flame_s - r.dhw_s;
        ch_s += chs;
        cond += r.cond_s;
        minm += r.min_s;
        // Mean modulation of the minute applies to all its flame seconds; weight CH by its share
        mod_ch += (double)r.mod * chs;
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
    int64_t from_t = now_t > 0 ? now_t - (int64_t)lroundf(window_h * 3600) : 0;
    uint16_t durs[512];
    int nb = 0, shorts = 0;
    for (int j = 0; j < d->bcount && nb < 512; j++) {
        const dl_burn_t *b = &d->burns[(d->bhead - 1 - j + d->nburns) % d->nburns];
        bool inside = (now_t > 0 && b->t > 0) ? b->t > from_t : (b->start_s > 0 && b->start_s >= from);
        if (!inside) {
            break;
        }
        if (b->dhw || b->mark == DL_MARK_SVC) {  // tank burns and service-test burns are not CH cycling
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
