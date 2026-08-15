/*
 * main_sim.c — SIL demo: full AMF cycle on the fake genset, narrated.
 *
 * Timeline: healthy mains → mains fails → auto start → transfer →
 * mains returns → retransfer → cooldown → stop. Prints every state change
 * and asserts the contactor interlock on every single tick.
 */
#include <assert.h>
#include <stdio.h>

#include "../core/gcu_app.h"
#include "plant.h"

static const char *onoff(bool b) { return b ? "ON " : "off"; }

int main(void)
{
    gcu_app_t app;
    plant_t plant;
    gcu_inputs_t in;
    gcu_outputs_t out = {0};

    gcu_app_init(&app);
    plant_init(&plant);
    /* Shorten the boring waits for the demo (still realistic order). */
    app.cfg.cooldown_ms = 20000;
    app.cfg.mains_return_qualify_ms = 10000;

    engine_state_t last_eng = ENG_STOPPED;
    amf_state_t last_amf = AMF_ON_MAINS;

    printf("t=0.0s  demo start: AUTO mode, mains healthy, engine off\n");

    for (uint32_t tick = 0; tick < 60 * 100 * 3; tick++) { /* 3 minutes */
        float t = tick / 100.0f;

        /* Scenario script */
        if (tick == 10 * 100) {
            plant.mains_on = false;
            printf("t=%.1fs  >>> MAINS FAILS\n", t);
        }
        if (tick == 90 * 100) {
            plant.mains_on = true;
            printf("t=%.1fs  >>> MAINS RETURNS\n", t);
        }

        plant_step(&plant, &out, &in);
        in.mode_auto = true;
        gcu_app_tick(&app, &in, &out);

        /* Invariant: never both contactors — checked every tick. */
        assert(!(out.gen_contactor && out.mains_contactor));

        if (app.engine.state != last_eng) {
            printf("t=%5.1fs engine: %-10s -> %-10s  rpm=%4.0f oil=%.1fbar "
                   "genV=%3.0f K1=%s K2=%s\n",
                   t, engine_state_name(last_eng),
                   engine_state_name(app.engine.state), in.rpm,
                   in.oil_pressure_bar, in.gen_v[0], onoff(out.run_enable),
                   onoff(out.starter));
            last_eng = app.engine.state;
        }
        if (app.amf.state != last_amf) {
            printf("t=%5.1fs amf:    %-10s -> %-10s  K3(gen)=%s K4(mains)=%s\n",
                   t, amf_state_name(last_amf), amf_state_name(app.amf.state),
                   onoff(out.gen_contactor), onoff(out.mains_contactor));
            last_amf = app.amf.state;
        }
    }

    printf("t=180s  demo end: engine %s, amf %s, alarms:%s\n",
           engine_state_name(app.engine.state), amf_state_name(app.amf.state),
           protection_shutdown_active(&app.prot) ? " SHUTDOWN" : " none");

    /* Demo must end back on mains with the engine stopped. */
    assert(app.engine.state == ENG_STOPPED);
    assert(app.amf.state == AMF_ON_MAINS);
    assert(out.mains_contactor && !out.gen_contactor);
    printf("PASS: full AMF cycle completed, interlock held on every tick\n");
    return 0;
}
