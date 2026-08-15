/*
 * protection.h — protection engine.
 *
 * Evaluates every monitored quantity each tick. Two classes:
 *   WARNING  — alarm only, engine keeps running.
 *   SHUTDOWN — engine_fsm must cut fuel immediately; alarm latches until
 *              operator reset (key_stop while stopped).
 * Each rule has a threshold, a qualification delay, and an arming condition
 * (e.g. low oil pressure only means something once the engine runs).
 */
#ifndef PROTECTION_H
#define PROTECTION_H

#include "gcu_types.h"

typedef enum {
    ALARM_NONE = 0,
    ALARM_EMERGENCY_STOP,
    ALARM_LOW_OIL_PRESSURE,
    ALARM_HIGH_COOLANT_TEMP,
    ALARM_OVERSPEED,
    ALARM_UNDERSPEED,
    ALARM_GEN_UNDER_VOLT,
    ALARM_GEN_OVER_VOLT,
    ALARM_GEN_UNDER_FREQ,
    ALARM_GEN_OVER_FREQ,
    ALARM_OVERCURRENT,
    ALARM_CHARGE_FAIL,
    ALARM_BATT_LOW,
    ALARM_BATT_HIGH,
    ALARM_LOW_COOLANT_LEVEL,
    ALARM_FAIL_TO_START,
    ALARM_FAIL_TO_STOP,
    ALARM_SENSOR_LOSS, /* rpm/oil sources invalid while running */
    ALARM_COUNT
} alarm_id_t;

typedef enum {
    ALARM_CLASS_WARNING,
    ALARM_CLASS_SHUTDOWN,
} alarm_class_t;

typedef struct {
    /* Per-alarm qualification timers (ticks the condition has been true). */
    uint32_t qual_ticks[ALARM_COUNT];
    /* Latched active alarms. Shutdown alarms stay set until reset. */
    bool active[ALARM_COUNT];
    /* Ticks since the engine entered RUNNING_WARMUP (for arming). 0 = not running. */
    uint32_t running_ticks;
} protection_state_t;

void protection_init(protection_state_t *st);

/*
 * One tick. engine_running: fsm is in WARMUP/RUNNING/COOLDOWN (oil, speed,
 * charge protections armed). gen_excited: fsm expects healthy AC output
 * (RUNNING after arm delay — volt/freq protections armed).
 * fail_to_start / fail_to_stop are events reported by engine_fsm.
 */
void protection_tick(protection_state_t *st, const gcu_config_t *cfg,
                     const gcu_inputs_t *in, bool engine_running,
                     bool gen_excited, bool fail_to_start, bool fail_to_stop);

/* Operator alarm reset: clears latched alarms whose cause is gone. */
void protection_reset(protection_state_t *st);

alarm_class_t protection_class(alarm_id_t id);
bool protection_shutdown_active(const protection_state_t *st);
bool protection_warning_active(const protection_state_t *st);
const char *protection_name(alarm_id_t id);

#endif /* PROTECTION_H */
