#include "protection.h"

#include <string.h>

void protection_init(protection_state_t *st)
{
    memset(st, 0, sizeof(*st));
}

alarm_class_t protection_class(alarm_id_t id)
{
    switch (id) {
    case ALARM_CHARGE_FAIL:
    case ALARM_BATT_LOW:
    case ALARM_BATT_HIGH:
    case ALARM_LOW_COOLANT_LEVEL:
    case ALARM_ECU_WARNING:
    case ALARM_ECU_COMMS_LOST:
        return ALARM_CLASS_WARNING;
    default:
        return ALARM_CLASS_SHUTDOWN;
    }
}

const char *protection_name(alarm_id_t id)
{
    static const char *names[ALARM_COUNT] = {
        "none",          "EMERGENCY STOP", "LOW OIL PRESSURE",
        "HIGH COOLANT",  "OVERSPEED",      "UNDERSPEED",
        "GEN UNDER-V",   "GEN OVER-V",     "GEN UNDER-HZ",
        "GEN OVER-HZ",   "OVERCURRENT",    "CHARGE FAIL",
        "BATT LOW",      "BATT HIGH",      "LOW COOLANT LVL",
        "FAIL TO START", "FAIL TO STOP",   "SENSOR LOSS",
        "ECU STOP LAMP", "ECU WARNING",    "ECU COMMS LOST",
    };
    return (id < ALARM_COUNT) ? names[id] : "?";
}

/* Qualify a condition over its delay; latch the alarm when it holds. */
static void qualify(protection_state_t *st, alarm_id_t id, bool condition,
                    uint32_t delay_ms)
{
    if (condition) {
        if (st->qual_ticks[id] < UINT32_MAX) {
            st->qual_ticks[id]++;
        }
        if (st->qual_ticks[id] >= GCU_MS_TO_TICKS(delay_ms)) {
            st->active[id] = true;
        }
    } else {
        st->qual_ticks[id] = 0;
        /* Warnings self-clear when the cause goes; shutdowns stay latched. */
        if (protection_class(id) == ALARM_CLASS_WARNING) {
            st->active[id] = false;
        }
    }
}

void protection_tick(protection_state_t *st, const gcu_config_t *cfg,
                     const gcu_inputs_t *in, bool engine_running,
                     bool gen_excited, bool fail_to_start, bool fail_to_stop)
{
    if (engine_running) {
        if (st->running_ticks < UINT32_MAX) {
            st->running_ticks++;
        }
    } else {
        st->running_ticks = 0;
    }

    /* Immediate, no delay: e-stop and events from the engine FSM. */
    qualify(st, ALARM_EMERGENCY_STOP, in->emergency_stop, 0);
    if (fail_to_start) {
        st->active[ALARM_FAIL_TO_START] = true;
    }
    if (fail_to_stop) {
        st->active[ALARM_FAIL_TO_STOP] = true;
    }

    /* Engine protections — armed while the engine should be running. */
    bool oil_low = engine_running &&
                   ((in->oil_pressure_valid &&
                     in->oil_pressure_bar < cfg->low_oil_bar) ||
                    in->low_oil_switch);
    qualify(st, ALARM_LOW_OIL_PRESSURE, oil_low, cfg->low_oil_delay_ms);

    bool coolant_hot = engine_running &&
                       ((in->coolant_temp_valid &&
                         in->coolant_temp_c > cfg->high_coolant_c) ||
                        in->high_coolant_switch);
    qualify(st, ALARM_HIGH_COOLANT_TEMP, coolant_hot, 1000);

    qualify(st, ALARM_OVERSPEED,
            in->rpm_valid && in->rpm > cfg->overspeed_rpm, 200);

    /* Underspeed armed only once the set is expected at nominal speed
     * (gen_excited = RUNNING + arm delay) — a warmup idle must not trip. */
    bool underspeed = gen_excited && in->rpm_valid &&
                      in->rpm < 0.8f * cfg->nominal_rpm;
    qualify(st, ALARM_UNDERSPEED, underspeed, 3000);

    bool sensors_lost = engine_running &&
                        (!in->rpm_valid || !in->oil_pressure_valid);
    qualify(st, ALARM_SENSOR_LOSS, sensors_lost, 3000);

    /* Generator AC protections — armed only once output is expected. */
    float gvmin = in->gen_v[0], gvmax = in->gen_v[0];
    for (int i = 1; i < 3; i++) {
        if (in->gen_v[i] < gvmin) gvmin = in->gen_v[i];
        if (in->gen_v[i] > gvmax) gvmax = in->gen_v[i];
    }
    qualify(st, ALARM_GEN_UNDER_VOLT, gen_excited && gvmin < cfg->gen_under_v,
            cfg->gen_volt_delay_ms);
    qualify(st, ALARM_GEN_OVER_VOLT, gen_excited && gvmax > cfg->gen_over_v,
            cfg->gen_volt_delay_ms);
    qualify(st, ALARM_GEN_UNDER_FREQ, gen_excited && in->gen_hz < cfg->gen_under_hz,
            cfg->gen_freq_delay_ms);
    qualify(st, ALARM_GEN_OVER_FREQ, gen_excited && in->gen_hz > cfg->gen_over_hz,
            cfg->gen_freq_delay_ms);

    float imax = in->load_a[0];
    for (int i = 1; i < 3; i++) {
        if (in->load_a[i] > imax) imax = in->load_a[i];
    }
    qualify(st, ALARM_OVERCURRENT, gen_excited && imax > cfg->overcurrent_a,
            cfg->overcurrent_delay_ms);

    /* Warnings */
    bool charge_fail = engine_running &&
                       st->running_ticks > GCU_MS_TO_TICKS(10000) &&
                       in->dplus_v < cfg->charge_fail_ratio * in->battery_v;
    qualify(st, ALARM_CHARGE_FAIL, charge_fail, cfg->charge_fail_delay_ms);
    qualify(st, ALARM_BATT_LOW, in->battery_v < cfg->batt_low_v,
            cfg->batt_delay_ms);
    qualify(st, ALARM_BATT_HIGH, in->battery_v > cfg->batt_high_v,
            cfg->batt_delay_ms);
    qualify(st, ALARM_LOW_COOLANT_LEVEL, in->low_coolant_level, 5000);

    /* J1939 engine ECU severity. The red stop lamp is the engine ECU
     * telling us to shut down — treat it like our own shutdowns. Armed
     * only while running: a latched red lamp from a previous fault must
     * not block cranking after operator reset (the ECU re-evaluates). */
    qualify(st, ALARM_ECU_RED_LAMP, engine_running && in->ecu_red_lamp, 500);
    qualify(st, ALARM_ECU_WARNING, in->ecu_amber_lamp, 1000);
    qualify(st, ALARM_ECU_COMMS_LOST, in->ecu_comms_lost, 1000);
}

void protection_reset(protection_state_t *st)
{
    for (int i = 0; i < ALARM_COUNT; i++) {
        /* Clear latched alarms whose qualifying condition is gone now. */
        if (st->qual_ticks[i] == 0) {
            st->active[i] = false;
        }
    }
}

bool protection_shutdown_active(const protection_state_t *st)
{
    for (int i = 1; i < ALARM_COUNT; i++) {
        if (st->active[i] && protection_class((alarm_id_t)i) == ALARM_CLASS_SHUTDOWN) {
            return true;
        }
    }
    return false;
}

bool protection_warning_active(const protection_state_t *st)
{
    for (int i = 1; i < ALARM_COUNT; i++) {
        if (st->active[i] && protection_class((alarm_id_t)i) == ALARM_CLASS_WARNING) {
            return true;
        }
    }
    return false;
}
