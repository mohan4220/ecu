#include "plant.h"

#include <string.h>

#define DT 0.01f /* 10 ms */

void plant_init(plant_t *p)
{
    memset(p, 0, sizeof(*p));
    p->coolant_c = 30.0f;
    p->battery_v = 25.2f;
    p->mains_on = true;
    p->load_pct = 40.0f;
}

static float towards(float x, float target, float rate)
{
    float step = rate * DT;
    if (x < target - step) return x + step;
    if (x > target + step) return x - step;
    return target;
}

void plant_step(plant_t *p, const gcu_outputs_t *out, gcu_inputs_t *in)
{
    /* --- engine speed ------------------------------------------------ */
    float target;
    float rate;
    if (p->fired && (out->run_enable || p->stuck_fuel)) {
        target = p->governor_runaway ? 2000.0f : 1500.0f;
        rate = 600.0f; /* rpm/s spin-up */
    } else if (out->starter) {
        target = 250.0f; /* cranking speed */
        rate = 500.0f;
    } else {
        target = 0.0f;
        rate = 300.0f; /* spin-down */
    }
    p->rpm = towards(p->rpm, target, rate);

    /* Combustion: fires after a bit of cranking with fuel available. */
    if (!p->fired && out->run_enable && out->starter && !p->fuel_blocked &&
        p->rpm > 180.0f) {
        p->fired = true;
    }
    /* Dies when fuel is cut (or blocked mid-run) — unless stuck. */
    if (p->fired && ((!out->run_enable && !p->stuck_fuel) || p->fuel_blocked)) {
        p->fired = false;
    }

    /* --- thermals, oil, battery -------------------------------------- */
    if (p->fired) {
        p->coolant_c = towards(p->coolant_c, 82.0f, 0.8f);
    } else {
        p->coolant_c = towards(p->coolant_c, 30.0f, 0.1f);
    }

    float oil = (p->oil_pump_broken) ? 0.0f : (p->rpm / 1500.0f) * 4.0f;

    if (out->starter) {
        p->battery_v = towards(p->battery_v, 18.0f, 8.0f); /* crank sag */
    } else if (p->fired && !p->charge_alt_broken) {
        p->battery_v = towards(p->battery_v, 27.6f, 0.5f); /* charging */
    } else {
        p->battery_v = towards(p->battery_v, 25.0f, 0.2f); /* rest */
    }

    /* --- fill controller inputs -------------------------------------- */
    memset(in, 0, sizeof(*in));
    in->rpm = p->rpm;
    in->rpm_valid = true;
    in->oil_pressure_bar = oil;
    in->oil_pressure_valid = true;
    in->coolant_temp_c = p->coolant_c;
    in->coolant_temp_valid = true;
    in->fuel_level_pct = 80.0f;

    float gen_v = 0.0f, gen_hz = 0.0f;
    if (p->rpm > 1200.0f) { /* AVR excites near speed */
        gen_hz = p->rpm / 30.0f;         /* 4-pole: 1500 rpm = 50 Hz */
        gen_v = 240.0f * (gen_hz / 50.0f); /* V/Hz-ish behaviour       */
        if (gen_v > 250.0f) gen_v = 250.0f;
    }
    for (int i = 0; i < 3; i++) {
        in->gen_v[i] = gen_v;
        in->mains_v[i] = p->mains_on ? 240.0f : 0.0f;
        in->load_a[i] =
            out->gen_contactor ? (34.8f * p->load_pct / 100.0f) : 0.0f;
    }
    in->gen_hz = gen_hz;
    in->mains_hz = p->mains_on ? 50.0f : 0.0f;

    in->battery_v = p->battery_v;
    in->dplus_v =
        (p->fired && !p->charge_alt_broken) ? p->battery_v : 1.0f;
}
