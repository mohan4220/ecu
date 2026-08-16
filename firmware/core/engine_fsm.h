/*
 * engine_fsm.h — engine start/stop state machine.
 *
 * STOPPED → PREHEAT → CRANK → (CRANK_REST, ≤N attempts) → RUNNING_WARMUP
 *        → RUNNING → COOLDOWN → STOPPING → STOPPED
 * plus SHUTDOWN (latched fault stop: fuel cut immediately, no cooldown).
 *
 * Crank disconnect: RPM above threshold OR oil pressure risen.
 * Fail-to-start: all attempts exhausted. Fail-to-stop: rpm never reaches
 * zero after fuel cut within timeout.
 */
#ifndef ENGINE_FSM_H
#define ENGINE_FSM_H

#include "gcu_types.h"

typedef enum {
    ENG_STOPPED = 0,
    ENG_PREHEAT,
    ENG_CRANK,
    ENG_CRANK_REST,
    ENG_WARMUP,
    ENG_RUNNING,
    ENG_COOLDOWN,
    ENG_STOPPING,
    ENG_SHUTDOWN, /* stopped by a shutdown alarm; latched until reset */
} engine_state_t;

typedef struct {
    engine_state_t state;
    uint32_t state_ticks;   /* ticks in current state                  */
    uint8_t attempt;        /* crank attempt counter                   */
    bool fail_to_start;     /* event flags, true for one tick          */
    bool fail_to_stop;
    bool standstill_seen;   /* rpm < 10 observed during this stop:
                               valid stopped-evidence even after a J1939
                               engine ECU sleeps and rpm goes stale     */
} engine_fsm_t;

/* Commands from amf_fsm / operator, evaluated each tick. */
typedef struct {
    bool start_requested; /* keep engine running                     */
    bool stop_requested;  /* normal stop (with cooldown)             */
    bool immediate_stop;  /* shutdown alarm or e-stop: cut fuel NOW  */
    bool skip_cooldown;   /* manual stop: no cooldown wanted         */
    bool alarm_reset;     /* operator reset (leaves SHUTDOWN state)  */
} engine_cmd_t;

void engine_fsm_init(engine_fsm_t *f);
void engine_fsm_tick(engine_fsm_t *f, const gcu_config_t *cfg,
                     const gcu_inputs_t *in, const engine_cmd_t *cmd,
                     gcu_outputs_t *out);

/* True in states where the engine is expected to be running. */
bool engine_fsm_running(const engine_fsm_t *f);
const char *engine_state_name(engine_state_t s);

#endif /* ENGINE_FSM_H */
