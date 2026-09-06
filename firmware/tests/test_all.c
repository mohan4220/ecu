/*
 * test_all.c — unit + scenario tests for the pure logic modules.
 * Plain C, no framework: CHECK() prints and counts failures.
 */
#include <stdio.h>
#include <string.h>

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

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
