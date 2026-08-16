/*
 * test_all.c — unit + scenario tests for the pure logic modules.
 * Plain C, no framework: CHECK() prints and counts failures.
 */
#include <stdio.h>
#include <string.h>

#include "../core/gcu_app.h"
#include "../core/j1939.h"
#include "../sim/plant.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                  \
    } while (0)

/* Run app+plant together for a given number of ticks. */
static void run(gcu_app_t *app, plant_t *plant, gcu_inputs_t *in,
                gcu_outputs_t *out, uint32_t ticks, bool auto_mode)
{
    for (uint32_t i = 0; i < ticks; i++) {
        plant_step(plant, out, in);
        in->mode_auto = auto_mode;
        gcu_app_tick(app, in, out);
        CHECK(!(out->gen_contactor && out->mains_contactor));
    }
}

static void fresh(gcu_app_t *app, plant_t *plant, gcu_inputs_t *in,
                  gcu_outputs_t *out)
{
    gcu_app_init(app);
    plant_init(plant);
    memset(in, 0, sizeof(*in));
    memset(out, 0, sizeof(*out));
}

/* ---------------------------------------------------------------- tests */

static void test_auto_start_on_mains_fail(void)
{
    gcu_app_t app; plant_t plant; gcu_inputs_t in; gcu_outputs_t out;
    fresh(&app, &plant, &in, &out);

    run(&app, &plant, &in, &out, 500, true); /* 5 s healthy */
    CHECK(app.engine.state == ENG_STOPPED);
    CHECK(out.mains_contactor);

    plant.mains_on = false;
    run(&app, &plant, &in, &out, 4000, true); /* 40 s */
    CHECK(engine_fsm_running(&app.engine));
    CHECK(app.amf.state == AMF_ON_GEN);
    CHECK(out.gen_contactor && !out.mains_contactor);

    plant.mains_on = true;
    app.cfg.mains_return_qualify_ms = 5000;
    app.cfg.cooldown_ms = 5000;
    run(&app, &plant, &in, &out, 3000, true); /* 30 s */
    CHECK(app.engine.state == ENG_STOPPED);
    CHECK(out.mains_contactor && !out.gen_contactor);
}

static void test_fail_to_start_after_attempts(void)
{
    gcu_app_t app; plant_t plant; gcu_inputs_t in; gcu_outputs_t out;
    fresh(&app, &plant, &in, &out);
    plant.fuel_blocked = true;
    plant.mains_on = false;

    /* preheat 5s + 3×(crank 8s + rest 10s) ≈ 60 s; run 90 s */
    run(&app, &plant, &in, &out, 9000, true);
    CHECK(app.engine.state == ENG_SHUTDOWN);
    CHECK(app.prot.active[ALARM_FAIL_TO_START]);
    CHECK(!out.starter && !out.run_enable);
    CHECK(!out.gen_contactor);
}

static void test_low_oil_shutdown(void)
{
    gcu_app_t app; plant_t plant; gcu_inputs_t in; gcu_outputs_t out;
    fresh(&app, &plant, &in, &out);
    plant.mains_on = false;
    run(&app, &plant, &in, &out, 4000, true);
    CHECK(app.engine.state == ENG_RUNNING);

    plant.oil_pump_broken = true;
    run(&app, &plant, &in, &out, 600, true); /* > 2 s delay + margin */
    CHECK(app.prot.active[ALARM_LOW_OIL_PRESSURE]);
    CHECK(app.engine.state == ENG_SHUTDOWN);
    CHECK(!out.run_enable);
    CHECK(!out.gen_contactor);
}

static void test_estop_immediate(void)
{
    gcu_app_t app; plant_t plant; gcu_inputs_t in; gcu_outputs_t out;
    fresh(&app, &plant, &in, &out);
    plant.mains_on = false;
    run(&app, &plant, &in, &out, 4000, true);
    CHECK(app.engine.state == ENG_RUNNING);

    /* Inject e-stop: plant fills inputs, we override the switch. */
    for (int i = 0; i < 5; i++) {
        plant_step(&plant, &out, &in);
        in.mode_auto = true;
        in.emergency_stop = true;
        gcu_app_tick(&app, &in, &out);
    }
    CHECK(app.engine.state == ENG_SHUTDOWN);
    CHECK(!out.run_enable && !out.starter);
    CHECK(app.prot.active[ALARM_EMERGENCY_STOP]);
}

static void test_overspeed_shutdown(void)
{
    gcu_app_t app; plant_t plant; gcu_inputs_t in; gcu_outputs_t out;
    fresh(&app, &plant, &in, &out);
    plant.mains_on = false;
    run(&app, &plant, &in, &out, 4000, true);
    CHECK(app.engine.state == ENG_RUNNING);

    plant.governor_runaway = true;
    run(&app, &plant, &in, &out, 300, true);
    CHECK(app.prot.active[ALARM_OVERSPEED]);
    CHECK(app.engine.state == ENG_SHUTDOWN);
}

static void test_charge_fail_warning_not_shutdown(void)
{
    gcu_app_t app; plant_t plant; gcu_inputs_t in; gcu_outputs_t out;
    fresh(&app, &plant, &in, &out);
    plant.mains_on = false;
    plant.charge_alt_broken = true;
    run(&app, &plant, &in, &out, 7000, true); /* 70 s */
    CHECK(app.prot.active[ALARM_CHARGE_FAIL]);
    CHECK(engine_fsm_running(&app.engine)); /* warning: keeps running */
    CHECK(out.horn);
}

static void test_manual_mode(void)
{
    gcu_app_t app; plant_t plant; gcu_inputs_t in; gcu_outputs_t out;
    fresh(&app, &plant, &in, &out);

    /* MANUAL: key_start runs engine; no transfer happens by itself. */
    for (int i = 0; i < 4000; i++) {
        plant_step(&plant, &out, &in);
        in.mode_auto = false;
        in.key_start = (i == 100);
        gcu_app_tick(&app, &in, &out);
        CHECK(!(out.gen_contactor && out.mains_contactor));
        CHECK(!out.gen_contactor); /* manual never closes gen contactor */
    }
    CHECK(engine_fsm_running(&app.engine));

    /* key_stop stops without cooldown. */
    for (int i = 0; i < 3000; i++) {
        plant_step(&plant, &out, &in);
        in.mode_auto = false;
        in.key_stop = (i == 10);
        gcu_app_tick(&app, &in, &out);
    }
    CHECK(app.engine.state == ENG_STOPPED);
}

static void test_alarm_reset_flow(void)
{
    gcu_app_t app; plant_t plant; gcu_inputs_t in; gcu_outputs_t out;
    fresh(&app, &plant, &in, &out);
    plant.mains_on = false;
    plant.fuel_blocked = true;
    run(&app, &plant, &in, &out, 9000, true);
    CHECK(app.engine.state == ENG_SHUTDOWN);

    /* Reset in AUTO must be refused (would cause unattended crank). */
    for (int i = 0; i < 3; i++) {
        plant_step(&plant, &out, &in);
        in.mode_auto = true;
        in.key_stop = true;
        gcu_app_tick(&app, &in, &out);
    }
    CHECK(protection_shutdown_active(&app.prot)); /* still latched */
    CHECK(app.engine.state == ENG_SHUTDOWN);

    /* Fix fault, switch to MANUAL, press STOP: reset accepted. */
    plant.fuel_blocked = false;
    for (int i = 0; i < 3; i++) {
        plant_step(&plant, &out, &in);
        in.mode_auto = false;
        in.key_stop = true;
        gcu_app_tick(&app, &in, &out);
    }
    CHECK(!protection_shutdown_active(&app.prot));
    CHECK(app.engine.state == ENG_STOPPED);

    /* Back to AUTO: deliberate re-arm; mains still failed → restart. */
    run(&app, &plant, &in, &out, 4000, true);
    CHECK(engine_fsm_running(&app.engine));
}

/* Measure the gap (ticks) between the gen contactor opening and the mains
 * contactor closing, across any handover path. */
static uint32_t measure_gen_to_mains_gap(gcu_app_t *app, plant_t *plant,
                                         gcu_inputs_t *in, gcu_outputs_t *out,
                                         uint32_t ticks, bool auto_mode,
                                         void (*mutate)(plant_t *, gcu_inputs_t *,
                                                        uint32_t))
{
    bool gen_was_on = out->gen_contactor; /* seed from pre-loop state */
    uint32_t gen_open_at = 0, tick_now = 0, gap = UINT32_MAX;
    for (uint32_t i = 0; i < ticks; i++) {
        tick_now++;
        plant_step(plant, out, in);
        in->mode_auto = auto_mode;
        if (mutate) mutate(plant, in, i);
        gcu_app_tick(app, in, out);
        CHECK(!(out->gen_contactor && out->mains_contactor));
        if (out->gen_contactor) {
            gen_was_on = true;
        } else if (gen_was_on) {
            gen_was_on = false;
            gen_open_at = tick_now;
        }
        if (out->mains_contactor && gen_open_at && gap == UINT32_MAX) {
            gap = tick_now - gen_open_at;
        }
    }
    return gap;
}

static void mutate_kill_oil(plant_t *p, gcu_inputs_t *in, uint32_t i)
{
    (void)in;
    if (i == 100) {
        p->mains_on = true;      /* mains live but unqualified */
        p->oil_pump_broken = true; /* force shutdown while ON_GEN */
    }
}

static void test_deadtime_on_shutdown_during_on_gen(void)
{
    gcu_app_t app; plant_t plant; gcu_inputs_t in; gcu_outputs_t out;
    fresh(&app, &plant, &in, &out);
    plant.mains_on = false;
    run(&app, &plant, &in, &out, 4000, true); /* reach ON_GEN */
    CHECK(out.gen_contactor);

    uint32_t brk = GCU_MS_TO_TICKS(app.cfg.transfer_break_ms);
    uint32_t gap = measure_gen_to_mains_gap(&app, &plant, &in, &out, 6000,
                                            true, mutate_kill_oil);
    CHECK(gap != UINT32_MAX); /* mains did eventually take over */
    CHECK(gap >= brk);        /* full dead time on the fault path */
}

static void test_deadtime_on_mode_switch(void)
{
    gcu_app_t app; plant_t plant; gcu_inputs_t in; gcu_outputs_t out;
    fresh(&app, &plant, &in, &out);
    plant.mains_on = false;
    run(&app, &plant, &in, &out, 4000, true);
    CHECK(out.gen_contactor);

    plant.mains_on = true; /* mains back, operator flips to MANUAL */
    uint32_t brk = GCU_MS_TO_TICKS(app.cfg.transfer_break_ms);
    uint32_t gap = measure_gen_to_mains_gap(&app, &plant, &in, &out, 2000,
                                            false, NULL);
    CHECK(gap != UINT32_MAX);
    CHECK(gap >= brk);
}

static void test_sensor_loss_no_crank_into_spinning(void)
{
    gcu_app_t app; plant_t plant; gcu_inputs_t in; gcu_outputs_t out;
    fresh(&app, &plant, &in, &out);
    plant.mains_on = false;
    run(&app, &plant, &in, &out, 4000, true);
    CHECK(app.engine.state == ENG_RUNNING);

    /* Lose rpm + oil sensors while running; then operator hammers reset. */
    for (int i = 0; i < 6000; i++) {
        plant_step(&plant, &out, &in);
        in.mode_auto = false; /* manual: reset allowed */
        in.rpm_valid = false;
        in.oil_pressure_valid = false;
        in.key_stop = true;
        gcu_app_tick(&app, &in, &out);
        /* Starter must never engage while the real engine spins. */
        CHECK(!(out.starter && plant.rpm > 400.0f));
    }
}

static void test_mains_fail_during_cooldown(void)
{
    gcu_app_t app; plant_t plant; gcu_inputs_t in; gcu_outputs_t out;
    fresh(&app, &plant, &in, &out);
    app.cfg.mains_return_qualify_ms = 5000;
    plant.mains_on = false;
    run(&app, &plant, &in, &out, 4000, true);
    CHECK(app.amf.state == AMF_ON_GEN);

    plant.mains_on = true; /* returns → retransfer → cooldown */
    run(&app, &plant, &in, &out, 1200, true);
    CHECK(app.engine.state == ENG_COOLDOWN);

    plant.mains_on = false; /* fails again during cooldown */
    uint32_t starter_ticks = 0;
    for (int i = 0; i < 2500; i++) {
        plant_step(&plant, &out, &in);
        in.mode_auto = true;
        gcu_app_tick(&app, &in, &out);
        CHECK(!(out.gen_contactor && out.mains_contactor));
        if (out.starter) starter_ticks++;
    }
    CHECK(starter_ticks == 0); /* engine never left RUNNING band: no re-crank */
    CHECK(app.amf.state == AMF_ON_GEN);
    CHECK(app.engine.state == ENG_RUNNING);
}

static void test_fail_to_stop(void)
{
    gcu_app_t app; plant_t plant; gcu_inputs_t in; gcu_outputs_t out;
    fresh(&app, &plant, &in, &out);
    plant.mains_on = false;
    run(&app, &plant, &in, &out, 4000, true);
    CHECK(app.engine.state == ENG_RUNNING);

    plant.stuck_fuel = true; /* runaway: fuel cut has no effect */
    plant.mains_on = true;
    app.cfg.mains_return_qualify_ms = 5000;
    app.cfg.cooldown_ms = 2000;
    run(&app, &plant, &in, &out, 7000, true); /* retransfer + stop attempt */
    /* 45 s stop timeout: total = qualify 5 s + cooldown 2 s + 45 s */
    run(&app, &plant, &in, &out, 5000, true);
    CHECK(app.prot.active[ALARM_FAIL_TO_STOP]);
    CHECK(app.engine.state == ENG_SHUTDOWN);
}

static void test_estop_not_resettable_while_pressed(void)
{
    gcu_app_t app; plant_t plant; gcu_inputs_t in; gcu_outputs_t out;
    fresh(&app, &plant, &in, &out);
    plant.mains_on = false;
    run(&app, &plant, &in, &out, 4000, true);

    for (int i = 0; i < 200; i++) {
        plant_step(&plant, &out, &in);
        in.mode_auto = false;
        in.emergency_stop = true; /* held pressed */
        in.key_stop = true;       /* operator tries to reset */
        gcu_app_tick(&app, &in, &out);
    }
    CHECK(app.prot.active[ALARM_EMERGENCY_STOP]); /* not resettable */
    CHECK(!out.run_enable);
}

static void test_remote_start_runs_offload(void)
{
    gcu_app_t app; plant_t plant; gcu_inputs_t in; gcu_outputs_t out;
    fresh(&app, &plant, &in, &out);

    for (int i = 0; i < 6000; i++) { /* mains healthy throughout */
        plant_step(&plant, &out, &in);
        in.mode_auto = true;
        in.remote_start = true;
        gcu_app_tick(&app, &in, &out);
        CHECK(!out.gen_contactor); /* off-load test run */
    }
    CHECK(engine_fsm_running(&app.engine));
    CHECK(out.mains_contactor); /* load never left mains */
}

static void test_crank_disconnect_time(void)
{
    gcu_app_t app; plant_t plant; gcu_inputs_t in; gcu_outputs_t out;
    fresh(&app, &plant, &in, &out);
    plant.mains_on = false;

    uint32_t starter_ticks = 0;
    for (int i = 0; i < 4000; i++) {
        plant_step(&plant, &out, &in);
        in.mode_auto = true;
        gcu_app_tick(&app, &in, &out);
        if (out.starter) starter_ticks++;
    }
    /* Healthy engine fires quickly: starter must engage well under 3 s. */
    CHECK(starter_ticks > 0);
    CHECK(starter_ticks < 300);
}

/* ------------------------------------------------------------ J1939 tests */

/* J1939 mode: engine data comes ONLY from the fake ECU's frames; the
 * analog values plant_step wrote are overwritten every tick. */
static void run_j1939(gcu_app_t *app, plant_t *plant, j1939_state_t *j,
                      gcu_inputs_t *in, gcu_outputs_t *out, uint32_t ticks,
                      bool auto_mode)
{
    for (uint32_t i = 0; i < ticks; i++) {
        plant_step(plant, out, in);
        j1939_frame_t fr[4];
        j1939_frame_t tx;
        int n = plant_j1939_emit(plant, out, fr, 4);
        for (int k = 0; k < n; k++) {
            j1939_rx(j, &fr[k]);
        }
        j1939_tick(j, &tx);
        j1939_fill_inputs(j, out->run_enable, in);
        in->mode_auto = auto_mode;
        gcu_app_tick(app, in, out);
        CHECK(!(out->gen_contactor && out->mains_contactor));
    }
}

static void test_j1939_spn_scaling(void)
{
    j1939_state_t j;
    j1939_init(&j, J1939_ADDR_GENSET_CONTROLLER, J1939_ADDR_ENGINE_1);

    /* EEC1: 1500 rpm -> raw 12000 = 0x2EE0, bytes 4-5 LE, SA 0x00 */
    j1939_frame_t f = {.id = 0x18F00400U, .dlc = 8,
                       .data = {0xFF, 0xFF, 0xFF, 0xE0, 0x2E, 0xFF, 0xFF, 0xFF}};
    j1939_rx(&j, &f);
    CHECK(j.rpm > 1499.9f && j.rpm < 1500.1f);
    CHECK(j.rpm_age == 0);

    /* ET1: 90 C -> raw 130 */
    j1939_frame_t f2 = {.id = 0x18FEEE00U, .dlc = 8, .data = {130}};
    j1939_rx(&j, &f2);
    CHECK(j.coolant_c > 89.9f && j.coolant_c < 90.1f);

    /* EFL/P1: 4.0 bar = 400 kPa -> raw 100 in byte 4 */
    j1939_frame_t f3 = {.id = 0x18FEEF00U, .dlc = 8,
                        .data = {0xFF, 0xFF, 0xFF, 100, 0xFF, 0xFF, 0xFF, 0xFF}};
    j1939_rx(&j, &f3);
    CHECK(j.oil_bar > 3.99f && j.oil_bar < 4.01f);

    /* not-available rpm (0xFFFF) must NOT update the value or the age */
    j1939_frame_t f4 = {.id = 0x18F00400U, .dlc = 8,
                        .data = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
    j1939_frame_t tx;
    j1939_tick(&j, &tx); /* age 1 */
    j1939_rx(&j, &f4);
    CHECK(j.rpm > 1499.9f);
    CHECK(j.rpm_age == 1);

    /* frames from a different source address are ignored */
    j1939_frame_t f5 = {.id = 0x18F00417U, .dlc = 8,
                        .data = {0xFF, 0xFF, 0xFF, 0x00, 0x10, 0xFF, 0xFF, 0xFF}};
    j1939_rx(&j, &f5);
    CHECK(j.rpm > 1499.9f);
}

static void test_j1939_auto_cycle(void)
{
    gcu_app_t app; plant_t plant; gcu_inputs_t in; gcu_outputs_t out;
    j1939_state_t j;
    fresh(&app, &plant, &in, &out);
    j1939_init(&j, J1939_ADDR_GENSET_CONTROLLER, J1939_ADDR_ENGINE_1);

    run_j1939(&app, &plant, &j, &in, &out, 500, true);
    CHECK(app.engine.state == ENG_STOPPED);
    CHECK(!in.rpm_valid); /* engine ECU unpowered, silence is normal */
    CHECK(!in.ecu_comms_lost);

    plant.mains_on = false;
    run_j1939(&app, &plant, &j, &in, &out, 4000, true);
    CHECK(engine_fsm_running(&app.engine));
    CHECK(app.amf.state == AMF_ON_GEN);
    CHECK(in.rpm_valid);
    CHECK(in.rpm > 1400.0f && in.rpm < 1600.0f);
    CHECK(in.oil_pressure_valid);
    CHECK(in.coolant_temp_valid);
    CHECK(!app.prot.active[ALARM_ECU_COMMS_LOST]);

    plant.mains_on = true;
    app.cfg.mains_return_qualify_ms = 5000;
    app.cfg.cooldown_ms = 5000;
    run_j1939(&app, &plant, &j, &in, &out, 3000, true);
    CHECK(app.engine.state == ENG_STOPPED);
    CHECK(out.mains_contactor && !out.gen_contactor);
}

static void test_j1939_comms_lost(void)
{
    gcu_app_t app; plant_t plant; gcu_inputs_t in; gcu_outputs_t out;
    j1939_state_t j;
    fresh(&app, &plant, &in, &out);
    j1939_init(&j, J1939_ADDR_GENSET_CONTROLLER, J1939_ADDR_ENGINE_1);
    plant.mains_on = false;
    run_j1939(&app, &plant, &j, &in, &out, 4000, true);
    CHECK(app.engine.state == ENG_RUNNING);

    plant.j1939_silent = true; /* harness cut while running */
    run_j1939(&app, &plant, &j, &in, &out, 200, true); /* 2 s */
    CHECK(!in.rpm_valid);
    CHECK(app.prot.active[ALARM_ECU_COMMS_LOST]); /* warning at 1 s */
    CHECK(engine_fsm_running(&app.engine));       /* not yet shut down */

    run_j1939(&app, &plant, &j, &in, &out, 200, true); /* total 4 s */
    CHECK(app.prot.active[ALARM_SENSOR_LOSS]);    /* shutdown at 3 s */
    CHECK(!out.run_enable);
}

static void test_j1939_red_lamp_shutdown(void)
{
    gcu_app_t app; plant_t plant; gcu_inputs_t in; gcu_outputs_t out;
    j1939_state_t j;
    fresh(&app, &plant, &in, &out);
    j1939_init(&j, J1939_ADDR_GENSET_CONTROLLER, J1939_ADDR_ENGINE_1);
    plant.mains_on = false;
    run_j1939(&app, &plant, &j, &in, &out, 4000, true);
    CHECK(app.engine.state == ENG_RUNNING);

    plant.dm1_red_lamp = true;
    run_j1939(&app, &plant, &j, &in, &out, 300, true); /* DM1 1s + qual 0.5s */
    CHECK(app.prot.active[ALARM_ECU_RED_LAMP]);
    CHECK(!out.run_enable);
    CHECK(j.dtc_spn == 100 && j.dtc_fmi == 1);
}

static void test_j1939_amber_is_warning_only(void)
{
    gcu_app_t app; plant_t plant; gcu_inputs_t in; gcu_outputs_t out;
    j1939_state_t j;
    fresh(&app, &plant, &in, &out);
    j1939_init(&j, J1939_ADDR_GENSET_CONTROLLER, J1939_ADDR_ENGINE_1);
    plant.mains_on = false;
    plant.dm1_amber_lamp = true;
    run_j1939(&app, &plant, &j, &in, &out, 4000, true);
    CHECK(app.prot.active[ALARM_ECU_WARNING]);
    CHECK(engine_fsm_running(&app.engine)); /* keeps running */
    CHECK(app.amf.state == AMF_ON_GEN);
}

static void test_j1939_address_claim(void)
{
    j1939_state_t j;
    j1939_frame_t tx;
    j1939_init(&j, J1939_ADDR_GENSET_CONTROLLER, J1939_ADDR_ENGINE_1);

    /* power-up claim */
    CHECK(j1939_tick(&j, &tx));
    CHECK(j1939_pgn(tx.id) == 60928U);
    CHECK((tx.id & 0xFFU) == J1939_ADDR_GENSET_CONTROLLER);
    CHECK(!j1939_tick(&j, &tx)); /* sent once */

    /* competitor with HIGHER name -> we defend (re-claim) */
    j1939_frame_t rival = {.id = 0x18EEFF00U | J1939_ADDR_GENSET_CONTROLLER,
                           .dlc = 8,
                           .data = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
    j1939_rx(&j, &rival);
    CHECK(!j.addr_conflict);
    CHECK(j1939_tick(&j, &tx));

    /* competitor with LOWER name -> we lose and go silent */
    memset(rival.data, 0x00, 8);
    j1939_rx(&j, &rival);
    CHECK(j.addr_conflict);
    CHECK(!j1939_tick(&j, &tx));
}

static void test_j1939_stale_lamps_drop(void)
{
    j1939_state_t j;
    j1939_frame_t tx;
    gcu_inputs_t in;
    memset(&in, 0, sizeof(in));
    j1939_init(&j, J1939_ADDR_GENSET_CONTROLLER, J1939_ADDR_ENGINE_1);

    j1939_frame_t dm1 = {.id = 0x18FECA00U, .dlc = 8,
                         .data = {0x10, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF}};
    j1939_rx(&j, &dm1);
    j1939_fill_inputs(&j, true, &in);
    CHECK(in.ecu_red_lamp);

    for (uint32_t i = 0; i <= J1939_DM1_TIMEOUT_TICKS; i++) {
        j1939_tick(&j, &tx);
    }
    j1939_fill_inputs(&j, true, &in);
    CHECK(!in.ecu_red_lamp); /* stale severity is no severity */
    CHECK(in.ecu_comms_lost);
}

int main(void)
{
    test_auto_start_on_mains_fail();
    test_fail_to_start_after_attempts();
    test_low_oil_shutdown();
    test_estop_immediate();
    test_overspeed_shutdown();
    test_charge_fail_warning_not_shutdown();
    test_manual_mode();
    test_alarm_reset_flow();
    test_crank_disconnect_time();
    test_deadtime_on_shutdown_during_on_gen();
    test_deadtime_on_mode_switch();
    test_sensor_loss_no_crank_into_spinning();
    test_mains_fail_during_cooldown();
    test_fail_to_stop();
    test_estop_not_resettable_while_pressed();
    test_remote_start_runs_offload();
    test_j1939_spn_scaling();
    test_j1939_auto_cycle();
    test_j1939_comms_lost();
    test_j1939_red_lamp_shutdown();
    test_j1939_amber_is_warning_only();
    test_j1939_address_claim();
    test_j1939_stale_lamps_drop();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
