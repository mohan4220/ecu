#include "gcu_app.h"

#include <string.h>

void gcu_app_init(gcu_app_t *app)
{
    memset(app, 0, sizeof(*app));
    gcu_config_defaults(&app->cfg);
    engine_fsm_init(&app->engine);
    protection_init(&app->prot);
    amf_fsm_init(&app->amf);
    /* Both contactors have "been open forever" at boot. */
    app->gen_open_ticks = UINT32_MAX;
    app->mains_open_ticks = UINT32_MAX;
}

/* Resolve the two configurable relay outputs. Keeping this in one place
 * means a new AUX function is a single switch arm, not a hunt through the
 * FSMs for somewhere to hang an output. */
static bool aux_value(gcu_app_t *app, gcu_aux_fn_t fn)
{
    switch (fn) {
    case AUX_HORN:
        return protection_shutdown_active(&app->prot) ||
               protection_warning_active(&app->prot);
    case AUX_PREHEAT:
        return app->engine.state == ENG_PREHEAT;
    case AUX_RUNNING:
        return app->engine.state == ENG_RUNNING ||
               app->engine.state == ENG_WARMUP ||
               app->engine.state == ENG_COOLDOWN;
    case AUX_FAULT:
        return protection_shutdown_active(&app->prot);
    case AUX_OFF:
    default:
        return false;
    }
}

static void aux_apply(gcu_app_t *app, gcu_outputs_t *out)
{
    out->aux1 = aux_value(app, app->cfg.aux1_fn);
    out->aux2 = aux_value(app, app->cfg.aux2_fn);
}

/*
 * Crank load shed.
 *
 * C9's ride-through budget is ~63 ms down to the 6.5 V UVLO *with the
 * backlight off*; at 1.5 W it is 42 ms and the 50 ms claim fails. The
 * backlight is the only load big enough to matter and the only one this
 * board can switch, so it goes off whenever the starter is commanded —
 * before the sag, not after it — and stays off through a measured sag.
 *
 * Battery sense taps +12V ahead of D3, so the harness dip is visible within
 * one tick rather than being hidden behind the hold-up diode. Hysteresis
 * stops the panel flickering at the threshold.
 */
static void backlight_apply(gcu_app_t *app, const gcu_inputs_t *in,
                            gcu_outputs_t *out)
{
    if (out->starter || in->battery_v <= app->cfg.backlight_shed_v) {
        app->backlight_shed = true;
    } else if (in->battery_v > app->cfg.backlight_restore_v) {
        app->backlight_shed = false;
    }
    out->backlight_pct = app->backlight_shed ? 0 : app->cfg.backlight_pct;
}

void gcu_app_tick(gcu_app_t *app, const gcu_inputs_t *in, gcu_outputs_t *out)
{
    memset(out, 0, sizeof(*out));

    /* 1. Protections see the world first. */
    bool running = engine_fsm_running(&app->engine);
    bool gen_excited =
        app->engine.state == ENG_RUNNING &&
        app->engine.state_ticks >=
            GCU_MS_TO_TICKS(app->cfg.protections_arm_delay_ms);
    protection_tick(&app->prot, &app->cfg, in, running, gen_excited,
                    app->engine.fail_to_start, app->engine.fail_to_stop);

    bool shutdown = protection_shutdown_active(&app->prot);

    /* 2. MANUAL mode start/stop from keys. */
    if (!in->mode_auto) {
        if (in->key_start && !shutdown) {
            app->manual_run = true;
        }
        if (in->key_stop) {
            app->manual_run = false;
        }
    } else {
        app->manual_run = false;
    }

    /* 3. AMF demand (engine availability = RUNNING and healthy). */
    amf_demand_t demand;
    bool engine_available =
        (app->engine.state == ENG_RUNNING) && !shutdown;
    amf_fsm_tick(&app->amf, &app->cfg, in, in->mode_auto && !shutdown,
                 engine_available, &demand);

    /* remote_start in AUTO = off-load test run: engine runs, load stays on
     * mains (configurable on-load remote start is a future option). */
    bool want_run = in->mode_auto
                        ? (demand.engine_start || in->remote_start)
                        : app->manual_run;

    /* 4. Engine command. Alarm reset is deliberate: only outside AUTO
     * (DSE/ComAp convention) so a reset can never cause an immediate
     * unattended crank — re-selecting AUTO is the conscious re-arm. */
    bool alarm_reset = in->key_stop && !in->mode_auto;
    /* A FRESH red stop lamp from the engine ECU blocks starting: never
     * crank an engine whose own ECU says stop. Only gates entry from
     * STOPPED — during preheat/crank a booting ECU may briefly broadcast
     * bulb-check lamps, and once running the protection alarm takes over.
     * (Stale lamps drop in j1939_fill_inputs, so a reset after the ECU
     * sleeps is not blocked.) */
    bool start_inhibit =
        in->ecu_red_lamp && app->engine.state == ENG_STOPPED;
    engine_cmd_t cmd = {
        .start_requested = want_run && !shutdown && !start_inhibit,
        .stop_requested = !want_run,
        .immediate_stop = shutdown || in->emergency_stop,
        .skip_cooldown = !in->mode_auto, /* manual stop = operator intent */
        .alarm_reset = alarm_reset,
    };
    engine_fsm_tick(&app->engine, &app->cfg, in, &cmd, out);

    /* Operator alarm reset only acts when the machine is safe. */
    if (alarm_reset &&
        (app->engine.state == ENG_STOPPED ||
         app->engine.state == ENG_SHUTDOWN)) {
        protection_reset(&app->prot);
    }

    /* 5. Transfer switches. Gen side additionally gated on the engine
     * actually being in RUNNING and alarm-free (defence in depth). */
    bool want_gen = demand.gen_contactor && engine_available;
    bool want_mains = demand.mains_contactor;
    if (want_gen && want_mains) {
        want_gen = false; /* never both — final backstop */
        want_mains = false;
    }
    /* Break-before-make on EVERY handover path (shutdown, mode switch,
     * engine death — not just the AMF's own transfer states): a contactor
     * may close only after the other has been commanded open for the full
     * dead time. Physical contacts take 20–50 ms to drop out. */
    uint32_t brk = GCU_MS_TO_TICKS(app->cfg.transfer_break_ms);
    if (want_gen && app->mains_open_ticks < brk) {
        want_gen = false;
    }
    if (want_mains && app->gen_open_ticks < brk) {
        want_mains = false;
    }
    out->gen_contactor = want_gen;
    out->mains_contactor = want_mains;
    if (want_gen) {
        app->gen_open_ticks = 0;
    } else if (app->gen_open_ticks < UINT32_MAX) {
        app->gen_open_ticks++;
    }
    if (want_mains) {
        app->mains_open_ticks = 0;
    } else if (app->mains_open_ticks < UINT32_MAX) {
        app->mains_open_ticks++;
    }

    aux_apply(app, out);
    backlight_apply(app, in, out);
}
