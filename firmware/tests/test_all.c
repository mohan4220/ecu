/*
 * test_all.c — unit + scenario tests for the pure logic modules.
 * Plain C, no framework: CHECK() prints and counts failures.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../core/ac_sense.h"
#include "../core/sensors.h"
#include "../core/ecu_main.h"
#include "../core/gcu_app.h"
#include "../core/j1939.h"
#include "../core/modbus.h"
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
    CHECK(out.aux1);   /* AUX1 defaults to AUX_HORN */
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
        j1939_tick(j, out->run_enable, &tx);
        j1939_fill_inputs(j, in);
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
    j1939_tick(&j, false, &tx); /* age 1 */
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
    CHECK(j1939_tick(&j, false, &tx));
    CHECK(j1939_pgn(tx.id) == 60928U);
    CHECK((tx.id & 0xFFU) == J1939_ADDR_GENSET_CONTROLLER);
    CHECK(!j1939_tick(&j, false, &tx)); /* sent once */

    /* competitor with HIGHER name -> we defend (re-claim) */
    j1939_frame_t rival = {.id = 0x18EEFF00U | J1939_ADDR_GENSET_CONTROLLER,
                           .dlc = 8,
                           .data = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F}};
    j1939_rx(&j, &rival);
    CHECK(!j.addr_conflict);
    CHECK(j1939_tick(&j, false, &tx));
    CHECK((tx.id & 0xFFU) == J1939_ADDR_GENSET_CONTROLLER);

    /* competitor with LOWER name -> we lose and announce cannot-claim */
    memset(rival.data, 0x00, 8);
    j1939_rx(&j, &rival);
    CHECK(j.addr_conflict);
    CHECK(j1939_tick(&j, false, &tx));
    CHECK((tx.id & 0xFFU) == J1939_ADDR_NULL);
    CHECK(!j1939_tick(&j, false, &tx));

    /* request-for-address-claimed while conflicted -> cannot-claim again */
    j1939_frame_t rq = {.id = 0x18EAFF00U | 0x17U, .dlc = 3,
                        .data = {0x00, 0xEE, 0x00}};
    j1939_rx(&j, &rq);
    CHECK(j1939_tick(&j, false, &tx));
    CHECK((tx.id & 0xFFU) == J1939_ADDR_NULL);

    /* EQUAL name (cloned unit) counts as a conflict, not a defence */
    j1939_state_t j2;
    j1939_init(&j2, J1939_ADDR_GENSET_CONTROLLER, J1939_ADDR_ENGINE_1);
    j1939_tick(&j2, false, &tx); /* drain power-up claim */
    j1939_frame_t clone = {.id = 0x18EEFF00U | J1939_ADDR_GENSET_CONTROLLER,
                           .dlc = 8, .data = {0}};
    for (int i = 0; i < 8; i++) {
        clone.data[i] = (uint8_t)(j2.name >> (8 * i));
    }
    j1939_rx(&j2, &clone);
    CHECK(j2.addr_conflict);

    /* destination-specific request for ANOTHER node: no answer */
    j1939_state_t j3;
    j1939_init(&j3, J1939_ADDR_GENSET_CONTROLLER, J1939_ADDR_ENGINE_1);
    j1939_tick(&j3, false, &tx); /* drain power-up claim */
    j1939_frame_t rq_other = {.id = 0x18EA5A00U | 0x17U, .dlc = 3,
                              .data = {0x00, 0xEE, 0x00}};
    j1939_rx(&j3, &rq_other);
    CHECK(!j1939_tick(&j3, false, &tx));
}

static void test_j1939_red_lamp_blocks_start(void)
{
    /* An awake engine ECU broadcasting a red stop lamp: the set must
     * refuse to crank at all. Frames injected directly — the plant's
     * fake ECU sleeps while the engine is off. */
    gcu_app_t app; plant_t plant; gcu_inputs_t in; gcu_outputs_t out;
    j1939_state_t j;
    fresh(&app, &plant, &in, &out);
    j1939_init(&j, J1939_ADDR_GENSET_CONTROLLER, J1939_ADDR_ENGINE_1);
    plant.mains_on = false; /* AMF wants the engine */

    j1939_frame_t dm1 = {.id = 0x18FECA00U, .dlc = 8,
                         .data = {0x10, 0xFF, 100, 0, 1, 1, 0xFF, 0xFF}};
    j1939_frame_t tx;
    uint32_t starter_ticks = 0;
    for (uint32_t i = 0; i < 4000; i++) { /* 40 s */
        plant_step(&plant, &out, &in);
        if (i % 100 == 0) {
            j1939_rx(&j, &dm1);
        }
        j1939_tick(&j, out.run_enable, &tx);
        j1939_fill_inputs(&j, &in);
        in.mode_auto = true;
        gcu_app_tick(&app, &in, &out);
        starter_ticks += out.starter;
    }
    CHECK(starter_ticks == 0);
    CHECK(app.engine.state == ENG_STOPPED);
}

static void test_j1939_reset_after_ecu_sleeps(void)
{
    /* Shutdown while running; the engine ECU broadcasts the spin-down to
     * 0 rpm and then sleeps. An operator reset minutes later must be
     * accepted (standstill was seen), not held for the 45 s timeout. */
    gcu_app_t app; plant_t plant; gcu_inputs_t in; gcu_outputs_t out;
    j1939_state_t j;
    fresh(&app, &plant, &in, &out);
    j1939_init(&j, J1939_ADDR_GENSET_CONTROLLER, J1939_ADDR_ENGINE_1);
    plant.mains_on = false;
    run_j1939(&app, &plant, &j, &in, &out, 4000, true);
    CHECK(app.engine.state == ENG_RUNNING);

    /* plant_step clears the input struct, so per-tick flags are set
     * inside the loop (same pattern as the legacy e-stop tests) */
    j1939_frame_t fr[4];
    j1939_frame_t tx;
    for (uint32_t i = 0; i < 700; i++) { /* e-stop + spin-down, 7 s */
        plant_step(&plant, &out, &in);
        int n = plant_j1939_emit(&plant, &out, fr, 4);
        for (int k = 0; k < n; k++) {
            j1939_rx(&j, &fr[k]);
        }
        j1939_tick(&j, out.run_enable, &tx);
        j1939_fill_inputs(&j, &in);
        in.mode_auto = true;
        in.emergency_stop = true;
        gcu_app_tick(&app, &in, &out);
    }
    CHECK(app.engine.state == ENG_SHUTDOWN);
    CHECK(app.engine.standstill_seen);
    CHECK(!in.rpm_valid); /* ECU asleep again */

    for (uint32_t i = 0; i < 50; i++) { /* reset well before 45 s timeout */
        plant_step(&plant, &out, &in);
        j1939_tick(&j, out.run_enable, &tx);
        j1939_fill_inputs(&j, &in);
        in.mode_auto = false;
        in.key_stop = true;
        gcu_app_tick(&app, &in, &out);
    }
    CHECK(app.engine.state == ENG_STOPPED);
    CHECK(!protection_shutdown_active(&app.prot));
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
    j1939_fill_inputs(&j, &in);
    CHECK(in.ecu_red_lamp);

    for (uint32_t i = 0; i <= J1939_DM1_TIMEOUT_TICKS; i++) {
        j1939_tick(&j, true, &tx);
    }
    j1939_fill_inputs(&j, &in);
    CHECK(!in.ecu_red_lamp); /* stale severity is no severity */
    CHECK(in.ecu_comms_lost); /* run-enable held past the boot grace */
}


/* ------------------------------------------------------------- Modbus RTU */

/* Build a request with a correct CRC appended. */
static size_t mb_req(uint8_t *b, size_t n)
{
    uint16_t c = modbus_crc(b, n);
    b[n] = (uint8_t)(c & 0xFF);
    b[n + 1] = (uint8_t)(c >> 8);
    return n + 2;
}

static void test_modbus_crc(void)
{
    /* Known-answer vector: the classic 01 04 02 FF FF frame CRC is 0xB880
     * (low byte first on the wire). Verified against the Modbus spec. */
    uint8_t f[] = {0x01, 0x04, 0x02, 0xFF, 0xFF};
    CHECK(modbus_crc(f, sizeof(f)) == 0x80B8);

    /* CRC of a frame including its own CRC is always zero — the property a
     * receiver can rely on. */
    uint8_t g[8];
    memcpy(g, f, sizeof(f));
    size_t n = mb_req(g, sizeof(f));
    CHECK(modbus_crc(g, n) == 0);
}

static void test_modbus_read_input_regs(void)
{
    modbus_t mb;
    modbus_init(&mb, 17);
    uint16_t iregs[MODBUS_IREG_COUNT];
    memset(iregs, 0, sizeof(iregs));
    iregs[10] = 1500;   /* rpm */
    iregs[14] = 1280;   /* 12.80 V */

    uint8_t req[16], resp[64];
    req[0] = 17; req[1] = 0x04;
    req[2] = 0; req[3] = 10;   /* start 10 */
    req[4] = 0; req[5] = 5;    /* 5 regs   */
    size_t n = mb_req(req, 6);

    size_t r = modbus_rx(&mb, req, n, iregs, resp, sizeof(resp));
    CHECK(r == 3 + 10 + 2);
    CHECK(resp[0] == 17 && resp[1] == 0x04 && resp[2] == 10);
    CHECK(((resp[3] << 8) | resp[4]) == 1500);           /* reg 10 */
    CHECK(((resp[11] << 8) | resp[12]) == 1280);         /* reg 14 */
    CHECK(modbus_crc(resp, r) == 0);
}

static void test_modbus_addressing(void)
{
    modbus_t mb;
    modbus_init(&mb, 17);
    uint16_t iregs[MODBUS_IREG_COUNT] = {0};
    uint8_t req[16], resp[64];

    /* Wrong slave address: silence, and not counted as ours. */
    req[0] = 18; req[1] = 0x04; req[2] = 0; req[3] = 0; req[4] = 0; req[5] = 1;
    size_t n = mb_req(req, 6);
    CHECK(modbus_rx(&mb, req, n, iregs, resp, sizeof(resp)) == 0);
    CHECK(mb.rx_frames == 0);

    /* Bad CRC: silence, counted as a CRC error. */
    req[0] = 17;
    n = mb_req(req, 6);
    req[n - 1] ^= 0xFF;
    CHECK(modbus_rx(&mb, req, n, iregs, resp, sizeof(resp)) == 0);
    CHECK(mb.crc_errors == 1);

    /* Out-of-range read: exception 02, not a silent drop. */
    req[0] = 17; req[1] = 0x04; req[2] = 0; req[3] = 200; req[4] = 0; req[5] = 1;
    n = mb_req(req, 6);
    size_t r = modbus_rx(&mb, req, n, iregs, resp, sizeof(resp));
    CHECK(r == 5);
    CHECK(resp[1] == 0x84 && resp[2] == MODBUS_EX_ILLEGAL_ADDR);

    /* Unsupported function: exception 01. */
    req[1] = 0x08;
    n = mb_req(req, 6);
    r = modbus_rx(&mb, req, n, iregs, resp, sizeof(resp));
    CHECK(r == 5 && resp[1] == 0x88 && resp[2] == MODBUS_EX_ILLEGAL_FN);
}

static void test_modbus_write_and_commands(void)
{
    modbus_t mb;
    modbus_init(&mb, 17);
    uint16_t iregs[MODBUS_IREG_COUNT] = {0};
    uint8_t req[32], resp[64];

    /* Write mode = 1 (MANUAL); echo comes back. */
    req[0] = 17; req[1] = 0x06; req[2] = 0; req[3] = 0; req[4] = 0; req[5] = 1;
    size_t n = mb_req(req, 6);
    size_t r = modbus_rx(&mb, req, n, iregs, resp, sizeof(resp));
    CHECK(r == 8 && mb.hold[0] == 1);

    /* Illegal mode value is rejected and does NOT change state. */
    req[5] = 9;
    n = mb_req(req, 6);
    r = modbus_rx(&mb, req, n, iregs, resp, sizeof(resp));
    CHECK(r == 5 && resp[2] == MODBUS_EX_ILLEGAL_VALUE);
    CHECK(mb.hold[0] == 1);

    /* Alarm reset is an edge command: it latches, and reads back 0. */
    req[3] = 2; req[5] = 1;
    n = mb_req(req, 6);
    (void)modbus_rx(&mb, req, n, iregs, resp, sizeof(resp));
    CHECK(mb.cmd_alarm_reset);
    CHECK(mb.hold[2] == 0);

    /* Broadcast writes are executed but never answered. */
    modbus_init(&mb, 17);
    req[0] = 0; req[1] = 0x06; req[2] = 0; req[3] = 0; req[4] = 0; req[5] = 3;
    n = mb_req(req, 6);
    CHECK(modbus_rx(&mb, req, n, iregs, resp, sizeof(resp)) == 0);
    CHECK(mb.hold[0] == 3);
}

static void test_modbus_write_multiple_is_atomic(void)
{
    modbus_t mb;
    modbus_init(&mb, 17);
    uint16_t iregs[MODBUS_IREG_COUNT] = {0};
    uint8_t req[32], resp[64];

    /* Two registers, the SECOND one illegal. Nothing may be written. */
    req[0] = 17; req[1] = 0x10;
    req[2] = 0; req[3] = 0;     /* start 0            */
    req[4] = 0; req[5] = 2;     /* 2 registers        */
    req[6] = 4;                 /* 4 bytes            */
    req[7] = 0; req[8] = 1;     /* mode = 1 (legal)   */
    req[9] = 0; req[10] = 7;    /* remote start = 7 (illegal) */
    size_t n = mb_req(req, 11);
    size_t r = modbus_rx(&mb, req, n, iregs, resp, sizeof(resp));
    CHECK(r == 5 && resp[2] == MODBUS_EX_ILLEGAL_VALUE);
    CHECK(mb.hold[0] == 2);   /* still the AUTO default, not half-written */
    CHECK(mb.hold[1] == 0);

    /* Both legal: applied, and the echo carries start + count. */
    req[10] = 1;
    n = mb_req(req, 11);
    r = modbus_rx(&mb, req, n, iregs, resp, sizeof(resp));
    CHECK(r == 8 && mb.hold[0] == 1 && mb.hold[1] == 1);
    CHECK(((resp[4] << 8) | resp[5]) == 2);
}

static void test_modbus_publish_snapshot(void)
{
    gcu_app_t app;
    gcu_app_init(&app);
    gcu_inputs_t in;
    memset(&in, 0, sizeof(in));
    in.rpm = 1500.0f;            in.rpm_valid = true;
    in.oil_pressure_bar = 4.25f; in.oil_pressure_valid = true;
    in.coolant_temp_c = -5.0f;   in.coolant_temp_valid = true;
    in.battery_v = 13.8f;
    in.gen_v[0] = 230.5f;
    app.prot.active[ALARM_BATT_LOW] = true;

    uint16_t iregs[MODBUS_IREG_COUNT];
    modbus_publish(&in, &app, 1234, iregs);

    CHECK(iregs[0] == 2305);          /* 230.5 V in 0.1 V   */
    CHECK(iregs[10] == 1500);         /* rpm                */
    CHECK(iregs[11] == 425);          /* 4.25 bar           */
    CHECK((int16_t)iregs[12] == -50); /* -5.0 C, signed     */
    CHECK(iregs[14] == 1380);         /* 13.80 V            */
    CHECK(iregs[17] & (1u << ALARM_BATT_LOW));
    CHECK(iregs[21] == 1234);

    /* An invalid sensor must publish 0, never a stale or garbage reading. */
    in.rpm_valid = false;
    modbus_publish(&in, &app, 1234, iregs);
    CHECK(iregs[10] == 0);

    /* Power and PF: no AC sampling layer fills these yet, so they publish a
     * hard 0 meaning "not measured". Pinned here so the day something does
     * fill them, this test is what says the scaling is right. */
    CHECK(iregs[19] == 0);
    CHECK(iregs[20] == 0);
    in.real_power_w = 18400.0f;   /* 18.4 kW  */
    in.power_factor = 0.82f;
    modbus_publish(&in, &app, 1234, iregs);
    CHECK(iregs[19] == 184);      /* 0.1 kW steps  */
    CHECK(iregs[20] == 820);      /* 0.001 steps   */
}

/* A legal read that cannot fit the caller's buffer is a slave-side failure
 * (0x04), not the master's fault (0x03) — the master has no bad field to
 * correct, so ILLEGAL DATA VALUE would send it hunting (SME catch). */
static void test_modbus_short_buffer_is_slave_failure(void)
{
    modbus_t mb;
    modbus_init(&mb, 17);
    uint16_t iregs[MODBUS_IREG_COUNT];
    memset(iregs, 0, sizeof(iregs));
    uint8_t req[8], resp[16];

    req[0] = 17; req[1] = 0x04; req[2] = 0; req[3] = 0;
    req[4] = 0; req[5] = 5;            /* 5 registers = 15 bytes of answer */
    size_t n = mb_req(req, 6);
    size_t r = modbus_rx(&mb, req, n, iregs, resp, 8);
    CHECK(r == 5 && resp[2] == MODBUS_EX_SLAVE_FAILURE);

    /* With room, the same request succeeds. */
    r = modbus_rx(&mb, req, n, iregs, resp, sizeof(resp));
    CHECK(r == 15 && resp[1] == 0x04 && resp[2] == 10);
}


/* A healthy 12 V battery must never raise BATT_LOW. The 24 V-era thresholds
 * (22 V / 30 V) made the condition permanently true, which after the 60 s
 * qualification pinned the horn relay on for the life of the installation —
 * and no scenario test caught it because the plant was still a 24 V model. */
static void test_batt_thresholds_are_12v(void)
{
    gcu_app_t app;
    gcu_app_init(&app);
    gcu_inputs_t in;
    gcu_outputs_t out;
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.battery_v = 13.8f;      /* charging, entirely normal */
    in.mains_v[0] = in.mains_v[1] = in.mains_v[2] = 240.0f;
    in.mains_hz = 50.0f;

    /* 90 s — well past the 60 s alarm qualification. */
    for (int i = 0; i < 9000; i++) {
        gcu_app_tick(&app, &in, &out);
    }
    CHECK(!app.prot.active[ALARM_BATT_LOW]);
    CHECK(!app.prot.active[ALARM_BATT_HIGH]);
    CHECK(!out.aux1);          /* AUX1 defaults to AUX_HORN */

    /* A loaded-but-healthy 12 V battery is not an alarm. Without this the
     * low threshold could sit anywhere below 13.8 and still pass. */
    in.battery_v = 12.0f;
    for (int i = 0; i < 9000; i++) {
        gcu_app_tick(&app, &in, &out);
    }
    CHECK(!app.prot.active[ALARM_BATT_LOW]);

    /* Normal charging peak is not an alarm either. */
    in.battery_v = 15.0f;
    for (int i = 0; i < 9000; i++) {
        gcu_app_tick(&app, &in, &out);
    }
    CHECK(!app.prot.active[ALARM_BATT_LOW]);
    CHECK(!app.prot.active[ALARM_BATT_HIGH]);

    /* A genuinely flat 12 V battery still trips. */
    in.battery_v = 10.2f;
    for (int i = 0; i < 9000; i++) {
        gcu_app_tick(&app, &in, &out);
    }
    CHECK(app.prot.active[ALARM_BATT_LOW]);
    CHECK(out.aux1);
}

/* The high threshold needs its own coverage: the 24 V-era 30 V value could
 * never trip on a 12 V set, so a runaway regulator would have gone unreported
 * exactly where the 16 V TVS starts conducting. Mutating batt_high_v back to
 * 30.0f left the previous test passing (SME catch). */
static void test_batt_high_trips_on_runaway_regulator(void)
{
    gcu_app_t app;
    gcu_app_init(&app);
    gcu_inputs_t in;
    gcu_outputs_t out;
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.mains_v[0] = in.mains_v[1] = in.mains_v[2] = 240.0f;
    in.mains_hz = 50.0f;

    /* 16 V: regulator has lost control, and the SMCJ16CA is at its standoff. */
    in.battery_v = 16.0f;
    for (int i = 0; i < 9000; i++) {
        gcu_app_tick(&app, &in, &out);
    }
    CHECK(app.prot.active[ALARM_BATT_HIGH]);
    CHECK(!app.prot.active[ALARM_BATT_LOW]);
    CHECK(out.aux1);           /* horn follows any unacknowledged alarm */
}

/* ---------------------------------------------------------------- ac_sense */

/* Synthesise one measurement window of three-phase mains and push it through
 * the sampler. Voltages are L-N RMS, current is per-phase RMS, phi is the
 * current's lag in degrees (positive = inductive). */
static void ac_feed(ac_sense_t *ac, float v_rms, float i_rms, float hz,
                    float phi_deg, int windows)
{
    const float lsb = 3.3f / 4095.0f;
    float v_cnt = v_rms * 1.41421356f / (lsb * 235.9f); /* peak, in counts */
    float i_cnt = i_rms * 1.41421356f / (lsb / 0.1f * 10.0f);
    float phi = phi_deg * 3.14159265f / 180.0f;
    float w = 2.0f * 3.14159265f * hz / (float)ac->cal.sample_hz;

    uint32_t total = (uint32_t)ac->cal.window * (uint32_t)windows;
    for (uint32_t k = 0; k < total; k++) {
        uint16_t raw[AC_CH_COUNT];
        for (int ph = 0; ph < 3; ph++) {
            float th = w * (float)k - (float)ph * 2.0944f; /* 120 degrees */
            float v = 2048.0f + v_cnt * sinf(th);
            float i = 2048.0f + i_cnt * sinf(th - phi);
            raw[AC_GEN_L1 + ph] = (uint16_t)(v + 0.5f);
            raw[AC_MAINS_L1 + ph] = (uint16_t)(v + 0.5f);
            raw[AC_I_L1 + ph] = (uint16_t)(i + 0.5f);
        }
        ac_sense_push(ac, raw);
    }
}

static void test_ac_sense_rms_and_frequency(void)
{
    ac_cal_t cal;
    ac_cal_defaults(&cal, 50.0f);   /* 50:5 CTs */
    ac_sense_t ac;
    ac_sense_init(&ac, &cal);

    ac_feed(&ac, 230.0f, 34.8f, 50.0f, 0.0f, 2);
    CHECK(ac.valid);
    for (int i = 0; i < 3; i++) {
        CHECK(fabsf(ac.out.gen_v[i] - 230.0f) < 2.0f);
        CHECK(fabsf(ac.out.mains_v[i] - 230.0f) < 2.0f);
        CHECK(fabsf(ac.out.load_a[i] - 34.8f) < 0.5f);
    }
    CHECK(fabsf(ac.out.gen_hz - 50.0f) < 0.1f);
    CHECK(fabsf(ac.out.mains_hz - 50.0f) < 0.1f);

    /* 60 Hz: the window is a whole number of cycles there too. */
    ac_sense_init(&ac, &cal);
    ac_feed(&ac, 230.0f, 10.0f, 60.0f, 0.0f, 2);
    CHECK(fabsf(ac.out.gen_hz - 60.0f) < 0.1f);
    CHECK(fabsf(ac.out.gen_v[0] - 230.0f) < 2.0f);

    /* Off-nominal frequency must actually move the reading — a zero-crossing
     * counter without interpolation quantises to 10 Hz steps here. */
    ac_sense_init(&ac, &cal);
    ac_feed(&ac, 230.0f, 10.0f, 47.5f, 0.0f, 2);
    CHECK(fabsf(ac.out.gen_hz - 47.5f) < 0.2f);
}

static void test_ac_sense_power_and_pf(void)
{
    ac_cal_t cal;
    ac_cal_defaults(&cal, 50.0f);
    ac_sense_t ac;
    ac_sense_init(&ac, &cal);

    /* Unity PF: P = 3 * 230 * 20 = 13.8 kW. */
    ac_feed(&ac, 230.0f, 20.0f, 50.0f, 0.0f, 2);
    CHECK(fabsf(ac.out.real_power_w - 13800.0f) < 200.0f);
    CHECK(fabsf(ac.out.power_factor - 1.0f) < 0.02f);

    /* 0.8 lagging, the load a genset actually sees. */
    ac_sense_init(&ac, &cal);
    ac_feed(&ac, 230.0f, 20.0f, 50.0f, 36.87f, 2);
    CHECK(fabsf(ac.out.power_factor - 0.8f) < 0.02f);
    CHECK(fabsf(ac.out.real_power_w - 11040.0f) < 250.0f);

    /* Reverse power: current 180 degrees out. The sign must survive — this
     * is how a set motoring off the mains is detected. */
    ac_sense_init(&ac, &cal);
    ac_feed(&ac, 230.0f, 20.0f, 50.0f, 180.0f, 2);
    CHECK(ac.out.real_power_w < -13000.0f);
    CHECK(fabsf(ac.out.power_factor - 1.0f) < 0.02f);
}

static void test_ac_sense_dead_source(void)
{
    ac_cal_t cal;
    ac_cal_defaults(&cal, 50.0f);
    ac_sense_t ac;
    ac_sense_init(&ac, &cal);

    /* Bias only, plus one count of dither: a dead bus must read 0 V and
     * 0 Hz, never a frequency manufactured out of ADC noise. */
    for (uint32_t k = 0; k < cal.window * 2u; k++) {
        uint16_t raw[AC_CH_COUNT];
        for (int c = 0; c < AC_CH_COUNT; c++) {
            raw[c] = (uint16_t)(2048 + (int)(k % 3) - 1);
        }
        ac_sense_push(&ac, raw);
    }
    CHECK(ac.valid);
    CHECK(ac.out.gen_v[0] == 0.0f);
    CHECK(ac.out.gen_hz == 0.0f);
    CHECK(ac.out.mains_hz == 0.0f);
    CHECK(ac.out.real_power_w == 0.0f);
    CHECK(ac.out.power_factor == 0.0f);
}

static void test_ac_sense_tolerates_bias_drift(void)
{
    ac_cal_t cal;
    ac_cal_defaults(&cal, 50.0f);
    ac_sense_t ac;
    ac_sense_init(&ac, &cal);

    /* VREF_MID sitting 40 counts (32 mV) off nominal must not appear as
     * voltage: the DC term is measured, not assumed. */
    const float lsb = 3.3f / 4095.0f;
    float v_cnt = 230.0f * 1.41421356f / (lsb * 235.9f);
    float w = 2.0f * 3.14159265f * 50.0f / (float)cal.sample_hz;
    for (uint32_t k = 0; k < cal.window * 3u; k++) {
        uint16_t raw[AC_CH_COUNT];
        for (int ph = 0; ph < 3; ph++) {
            float th = w * (float)k - (float)ph * 2.0944f;
            uint16_t v = (uint16_t)(2088.0f + v_cnt * sinf(th) + 0.5f);
            raw[AC_GEN_L1 + ph] = v;
            raw[AC_MAINS_L1 + ph] = v;
            raw[AC_I_L1 + ph] = 2088;
        }
        ac_sense_push(&ac, raw);
    }
    CHECK(fabsf(ac.out.gen_v[0] - 230.0f) < 2.0f);
    CHECK(fabsf(ac.out.gen_hz - 50.0f) < 0.1f);
    CHECK(ac.out.load_a[0] == 0.0f);       /* no current, despite the offset */
    CHECK(ac.out.real_power_w == 0.0f);
}

static void test_ac_sense_fills_inputs(void)
{
    ac_cal_t cal;
    ac_cal_defaults(&cal, 50.0f);
    ac_sense_t ac;
    ac_sense_init(&ac, &cal);

    gcu_inputs_t in;
    memset(&in, 0, sizeof(in));
    in.gen_v[0] = 999.0f;
    ac_sense_fill(&ac, &in);
    CHECK(in.gen_v[0] == 999.0f);   /* nothing measured yet: left alone */

    ac_feed(&ac, 230.0f, 20.0f, 50.0f, 36.87f, 2);
    ac_sense_fill(&ac, &in);
    CHECK(fabsf(in.gen_v[0] - 230.0f) < 2.0f);
    CHECK(fabsf(in.gen_hz - 50.0f) < 0.1f);
    CHECK(fabsf(in.power_factor - 0.8f) < 0.02f);
    CHECK(in.real_power_w > 10000.0f);

    /* And the Modbus layer must now publish something other than zero. */
    gcu_app_t app;
    gcu_app_init(&app);
    uint16_t iregs[MODBUS_IREG_COUNT];
    modbus_publish(&in, &app, 0, iregs);
    CHECK(iregs[19] > 100 && iregs[19] < 120);   /* ~11.0 kW in 0.1 kW steps */
    CHECK(iregs[20] > 780 && iregs[20] < 820);   /* ~0.80 in 0.001 steps     */
}

/* The crank hold-up is sized for ~1 W, which assumes the backlight is off
 * while the starter drags the battery down. That assumption has to be code,
 * not a note in the calc sheet. */
static void test_backlight_sheds_during_crank(void)
{
    gcu_app_t app;
    gcu_app_init(&app);
    gcu_inputs_t in;
    gcu_outputs_t out;
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.battery_v = 12.6f;
    in.mains_v[0] = in.mains_v[1] = in.mains_v[2] = 240.0f;
    in.mains_hz = 50.0f;

    gcu_app_tick(&app, &in, &out);
    CHECK(out.backlight_pct == 80);      /* normal */

    /* Start manually and run until the starter engages. */
    app.cfg.preheat_ms = 0;
    in.mode_auto = false;
    in.key_start = true;
    for (int i = 0; i < 50 && !out.starter; i++) {
        gcu_app_tick(&app, &in, &out);
        in.key_start = false;
    }
    CHECK(out.starter);
    CHECK(out.backlight_pct == 0);       /* shed before the sag, not after */

    /* Sag arrives; still shed. */
    in.battery_v = 8.5f;
    gcu_app_tick(&app, &in, &out);
    CHECK(out.backlight_pct == 0);
}

static void test_backlight_shed_has_hysteresis(void)
{
    gcu_app_t app;
    gcu_app_init(&app);
    gcu_inputs_t in;
    gcu_outputs_t out;
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.mains_v[0] = in.mains_v[1] = in.mains_v[2] = 240.0f;
    in.mains_hz = 50.0f;

    in.battery_v = 12.6f;
    gcu_app_tick(&app, &in, &out);
    CHECK(out.backlight_pct == 80);

    in.battery_v = 9.8f;                 /* flat battery, no starter */
    gcu_app_tick(&app, &in, &out);
    CHECK(out.backlight_pct == 0);

    in.battery_v = 10.6f;                /* inside the hysteresis band */
    gcu_app_tick(&app, &in, &out);
    CHECK(out.backlight_pct == 0);       /* must not flicker back on */

    in.battery_v = 11.4f;
    gcu_app_tick(&app, &in, &out);
    CHECK(out.backlight_pct == 80);
}

/* ----------------------------------------------------------------- sensors */

static void test_sensor_curves(void)
{
    bool ok;

    /* Oil: table endpoints and an interpolated midpoint. */
    CHECK(fabsf(sensor_lookup(&SENSOR_OIL_VDO_10BAR, 10.0f, &ok) - 0.0f) < 0.01f);
    CHECK(ok);
    CHECK(fabsf(sensor_lookup(&SENSOR_OIL_VDO_10BAR, 184.0f, &ok) - 10.0f) < 0.01f);
    CHECK(ok);
    float mid = sensor_lookup(&SENSOR_OIL_VDO_10BAR, 92.5f, &ok);
    CHECK(ok && mid > 4.0f && mid < 5.5f);

    /* A shorted sender (below the table) and an open one (above it) are
     * INVALID, not 0 bar — 0 bar would look like a real low-oil shutdown. */
    (void)sensor_lookup(&SENSOR_OIL_VDO_10BAR, 2.0f, &ok);
    CHECK(!ok);
    (void)sensor_lookup(&SENSOR_OIL_VDO_10BAR, 5000.0f, &ok);
    CHECK(!ok);

    /* But a sender resting exactly on an end point, or a hair outside it,
     * is a working sender at the end of its range — not a fault. */
    CHECK(fabsf(sensor_lookup(&SENSOR_OIL_VDO_10BAR, 9.99f, &ok)) < 0.01f);
    CHECK(ok);
    (void)sensor_lookup(&SENSOR_OIL_VDO_10BAR, 200.0f, &ok);
    CHECK(ok);

    /* NTC: resistance falls as temperature rises. */
    float cold = sensor_lookup(&SENSOR_TEMP_VDO_NTC, 323.0f, &ok);
    float hot = sensor_lookup(&SENSOR_TEMP_VDO_NTC, 22.0f, &ok);
    CHECK(cold < hot);
    CHECK(fabsf(cold - 40.0f) < 0.01f && fabsf(hot - 120.0f) < 0.01f);

    /* Fuel: 0 ohm is full on this sender. */
    CHECK(fabsf(sensor_lookup(&SENSOR_FUEL_0_190, 0.0f, &ok) - 100.0f) < 0.01f);
    CHECK(fabsf(sensor_lookup(&SENSOR_FUEL_0_190, 190.0f, &ok) - 0.0f) < 0.01f);
}

static void test_sensor_scaling(void)
{
    const float lsb = 3.3f / 4095.0f;

    /* Oil channel: 8.06 mA through 184 ohm is 1.483 V = 1840 counts. */
    uint16_t counts = (uint16_t)(184.0f * 0.00806f / lsb + 0.5f);
    float r = sensor_ohms(counts, lsb, 0.00806f);
    CHECK(fabsf(r - 184.0f) < 1.0f);

    /* Battery divider: 12.6 V through 100k/22k lands at 2.272 V. */
    uint16_t bc = (uint16_t)((12.6f / 5.545f) / lsb + 0.5f);
    float v = sensor_divider_v(bc, lsb, 5.545f);
    CHECK(fabsf(v - 12.6f) < 0.02f);

    /* A 118-tooth flywheel at 1500 rpm gives a 339 us tooth period. */
    CHECK(fabsf(sensor_rpm(339, 118) - 1500.0f) < 5.0f);
    CHECK(sensor_rpm(0, 118) == 0.0f);       /* stopped, not a divide by 0 */
    CHECK(sensor_rpm(339, 0) == 0.0f);
}

static void test_din_debounce(void)
{
    din_debounce_t d;
    din_init(&d, 0x00);

    /* Three consecutive agreeing samples are needed to change state. */
    CHECK(din_update(&d, 0x01, 3) == 0x00);
    CHECK(din_update(&d, 0x01, 3) == 0x00);
    CHECK(din_update(&d, 0x01, 3) == 0x01);

    /* A bouncing contact must never get through: alternating samples reset
     * the agreement counter, so the state holds. */
    din_init(&d, 0x00);
    for (int i = 0; i < 20; i++) {
        uint8_t raw = (uint8_t)((i % 2) ? 0x01 : 0x00);
        CHECK(din_update(&d, raw, 3) == 0x00);
    }

    /* Channels are independent: one bouncing input cannot hold up another. */
    din_init(&d, 0x00);
    din_update(&d, 0x03, 3);
    din_update(&d, 0x01, 3);   /* bit 1 dropped out, bit 0 kept */
    uint8_t out = din_update(&d, 0x01, 3);
    CHECK(out == 0x01);
}

/* --------------------------------------------------------------- ecu_main */

/* A fake board. The point of the function-pointer platform is that the whole
 * runtime — conversion, debounce, scheduling, protocol servicing — runs here
 * with no hardware at all. */
static struct {
    uint32_t ms;
    uint16_t dc[PLAT_DC_COUNT];
    uint8_t din;
    uint32_t period_us;
    bool key_start, key_stop, mode_auto;
    gcu_outputs_t last_out;
    uint8_t backlight;
    int relay_calls;
    uint32_t hours_saved;
    /* AC waveform generator */
    uint32_t k;
    float mains_v_rms, gen_v_rms, i_rms;
} fake;

static uint32_t fk_millis(void) { return fake.ms; }
static uint16_t fk_dc(int i) { return fake.dc[i]; }
static uint8_t fk_din(void) { return fake.din; }
static uint32_t fk_period(void) { return fake.period_us; }
static void fk_relays(const gcu_outputs_t *o) { fake.last_out = *o; fake.relay_calls++; }
static void fk_backlight(uint8_t p) { fake.backlight = p; }
static bool fk_can_rx(j1939_frame_t *f) { (void)f; return false; }
static void fk_can_tx(const j1939_frame_t *f) { (void)f; }
static size_t fk_rs485_rx(uint8_t *b, size_t m) { (void)b; (void)m; return 0; }
static void fk_rs485_tx(const uint8_t *b, size_t n) { (void)b; (void)n; }
static void fk_save_hours(uint32_t h) { fake.hours_saved = h; }
static bool fk_keys(bool *s, bool *t, bool *a)
{
    *s = fake.key_start; *t = fake.key_stop; *a = fake.mode_auto;
    return true;
}

static bool fk_ac(uint16_t *out)
{
    /* Hand back at most a tick's worth per call so ecu_poll's drain loop is
     * actually exercised rather than short-circuited. */
    static int budget;
    if (budget <= 0) { budget = 32; return false; }
    budget--;
    const float lsb = 3.3f / 4095.0f;
    float gc = fake.gen_v_rms * 1.41421356f / (lsb * 235.9f);
    float mc = fake.mains_v_rms * 1.41421356f / (lsb * 235.9f);
    float ic = fake.i_rms * 1.41421356f / (lsb / 0.1f * 10.0f);
    float w = 2.0f * 3.14159265f * 50.0f / 3200.0f;
    for (int ph = 0; ph < 3; ph++) {
        float th = w * (float)fake.k - (float)ph * 2.0944f;
        out[AC_GEN_L1 + ph] = (uint16_t)(2048.0f + gc * sinf(th) + 0.5f);
        out[AC_MAINS_L1 + ph] = (uint16_t)(2048.0f + mc * sinf(th) + 0.5f);
        out[AC_I_L1 + ph] = (uint16_t)(2048.0f + ic * sinf(th) + 0.5f);
    }
    fake.k++;
    return true;
}

static const ecu_platform_t FAKE_PLAT = {
    .millis = fk_millis, .ac_sample = fk_ac, .dc_channel = fk_dc,
    .din_raw = fk_din, .rpm_period_us = fk_period, .keys = fk_keys,
    .relays = fk_relays, .backlight = fk_backlight,
    .can_rx = fk_can_rx, .can_tx = fk_can_tx,
    .rs485_rx = fk_rs485_rx, .rs485_tx = fk_rs485_tx,
    .nvm_load_hours = NULL, .nvm_save_hours = fk_save_hours,
};

static void fake_reset(void)
{
    const float lsb = 3.3f / 4095.0f;
    memset(&fake, 0, sizeof(fake));
    /* A stopped engine has NO oil pressure. Faking 6.5 bar here made
     * crank-disconnect fire on the first tick of cranking. */
    fake.dc[PLAT_DC_OIL] = (uint16_t)(10.0f * 0.00806f / lsb);    /* 0 bar   */
    fake.dc[PLAT_DC_FUEL] = (uint16_t)(95.0f * 0.00806f / lsb);   /* 50 %    */
    fake.dc[PLAT_DC_TEMP] = (uint16_t)(197.0f * 0.00200f / lsb);  /* 60 C    */
    fake.dc[PLAT_DC_VBAT] = (uint16_t)((12.6f / 5.545f) / lsb);
    fake.dc[PLAT_DC_DPLUS] = (uint16_t)((0.5f / 5.545f) / lsb);
    fake.din = 0x01;              /* e-stop healthy (closed), all else open */
    /* Mains live, generator dead — the engine is stopped. Feeding generator
     * volts here made the FSM's gen-frequency crank-disconnect fire on the
     * first tick of cranking. */
    fake.mains_v_rms = 240.0f;
    fake.gen_v_rms = 0.0f;
    fake.i_rms = 0.0f;
}

static void ecu_run_ms(ecu_t *e, uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        fake.ms++;
        ecu_poll(e);
    }
}

static void test_ecu_runtime_converts_and_ticks(void)
{
    fake_reset();
    ecu_rt_cfg_t cfg;
    ecu_rt_defaults(&cfg);
    ecu_t e;
    ecu_init(&e, &FAKE_PLAT, &cfg);

    ecu_run_ms(&e, 500);

    /* Senders converted through the curves, not passed through raw. */
    CHECK(e.in.oil_pressure_valid && e.in.oil_pressure_bar < 0.2f);
    CHECK(e.in.fuel_level_valid && fabsf(e.in.fuel_level_pct - 50.0f) < 3.0f);
    CHECK(e.in.coolant_temp_valid && fabsf(e.in.coolant_temp_c - 60.0f) < 3.0f);
    CHECK(fabsf(e.in.battery_v - 12.6f) < 0.1f);

    /* AC pipeline reached the control inputs. */
    CHECK(fabsf(e.in.mains_v[0] - 240.0f) < 3.0f);
    CHECK(fabsf(e.in.mains_hz - 50.0f) < 0.2f);

    /* Ticks ran at 10 ms and the outputs were driven every one of them. */
    CHECK(e.ticks >= 48 && e.ticks <= 51);
    CHECK(fake.relay_calls == (int)e.ticks);
    CHECK(fake.backlight == 80);

    /* E-stop is read INVERTED. Healthy field wiring reads not-pressed... */
    CHECK(!e.in.emergency_stop);
    /* ...and a cut wire reads as pressed, which is the whole point. */
    fake.din = 0x00;
    ecu_run_ms(&e, 100);
    CHECK(e.in.emergency_stop);
}

static void test_ecu_runtime_starts_the_engine(void)
{
    fake_reset();
    ecu_rt_cfg_t cfg;
    ecu_rt_defaults(&cfg);
    ecu_t e;
    ecu_init(&e, &FAKE_PLAT, &cfg);
    e.app.cfg.preheat_ms = 0;

    ecu_run_ms(&e, 200);
    CHECK(!fake.last_out.starter);

    /* Panel start in MANUAL: the runtime must turn a key press into a crank
     * without anything else changing. */
    fake.mode_auto = false;
    fake.key_start = true;
    ecu_run_ms(&e, 30);
    fake.key_start = false;
    ecu_run_ms(&e, 100);
    CHECK(fake.last_out.run_enable);
    CHECK(fake.last_out.starter);
    CHECK(fake.backlight == 0);   /* shed while cranking */

    /* The engine fires: speed appears and oil pressure comes up with it.
     * 118 teeth at 1500 rpm is a 339 us tooth period. */
    fake.period_us = 339;
    fake.dc[PLAT_DC_OIL] =
        (uint16_t)(123.0f * 0.00806f / (3.3f / 4095.0f));   /* 6.5 bar */
    fake.gen_v_rms = 240.0f;
    ecu_run_ms(&e, 300);
    CHECK(fabsf(e.in.rpm - 1500.0f) < 20.0f);
    CHECK(!fake.last_out.starter);
    CHECK(fake.last_out.run_enable);
    CHECK(fake.backlight == 80);  /* restored once cranking ends */

    /* And the Modbus image is being republished from the same data. */
    CHECK(e.iregs[10] > 1400 && e.iregs[10] < 1600);
}

static void test_ecu_runtime_survives_millis_wrap(void)
{
    fake_reset();
    ecu_rt_cfg_t cfg;
    ecu_rt_defaults(&cfg);
    ecu_t e;
    ecu_init(&e, &FAKE_PLAT, &cfg);

    /* Park the clock just below the 32-bit rollover and run through it. A
     * signed comparison here stalls the controller for 49 days. */
    fake.ms = 0xFFFFFF00u;
    e.last_tick_ms = fake.ms;
    uint32_t before = e.ticks;
    ecu_run_ms(&e, 600);
    CHECK(e.ticks - before >= 55);
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
    test_j1939_red_lamp_blocks_start();
    test_j1939_reset_after_ecu_sleeps();
    test_modbus_crc();
    test_modbus_read_input_regs();
    test_modbus_addressing();
    test_modbus_write_and_commands();
    test_modbus_write_multiple_is_atomic();
    test_modbus_publish_snapshot();
    test_modbus_short_buffer_is_slave_failure();
    test_batt_thresholds_are_12v();
    test_batt_high_trips_on_runaway_regulator();
    test_ac_sense_rms_and_frequency();
    test_ac_sense_power_and_pf();
    test_ac_sense_dead_source();
    test_ac_sense_tolerates_bias_drift();
    test_ac_sense_fills_inputs();
    test_backlight_sheds_during_crank();
    test_backlight_shed_has_hysteresis();
    test_sensor_curves();
    test_sensor_scaling();
    test_din_debounce();
    test_ecu_runtime_converts_and_ticks();
    test_ecu_runtime_starts_the_engine();
    test_ecu_runtime_survives_millis_wrap();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
