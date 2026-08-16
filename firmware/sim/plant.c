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
    p->tick++;
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

/* ---------------------------------------------------------- fake engine ECU */

#define ENGINE_SA 0x00U
#define PDU2_ID(pgn) ((0x18000000U | ((pgn) << 8) | ENGINE_SA))

static void frame(j1939_frame_t *f, uint32_t pgn)
{
    memset(f->data, 0xFF, 8); /* J1939 default: not available */
    f->id = PDU2_ID(pgn);
    f->dlc = 8;
}

int plant_j1939_emit(const plant_t *p, const gcu_outputs_t *out,
                     j1939_frame_t *frames, int max)
{
    /* Battery-powered ECU: awake on run-enable and through the whole
     * spin-down, so the supervisor sees rpm actually reach 0. */
    bool awake = out->run_enable || p->rpm > 0.5f;
    if (!awake || p->j1939_silent || max < 1) {
        return 0;
    }

    int n = 0;
    if (p->tick % 2 == 0 && n < max) {          /* EEC1, 20 ms */
        frame(&frames[n], 61444U);
        uint16_t raw = (uint16_t)(p->rpm / 0.125f);
        frames[n].data[3] = (uint8_t)(raw & 0xFFU);
        frames[n].data[4] = (uint8_t)(raw >> 8);
        n++;
    }
    if (p->tick % 50 == 0 && n < max) {         /* EFL/P1, 500 ms */
        float oil = p->oil_pump_broken ? 0.0f : (p->rpm / 1500.0f) * 4.0f;
        frame(&frames[n], 65263U);
        frames[n].data[3] = (uint8_t)(oil / 0.04f);
        n++;
    }
    if (p->tick % 100 == 0 && n < max) {        /* ET1, 1 s */
        frame(&frames[n], 65262U);
        frames[n].data[0] = (uint8_t)(p->coolant_c + 40.0f);
        n++;
    }
    if (p->tick % 100 == 50 && n < max) {       /* DM1, 1 s */
        frame(&frames[n], 65226U);
        uint8_t lamps = 0;
        if (p->dm1_red_lamp) lamps |= 0x1U << 4;
        if (p->dm1_amber_lamp) lamps |= 0x1U << 2;
        frames[n].data[0] = lamps;
        frames[n].data[1] = 0xFF;
        if (p->dm1_red_lamp) {
            /* SPN 100 (oil pressure), FMI 1 — a plausible stop cause */
            frames[n].data[2] = 100;
            frames[n].data[3] = 0;
            frames[n].data[4] = 1;
            frames[n].data[5] = 1;
        } else {
            memset(&frames[n].data[2], 0, 4);
        }
        n++;
    }
    return n;
}
