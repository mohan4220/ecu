/*
 * gcu_app.h — top-level supervisor: wires engine_fsm + protection + amf_fsm.
 *
 * This is the module the RTOS task (or the SIL harness) calls once per tick.
 * It owns mode logic (AUTO/MANUAL), routes alarms into engine commands, and
 * produces the final relay outputs.
 */
#ifndef GCU_APP_H
#define GCU_APP_H

#include "amf_fsm.h"
#include "engine_fsm.h"
#include "protection.h"

typedef struct {
    gcu_config_t cfg;
    engine_fsm_t engine;
    protection_state_t prot;
    amf_fsm_t amf;
    bool manual_run; /* MANUAL mode: operator wants engine running */
    /* Break-before-make enforcement across ALL handover paths: ticks each
     * contactor has been commanded open (saturating). */
    uint32_t gen_open_ticks;
    uint32_t mains_open_ticks;
} gcu_app_t;

void gcu_app_init(gcu_app_t *app);
void gcu_app_tick(gcu_app_t *app, const gcu_inputs_t *in, gcu_outputs_t *out);

#endif /* GCU_APP_H */
