#include "cfgtext.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Texts are ASCII apart from the degree sign: the device fonts have no minus or plus-minus glyphs.

static const struct {
    const char *title, *brief;
} GROUPS[CG_COUNT] = {
    {"Program, boost and away", "Weekly Comfort / Eco times, Eco drop, boost, away"},
    {"Summer mode", "Turn heating off automatically in warm weather"},
    {"Hot water", "When the boiler may heat the tank, tank temperature"},
    {"Legionella protection", "Weekly hot cycle of the tank"},
    {"Safety", "Frost protection, what to do if the room sensor fails"},
    {"Heating system", "Radiator water limits, underfloor circuit"},
    {"Fine tuning (expert)", "How the thermostat drives the boiler"},
    {"Alerts", "Which problems to report, and when"},
    {"Service tests (expert)", "Settings of the commissioning tests"},
};

static const cfgtext_opt_t DHW_MODES[] = {
    {"Always", "The boiler keeps the tank hot all day."},
    {"Time windows", "The tank is heated only inside the hot water windows (plus the minimum tank temperature)."},
    {"Off", "The tank is not heated (except to keep its minimum temperature, if that is on)."},
};
static const cfgtext_opt_t LOSS_MODES[] = {
    {"Keep going", "Carry on with the last heating decision until the sensor is back."},
    {"Heating off", "Stop heating. The house may cool down until the sensor is fixed."},
    {"Gentle heat", "Heat at a fixed radiator temperature for part of every 30 minutes."},
};
static const cfgtext_opt_t STRATEGIES[] = {
    {"Simple on/off", "Burner on below the target, off above it, at one fixed radiator temperature. "
                      "The reference to compare against."},
    {"Modulating", "Adjusts the radiator water temperature continuously to the heat the house needs "
                   "(recommended, best for a condensing boiler)."},
};
static const cfgtext_opt_t WEEKDAYS[] = {
    {"Monday", ""}, {"Tuesday", ""}, {"Wednesday", ""}, {"Thursday", ""}, {"Friday", ""}, {"Saturday", ""}, {"Sunday", ""},
};
#define OPTS(a) a, (int)(sizeof(a) / sizeof(a[0]))
#define NOOPTS NULL, 0

#define WIN(n)                                                                                                       \
    {"dhw_win" #n "_start", CG_DHW, "Window " #n " starts", "Start of hot water window " #n,                         \
     "Start of a time window in which the boiler may heat the tank (Time windows mode). Set below 00:00 to "      \
     "leave this window unused.",                                                                                   \
     "", "dhw_mode=1", NOOPTS},                                                                                     \
    {"dhw_win" #n "_end", CG_DHW, "Window " #n " ends", "End of hot water window " #n,                               \
     "End of the window. It may be after midnight (e.g. 22:00 to 01:00).", "", "dhw_mode=1", NOOPTS}

static const cfgtext_t T[] = {
    /* ---------------------------------------------------------------- program */
    {"prog_on", CG_PROGRAM, "Weekly program", "Follow the Comfort / Eco times of each day",
     "On: the room target follows the week program (Schedule page): Comfort uses your setpoint, Eco is a few "
     "degrees lower. Off: the thermostat holds your setpoint all the time.",
     "", NULL, NOOPTS},
    {"eco_setback_k", CG_PROGRAM, "Eco temperature drop", "How much cooler the Eco periods are",
     "During Eco periods the target is your setpoint minus this. Example: setpoint 21 °C, drop 2 °C = 19 °C. "
     "Small drops (1 to 3 °C) suit a condensing boiler and underfloor heating best.",
     "°C", "prog_on", NOOPTS},
    {"boost_on", CG_PROGRAM, "Boost button", "Allow a short temporary warm-up",
     "Shows the Boost button (Schedule page, Home Assistant): the target is raised for a while, then the normal "
     "program continues.",
     "", NULL, NOOPTS},
    {"boost_k", CG_PROGRAM, "Boost: extra warmth", "Added to your setpoint during a boost",
     "How much warmer than your setpoint a boost makes the room.", "°C", "boost_on", NOOPTS},
    {"boost_min", CG_PROGRAM, "Boost: length", "How long a boost lasts",
     "After this time the normal program continues by itself.", "min", "boost_on", NOOPTS},
    {"away_on", CG_PROGRAM, "Away mode", "Allow a low holiday temperature",
     "Shows the Away button (Schedule page, Home Assistant). While away, the house is kept at the away "
     "temperature until you come back or the end date.",
     "", NULL, NOOPTS},
    {"away_c", CG_PROGRAM, "Away: temperature", "Room temperature kept while away",
     "The target while away mode is on. 15 °C keeps the house dry and quick to warm up again.", "°C", "away_on",
     NOOPTS},
    {"away_dhw_off", CG_PROGRAM, "Away: hot water off", "Stop heating the tank while away",
     "On: the tank is not heated while away (the minimum tank temperature still applies, if on).", "", "away_on",
     NOOPTS},

    /* ----------------------------------------------------------------- summer */
    {"summer_on", CG_SUMMER, "Summer mode (auto)", "No heating while the outside is warm",
     "On: heating stops while the 24-hour average outside temperature is warm; hot water and frost protection "
     "keep working. Needs an outdoor temperature (boiler outdoor probe, or Home Assistant later).",
     "", NULL, NOOPTS},
    {"summer_c", CG_SUMMER, "Summer: heating off from", "24 h average outside temperature",
     "Heating stops when the 24-hour average outside temperature reaches this.", "°C", "summer_on", NOOPTS},
    {"summer_hyst_k", CG_SUMMER, "Summer: heating back below", "Degrees under the off temperature",
     "Heating comes back when the 24-hour average falls this much below the off temperature, so it does not "
     "switch back and forth in spring and autumn.",
     "°C", "summer_on", NOOPTS},

    /* -------------------------------------------------------------- hot water */
    {"dhw_mode", CG_DHW, "Hot water", "When the boiler may heat the tank",
     "Always, only inside the time windows below, or off.", "", NULL, OPTS(DHW_MODES)},
    WIN(1), WIN(2), WIN(3),
    {"guard_on", CG_DHW, "Minimum tank temperature", "Never let the tank get too cold",
     "On: the tank is heated whenever it falls below the minimum, even outside the windows, in Off mode or "
     "while away.",
     "", NULL, NOOPTS},
    {"guard_c", CG_DHW, "Minimum: temperature", "Heat the tank below this",
     "The lowest tank temperature allowed.", "°C", "guard_on", NOOPTS},
    {"guard_hyst_k", CG_DHW, "Minimum: reheat by", "Degrees above the minimum",
     "Once started, the tank is heated until it is this much above the minimum.", "°C", "guard_on", NOOPTS},
    {"piggy_on", CG_DHW, "Top up after heating", "Heat the tank right after a radiator burn",
     "In Time windows mode: after the burner has heated the radiators, also allow hot water for a while. "
     "The boiler is already warm, so this saves a separate start.",
     "", NULL, NOOPTS},
    {"piggy_min", CG_DHW, "Top up: length", "How long hot water is allowed after a burn",
     "Length of the extra hot water time after a heating burn.", "min", "piggy_on", NOOPTS},
    {"dhw_boost_min", CG_DHW, "Hot water boost", "Length of the hot water boost button",
     "The hot water boost button allows tank heating now, for this long, whatever the mode. 0 hides the "
     "button.",
     "min", NULL, NOOPTS},
    {"dhw_set_on", CG_DHW, "Set tank temperature here", "Instead of on the boiler panel",
     "On: the thermostat sends the tank temperature below to the boiler. Off: the boiler panel setting is "
     "used. Works only if the boiler accepts it (Service, test 'writable').",
     "", NULL, NOOPTS},
    {"dhw_set_c", CG_DHW, "Tank temperature", "Hot water temperature the boiler keeps",
     "The tank temperature sent to the boiler. 50 °C is a good balance of comfort and efficiency.", "°C",
     "dhw_set_on", NOOPTS},

    /* ------------------------------------------------------------- legionella */
    {"pasteur_on", CG_LEGIONELLA, "Weekly hot cycle", "Heat the tank to kill legionella once a week",
     "On: once a week the tank is heated to a high temperature and held there. Works only if the boiler "
     "accepts a tank temperature from the thermostat. Take care at the taps during the cycle.",
     "", NULL, NOOPTS},
    {"pasteur_day", CG_LEGIONELLA, "Day", "Day of the weekly cycle", "Day of the week the cycle runs.", "",
     "pasteur_on", OPTS(WEEKDAYS)},
    {"pasteur_min", CG_LEGIONELLA, "Start time", "When the cycle starts",
     "Pick a time when someone is home and awake (scald risk), e.g. after lunch.", "", "pasteur_on", NOOPTS},
    {"pasteur_c", CG_LEGIONELLA, "Temperature", "Tank temperature during the cycle",
     "60 to 65 °C kills legionella bacteria.", "°C", "pasteur_on", NOOPTS},
    {"pasteur_hold_min", CG_LEGIONELLA, "Hold time", "How long the tank stays that hot",
     "Time the tank is kept at the cycle temperature.", "min", "pasteur_on", NOOPTS},
    {"pasteur_timeout_min", CG_LEGIONELLA, "Give up after", "Stop if it takes too long",
     "If the tank has not finished the cycle by then, the cycle stops and an alert is raised.", "min",
     "pasteur_on", NOOPTS},

    /* ----------------------------------------------------------------- safety */
    {"frost_on", CG_SAFETY, "Frost protection", "Heat even when off if the room gets very cold",
     "On: whatever the mode, program or summer mode, the house is heated when the room falls below the frost "
     "limit. Keeps pipes from freezing.",
     "", NULL, NOOPTS},
    {"frost_room_c", CG_SAFETY, "Frost limit", "Room temperature that starts frost heating",
     "Below this room temperature the frost protection heats.", "°C", "frost_on", NOOPTS},
    {"frost_flow_c", CG_SAFETY, "Frost: radiator water", "Water temperature used for frost heating",
     "Radiator water temperature while frost protection heats.", "°C", "frost_on", NOOPTS},
    {"loss_mode", CG_SAFETY, "If the room sensor fails", "What to do without a room temperature",
     "What the thermostat does while it cannot read the room temperature. An alert is raised either way.", "",
     NULL, OPTS(LOSS_MODES)},
    {"loss_flow_c", CG_SAFETY, "Sensor failed: water temp", "Radiator water temperature in Gentle heat",
     "Radiator water temperature used while the room sensor has failed (Gentle heat).", "°C", "loss_mode=2",
     NOOPTS},
    {"loss_duty_pct", CG_SAFETY, "Sensor failed: heating share", "Part of every 30 minutes with heat",
     "Example: 40 % = 12 minutes of heat in every 30 minutes.", "%", "loss_mode=2", NOOPTS},

    /* ----------------------------------------------------------------- system */
    {"flow_max_c", CG_SYSTEM, "Hottest radiator water", "Upper limit of the water temperature",
     "The thermostat never asks the boiler for hotter water than this. Lower is more efficient (the boiler "
     "condenses below about 55 °C return); raise only if rooms stay cold on the coldest days.",
     "°C", NULL, NOOPTS},
    {"flow_min_c", CG_SYSTEM, "Lowest steady water temp", "Boiler at its smallest flame",
     "The water temperature the boiler reaches at its smallest flame. Below it the burner is pulsed instead. "
     "Measured by the service test 'Minimum-fire flow'.",
     "°C", NULL, NOOPTS},
    {"ufh_on", CG_SYSTEM, "Underfloor circuit", "Keep the water warm enough for the floor",
     "On: while heating, the water is never colder than the floor mixer setting plus the margin, so the "
     "underfloor circuit still gets heat.",
     "", NULL, NOOPTS},
    {"ufh_mixer_c", CG_SYSTEM, "Floor mixer setting", "Temperature set on the floor mixing valve",
     "The temperature set on the underfloor thermostatic mixing valve.", "°C", "ufh_on", NOOPTS},
    {"ufh_margin_k", CG_SYSTEM, "Floor: extra margin", "Water this much above the mixer setting",
     "The boiler water is kept this much above the mixer setting while heating.", "°C", "ufh_on", NOOPTS},

    /* ----------------------------------------------------------------- tuning */
    {"strategy", CG_TUNING, "Control method", "How the radiator water temperature is chosen",
     "Modulating is the normal method. Simple on/off is a reference for comparing efficiency.", "", NULL,
     OPTS(STRATEGIES)},
    {"onoff_hyst_k", CG_TUNING, "On/off: switching band", "Degrees either side of the target",
     "Simple on/off only: the burner starts this much below the target and stops this much above it.", "°C",
     "strategy=0", NOOPTS},
    {"onoff_flow_c", CG_TUNING, "On/off: water temperature", "Fixed radiator water temperature",
     "Simple on/off only: the radiator water temperature while heating.", "°C", "strategy=0", NOOPTS},
    {"pi_base_c", CG_TUNING, "Starting water temperature", "Water temperature with no correction",
     "Modulating: the water temperature asked for when the room is exactly at its target, before the "
     "thermostat has learnt the house.",
     "°C", "strategy=1", NOOPTS},
    {"pi_kp", CG_TUNING, "Reaction strength", "Water degrees per room degree off target",
     "Modulating: how strongly the water temperature reacts to the room being off target. Example: 12 means "
     "0.5 °C too cold = 6 °C warmer water. Too high: swings; too low: slow.",
     "", "strategy=1", NOOPTS},
    {"pi_ti_h", CG_TUNING, "Learning time", "How fast the long-term correction builds up",
     "Modulating: the time over which a lasting difference is corrected. Shorter learns faster but can "
     "overshoot. 2 to 3 hours suits most houses.",
     "h", "strategy=1", NOOPTS},
    {"pi_deadband_k", CG_TUNING, "Learning dead zone", "Ignore room errors smaller than this",
     "Modulating: the long-term correction ignores differences smaller than this, so sensor noise is not "
     "learnt.",
     "°C", "strategy=1", NOOPTS},
    {"pi_i_limit_k", CG_TUNING, "Learning limit", "Largest long-term correction",
     "Modulating: the long-term correction never moves the water temperature more than this.", "°C", "strategy=1",
     NOOPTS},
    {"rate_k_per_min", CG_TUNING, "Water temp change limit", "Fastest change per minute (0 = no limit)",
     "Modulating: the water temperature asked for changes at most this much per minute. Smooth changes "
     "avoid burner starts and overshoot.",
     "°C", "strategy=1", NOOPTS},
    {"hold_on", CG_TUNING, "Keep the small flame", "Avoid stop-start at the smallest flame",
     "On: when the boiler burns at its smallest flame, the thermostat asks for slightly less than the actual "
     "water temperature, so the boiler keeps a long, steady small flame instead of stopping and starting.",
     "", NULL, NOOPTS},
    {"hold_k", CG_TUNING, "Small flame: below by", "Ask this much below the water temperature",
     "How far below the actual water temperature the request is set while keeping the small flame.", "°C",
     "hold_on", NOOPTS},
    {"duty_on", CG_TUNING, "Pulse at low demand", "Long burns with pauses instead of short ones",
     "On: when the house needs less heat than the smallest flame gives, the burner runs in planned long burns "
     "with pauses, instead of many short starts.",
     "", NULL, NOOPTS},
    {"duty_min_burn_s", CG_TUNING, "Pulse: shortest burn", "Each planned burn lasts at least this",
     "The shortest planned burn while pulsing.", "s", "duty_on", NOOPTS},
    {"duty_min_off_s", CG_TUNING, "Pulse: shortest pause", "Each pause lasts at least this",
     "The shortest pause between planned burns.", "s", "duty_on", NOOPTS},
    {"max_starts_h", CG_TUNING, "Most burner starts / hour", "Limit on heating starts",
     "The thermostat plans at most this many heating starts per hour. Fewer starts = less wear and gas.", "",
     NULL, NOOPTS},
    {"freeze_on_dhw", CG_TUNING, "Pause learning for hot water", "While the boiler heats the tank",
     "On: while the boiler heats the tank (radiators get no heat), the long-term correction is paused, so "
     "the room cooling a little is not over-corrected afterwards.",
     "", NULL, NOOPTS},

    /* ----------------------------------------------------------------- alerts */
    {"fault_on", CG_ALERTS, "Boiler fault", "The boiler reports an error code",
     "Report when the boiler signals a fault (with its code when available).", "", NULL, NOOPTS},
    {"pressure_on", CG_ALERTS, "Low water pressure", "The heating water pressure is low",
     "Report when the boiler water pressure falls below the limit. Top up at the filling loop.", "", NULL,
     NOOPTS},
    {"pressure_min_bar", CG_ALERTS, "Low pressure: limit", "Report below this pressure",
     "Typical cold pressure is 1 to 1.5 bar.", "bar", "pressure_on", NOOPTS},
    {"leak_on", CG_ALERTS, "Pressure falling", "Slow leak over a day",
     "Report when the pressure drops steadily over 24 hours, which hints at a slow leak.", "", NULL, NOOPTS},
    {"leak_bar_24h", CG_ALERTS, "Falling: drop per day", "Report a drop larger than this",
     "Pressure drop over 24 hours that counts as falling.", "bar", "leak_on", NOOPTS},
    {"link_on", CG_ALERTS, "Boiler not answering", "No reply from the boiler",
     "Report when the boiler stops answering the thermostat.", "", NULL, NOOPTS},
    {"link_s", CG_ALERTS, "Not answering: after", "How long without replies",
     "Report after the boiler has not answered for this long.", "s", "link_on", NOOPTS},
    {"sensor_on", CG_ALERTS, "Room sensor lost", "No room temperature",
     "Report when the room temperature sensor stops giving readings.", "", NULL, NOOPTS},
    {"sensor_s", CG_ALERTS, "Sensor lost: after", "How long without a reading",
     "Report after no reading for this long.", "s", "sensor_on", NOOPTS},
    {"short_on", CG_ALERTS, "Short cycling", "Burner starts and stops too often",
     "Report when the burner makes many short burns, which wastes gas and wears the boiler.", "", NULL,
     NOOPTS},
    {"short_starts_h", CG_ALERTS, "Short cycling: starts", "At least this many starts in an hour",
     "Number of heating starts in the last hour that counts as too many.", "", "short_on", NOOPTS},
    {"short_median_min", CG_ALERTS, "Short cycling: burn length", "With typical burns shorter than this",
     "Only report when the typical (median) burn is also shorter than this.", "min", "short_on", NOOPTS},
    {"cold_on", CG_ALERTS, "Room too cold", "The room does not reach its target",
     "Report when the room stays well below its target for a long time.", "", NULL, NOOPTS},
    {"cold_k", CG_ALERTS, "Too cold: by", "Degrees under the target",
     "How far under the target the room must be.", "°C", "cold_on", NOOPTS},
    {"cold_h", CG_ALERTS, "Too cold: for", "For at least this long", "How long the room must stay that cold.",
     "h", "cold_on", NOOPTS},
    {"alert_pasteur_on", CG_ALERTS, "Hot cycle failed", "The weekly legionella cycle did not finish",
     "Report when the weekly hot cycle could not reach or hold its temperature.", "", NULL, NOOPTS},
    {"clock_on", CG_ALERTS, "Clock not set", "No time from the internet",
     "Report when the thermostat has no valid time. Without it the program holds Comfort all day.", "", NULL,
     NOOPTS},
    {"clock_s", CG_ALERTS, "Clock not set: after", "How long after start-up",
     "Report when the time is still unknown this long after start-up.", "s", "clock_on", NOOPTS},

    /* ---------------------------------------------------------------- service */
    {"svc_idle_min", CG_SERVICE, "Service mode ends after", "Time without use",
     "Service mode switches itself off after this long without activity, and normal heating resumes.", "min",
     NULL, NOOPTS},
    {"svc_step_min", CG_SERVICE, "Setpoint test: step length", "Length of each step",
     "Test 'Which setpoint it heats to': how long each water temperature step lasts.", "min", NULL, NOOPTS},
    {"svc_opv_max_min", CG_SERVICE, "Min-fire test: give up after", "Longest run of that test",
     "Test 'Minimum-fire flow': stop if the water temperature has not settled by then.", "min", NULL, NOOPTS},
    {"svc_silence_s", CG_SERVICE, "Comm-loss test: silence", "How long the thermostat stays silent",
     "Test 'Comm loss': how long the thermostat sends nothing to the boiler.", "s", NULL, NOOPTS},
    {"svc_recover_min", CG_SERVICE, "Comm-loss test: wait", "For heating to work again",
     "Test 'Comm loss': how long to wait for heating to work again after talking resumes.", "min", NULL,
     NOOPTS},
    {"svc_test_flow_c", CG_SERVICE, "Test water temperature", "Used to start a burn in the tests",
     "Water temperature the tests ask for when they need the burner on.", "°C", NULL, NOOPTS},
};
#define N ((int)(sizeof(T) / sizeof(T[0])))

const cfgtext_t *cfgtext_get(const char *name)
{
    for (int i = 0; i < N; i++) {
        if (strcmp(T[i].name, name) == 0) {
            return &T[i];
        }
    }
    return NULL;
}

int cfgtext_count(void)
{
    return N;
}

const cfgtext_t *cfgtext_at(int i)
{
    return i >= 0 && i < N ? &T[i] : NULL;
}

const char *cfgtext_group_title(cfg_group_t g)
{
    return g < CG_COUNT ? GROUPS[g].title : "?";
}

const char *cfgtext_group_brief(cfg_group_t g)
{
    return g < CG_COUNT ? GROUPS[g].brief : "";
}

static void number(float x, char *out, size_t size)
{
    if (fabsf(x - roundf(x)) < 0.005f) {
        snprintf(out, size, "%.0f", x);
    } else if (fabsf(x * 10 - roundf(x * 10)) < 0.05f) {
        snprintf(out, size, "%.1f", x);
    } else {
        snprintf(out, size, "%.2f", x);
    }
}

static void duration(long s, char *out, size_t size)
{
    if (s < 120 && s % 60) {
        snprintf(out, size, "%ld s", s);
    } else if (s < 3600) {
        if (s % 60) {
            snprintf(out, size, "%ld min %ld s", s / 60, s % 60);
        } else {
            snprintf(out, size, "%ld min", s / 60);
        }
    } else if ((s / 60) % 60) {
        snprintf(out, size, "%ld h %ld min", s / 3600, (s / 60) % 60);
    } else {
        snprintf(out, size, "%ld h", s / 3600);
    }
}

static void with_unit(const ct_field_t *f, const cfgtext_t *t, float x, char *out, size_t size)
{
    const char *u = t ? t->unit : "";
    if (strcmp(u, "s") == 0 || strcmp(u, "min") == 0) {
        long s = lroundf(x) * (strcmp(u, "min") == 0 ? 60 : 1);
        if (s == 0) {
            snprintf(out, size, "0 (off)");
        } else {
            duration(s, out, size);
        }
        return;
    }
    char n[16];
    number(x, n, sizeof(n));
    if (!u[0]) {
        snprintf(out, size, "%s", n);
    } else if (strcmp(u, "%") == 0) {
        snprintf(out, size, "%s%%", n);
    } else {
        snprintf(out, size, "%s %s", n, u);
    }
}

void cfgtext_format(const ct_field_t *f, const char *raw, char *out, size_t size)
{
    const cfgtext_t *t = cfgtext_get(f->name);
    if (f->type == CT_BOOL) {
        snprintf(out, size, "%s", strcmp(raw, "0") ? "On" : "Off");
    } else if (f->type == CT_TIME || f->type == CT_TIME_OPT) {
        snprintf(out, size, "%s", strcmp(raw, "-") ? raw : "not used");
    } else if (t && t->opts) {
        int k = atoi(raw);
        snprintf(out, size, "%s", k >= 0 && k < t->nopts ? t->opts[k].label : raw);
    } else {
        with_unit(f, t, strtof(raw, NULL), out, size);
    }
}

void cfgtext_range(const ct_field_t *f, char *out, size_t size)
{
    const cfgtext_t *t = cfgtext_get(f->name);
    out[0] = 0;
    if (f->type == CT_BOOL || f->type == CT_TIME || f->type == CT_TIME_OPT || (t && t->opts)) {
        return;
    }
    char a[24], b[24];
    with_unit(f, t, f->min, a, sizeof(a));
    with_unit(f, t, f->max, b, sizeof(b));
    snprintf(out, size, "%s to %s", a, b);
}

bool cfgtext_parent(const cfgtext_t *t, char *name, size_t size, int *value)
{
    if (!t || !t->parent) {
        return false;
    }
    const char *eq = strchr(t->parent, '=');
    size_t n = eq ? (size_t)(eq - t->parent) : strlen(t->parent);
    if (n >= size) {
        return false;
    }
    memcpy(name, t->parent, n);
    name[n] = 0;
    *value = eq ? atoi(eq + 1) : 1;
    return true;
}
