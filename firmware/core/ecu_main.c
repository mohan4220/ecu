/*
 * ecu_main.c — the runtime. See ecu_main.h.
 */
#include "ecu_main.h"

#include <string.h>

/*
 * Digital input assignment. This is the one place it exists, so the panel
 * legend, docs/io-map.md and the firmware cannot drift apart.
 *
 * DIN1 (E-stop) is read INVERTED and that is deliberate: the mushroom head
 * is wired as a normally-CLOSED contact feeding 12 V in, so a pressed button
 * — or a cut wire, or a pulled connector — all read as 0 and all mean stop.
 * A normally-open e-stop that fails safe does not exist.
 */
#define DIN_ESTOP     0x01u
#define DIN_REMOTE    0x02u
#define DIN_LOW_OIL   0x04u
#define DIN_HIGH_TEMP 0x08u
#define DIN_LOW_LEVEL 0x10u

void ecu_rt_defaults(ecu_rt_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->adc_lsb_v = 3.3f / 4095.0f;
    cfg->oil_excite_a = 0.00806f;   /* 0.5 V / 62 ohm  */
    cfg->fuel_excite_a = 0.00806f;
    cfg->temp_excite_a = 0.00200f;  /* 0.5 V / 249 ohm */
    cfg->divider_ratio = 5.545f;    /* 100k / 22k      */
    cfg->flywheel_teeth = 118;
    cfg->modbus_address = 1;
    cfg->din_debounce = 3;          /* 30 ms at the 10 ms tick */
    cfg->oil = SENSOR_OIL_VDO_10BAR;
    cfg->fuel = SENSOR_FUEL_0_190;
    cfg->temp = SENSOR_TEMP_VDO_NTC;
    ac_cal_defaults(&cfg->ac, 50.0f);
}

void ecu_init(ecu_t *e, const ecu_platform_t *plat, const ecu_rt_cfg_t *cfg)
{
    memset(e, 0, sizeof(*e));
    e->plat = plat;
    e->cfg = *cfg;

    gcu_app_init(&e->app);
    ac_sense_init(&e->ac, &e->cfg.ac);
    j1939_init(&e->j1939, J1939_ADDR_GENSET_CONTROLLER, J1939_ADDR_ENGINE_1);
    modbus_init(&e->modbus, e->cfg.modbus_address);
    /* The debounce is seeded from the first real read on the first tick,
     * not from an assumption here — see read_digital(). */
    din_init(&e->din, 0x00);

    if (plat->nvm_load_hours) {
        e->run_hours = plat->nvm_load_hours();
    }
    if (plat->millis) {
        e->last_tick_ms = plat->millis();
    }
}

/* ------------------------------------------------------------- conversion */

static void read_senders(ecu_t *e)
{
    const ecu_rt_cfg_t *c = &e->cfg;
    const ecu_platform_t *p = e->plat;
    bool ok;

    float r = sensor_ohms(p->dc_channel(PLAT_DC_OIL), c->adc_lsb_v,
                          c->oil_excite_a);
    e->in.oil_pressure_bar = sensor_lookup(&c->oil, r, &ok);
    e->in.oil_pressure_valid = ok;

    r = sensor_ohms(p->dc_channel(PLAT_DC_FUEL), c->adc_lsb_v,
                    c->fuel_excite_a);
    e->in.fuel_level_pct = sensor_lookup(&c->fuel, r, &ok);
    e->in.fuel_level_valid = ok;

    r = sensor_ohms(p->dc_channel(PLAT_DC_TEMP), c->adc_lsb_v,
                    c->temp_excite_a);
    e->in.coolant_temp_c = sensor_lookup(&c->temp, r, &ok);
    e->in.coolant_temp_valid = ok;

    e->in.battery_v = sensor_divider_v(p->dc_channel(PLAT_DC_VBAT),
                                       c->adc_lsb_v, c->divider_ratio);
    e->in.dplus_v = sensor_divider_v(p->dc_channel(PLAT_DC_DPLUS),
                                     c->adc_lsb_v, c->divider_ratio);
}

static void read_digital(ecu_t *e)
{
    uint8_t raw = e->plat->din_raw();

    /* Adopt the field wiring's actual state on the first tick rather than
     * debouncing away from a guess. Seeding "all inactive" looks harmless
     * until you remember DIN1 is inverted: the e-stop then reads as PRESSED
     * for the first three ticks and latches a shutdown alarm on every single
     * power-up, which no amount of healthy wiring clears without an operator
     * pressing STOP. A genuinely pressed e-stop still reads pressed here. */
    if (!e->din_seeded) {
        din_init(&e->din, raw);
        e->din_seeded = true;
    }

    uint8_t d = din_update(&e->din, raw, e->cfg.din_debounce);

    e->in.emergency_stop = (d & DIN_ESTOP) == 0;  /* inverted: fail-safe */
    e->in.remote_start = (d & DIN_REMOTE) != 0;
    e->in.low_oil_switch = (d & DIN_LOW_OIL) != 0;
    e->in.high_coolant_switch = (d & DIN_HIGH_TEMP) != 0;
    e->in.low_coolant_level = (d & DIN_LOW_LEVEL) != 0;
}

static void read_speed(ecu_t *e)
{
    e->in.rpm = sensor_rpm(e->plat->rpm_period_us(), e->cfg.flywheel_teeth);
    e->in.rpm_valid = true;   /* J1939 overrides this below when present */
}

/* Modbus holding registers are commands, so they are folded in with the
 * panel keys rather than replacing them: a remote start and a panel start
 * both mean start. */
static void apply_remote(ecu_t *e)
{
    if (e->modbus.mode_from_remote) {
        switch (e->modbus.hold[0]) {
        case 2:
            e->in.mode_auto = true;
            break;
        case 1:
            e->in.mode_auto = false;
            break;
        default:
            break;   /* 0 (off) and 3 (test) leave the panel switch alone */
        }
    }
    if (e->modbus.hold[1]) {
        e->in.remote_start = true;
    }
    if (e->modbus.cmd_alarm_reset) {
        e->in.key_stop = true;    /* stop also clears latched alarms */
        e->modbus.cmd_alarm_reset = false;
    }
}

/* --------------------------------------------------------------- servicing */

static void service_can(ecu_t *e)
{
    j1939_frame_t f;
    int guard = 32;   /* never let a chattering bus starve the control tick */
    while (guard-- > 0 && e->plat->can_rx && e->plat->can_rx(&f)) {
        j1939_rx(&e->j1939, &f);
    }
}

static void service_modbus(ecu_t *e)
{
    if (!e->plat->rs485_rx) {
        return;
    }
    uint8_t req[MODBUS_MAX_FRAME], resp[MODBUS_MAX_FRAME];
    size_t n = e->plat->rs485_rx(req, sizeof(req));
    if (n == 0) {
        return;
    }
    size_t r = modbus_rx(&e->modbus, req, n, e->iregs, resp, sizeof(resp));
    if (r > 0 && e->plat->rs485_tx) {
        e->plat->rs485_tx(resp, r);
    }
}

static void accumulate_hours(ecu_t *e)
{
    if (!e->out.run_enable) {
        return;
    }
    e->run_ticks++;
    if (e->run_ticks >= (3600u * 1000u / GCU_TICK_MS)) {
        e->run_ticks = 0;
        e->run_hours++;
        if (e->plat->nvm_save_hours) {
            e->plat->nvm_save_hours(e->run_hours);
        }
    }
}

static void control_tick(ecu_t *e)
{
    /* Measurements first, so the tick acts on this tick's data. */
    ac_sense_fill(&e->ac, &e->in);
    read_senders(e);
    read_digital(e);
    read_speed(e);

    if (e->plat->keys) {
        bool start = false, stop = false, mode_auto = false;
        if (e->plat->keys(&start, &stop, &mode_auto)) {
            e->in.key_start = start;
            e->in.key_stop = stop;
            e->in.mode_auto = mode_auto;
        }
    }
    apply_remote(e);

    /* J1939 last, but as a MERGE, not an overwrite.
     *
     * j1939_fill_inputs() assigns every engine field unconditionally, which
     * is correct when CAN is the only source. Here it is not: with the
     * analog senders now real, an unconditional fill wipes four good
     * readings the moment the bus goes quiet — or permanently, on a legacy
     * mechanically-governed engine that has no ECU at all. So the fresh
     * J1939 value wins and a stale one leaves the sender reading standing.
     * The lamps and comms-lost flag have no analog equivalent and pass
     * straight through. */
    {
        gcu_inputs_t can = e->in;
        j1939_fill_inputs(&e->j1939, &can);
        if (can.rpm_valid) {
            e->in.rpm = can.rpm;
            e->in.rpm_valid = true;
        }
        if (can.oil_pressure_valid) {
            e->in.oil_pressure_bar = can.oil_pressure_bar;
            e->in.oil_pressure_valid = true;
        }
        if (can.coolant_temp_valid) {
            e->in.coolant_temp_c = can.coolant_temp_c;
            e->in.coolant_temp_valid = true;
        }
        if (can.fuel_level_valid) {
            e->in.fuel_level_pct = can.fuel_level_pct;
            e->in.fuel_level_valid = true;
        }
        e->in.ecu_red_lamp = can.ecu_red_lamp;
        e->in.ecu_amber_lamp = can.ecu_amber_lamp;
        e->in.ecu_comms_lost = can.ecu_comms_lost;
    }

    gcu_app_tick(&e->app, &e->in, &e->out);

    if (e->plat->relays) {
        e->plat->relays(&e->out);
    }
    if (e->plat->backlight) {
        e->plat->backlight(e->out.backlight_pct);
    }

    j1939_frame_t tx;
    if (j1939_tick(&e->j1939, e->out.run_enable, &tx) && e->plat->can_tx) {
        e->plat->can_tx(&tx);
    }

    accumulate_hours(e);
    modbus_publish(&e->in, &e->app, e->run_hours, e->iregs);

    /* One-shot inputs must not stick for a second tick. */
    e->in.key_start = false;
    e->in.key_stop = false;
    e->ticks++;
}

bool ecu_poll(ecu_t *e)
{
    /* Drain the AC pipeline every pass: at 3200 Hz a 10 ms tick brings 32
     * sample sets, and the DMA half-buffer must be emptied faster than it
     * fills or a whole window is lost. */
    uint16_t raw[AC_CH_COUNT];
    int guard = 4 * AC_MAX_WINDOW;
    while (guard-- > 0 && e->plat->ac_sample && e->plat->ac_sample(raw)) {
        ac_sense_push(&e->ac, raw);
    }

    service_can(e);
    service_modbus(e);

    uint32_t now = e->plat->millis ? e->plat->millis() : 0;
    if (!e->started) {
        e->started = true;
        e->last_tick_ms = now;
    }
    /* Unsigned subtraction, so the millisecond counter can wrap without the
     * controller stalling for 49 days. */
    if ((uint32_t)(now - e->last_tick_ms) < GCU_TICK_MS) {
        return false;
    }
    e->last_tick_ms += GCU_TICK_MS;
    /* If the loop fell badly behind (a long flash write, a debugger halt),
     * give up on catching every missed tick and resynchronise. */
    if ((uint32_t)(now - e->last_tick_ms) > 10u * GCU_TICK_MS) {
        e->last_tick_ms = now;
    }
    control_tick(e);
    return true;
}
