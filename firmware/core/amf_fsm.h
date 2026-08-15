/*
 * amf_fsm.h — auto-mains-failure supervisor.
 *
 * Watches mains health, decides when the engine must run, and owns the
 * transfer switches with break-before-make dead time. The contactor
 * interlock is enforced here in software AND must exist in panel wiring.
 *
 * ON_MAINS → (mains fails, qualified) → STARTING → ON_GEN
 *         → (mains returns, qualified) → RETRANSFER → cooldown → ON_MAINS
 */
#ifndef AMF_FSM_H
#define AMF_FSM_H

#include "gcu_types.h"

typedef enum {
    AMF_ON_MAINS = 0,   /* load on mains (or dead bus), engine off   */
    AMF_STARTING,       /* mains failed; engine start requested      */
    AMF_ON_GEN,         /* load on generator                         */
    AMF_RETRANSFER,     /* dead-time gap gen→mains                   */
    AMF_TRANSFER_TO_GEN,/* dead-time gap mains→gen                   */
    AMF_IDLE,           /* not in AUTO: amf makes no demands         */
} amf_state_t;

typedef struct {
    amf_state_t state;
    uint32_t state_ticks;
    uint32_t mains_bad_ticks;
    uint32_t mains_good_ticks;
    uint32_t gen_good_ticks;
    bool mains_ctr_latched_open; /* qualified failure seen; re-close only
                                    after 2 s of healthy mains (IDLE path) */
} amf_fsm_t;

typedef struct {
    bool engine_start;   /* amf wants the engine running             */
    bool gen_contactor;
    bool mains_contactor;
} amf_demand_t;

void amf_fsm_init(amf_fsm_t *a);

/*
 * One tick. engine_available: engine FSM is in RUNNING (not warmup —
 * loading a cold engine is poor practice) and no shutdown alarm.
 */
void amf_fsm_tick(amf_fsm_t *a, const gcu_config_t *cfg,
                  const gcu_inputs_t *in, bool auto_mode,
                  bool engine_available, amf_demand_t *demand);

const char *amf_state_name(amf_state_t s);
bool amf_mains_healthy(const gcu_config_t *cfg, const gcu_inputs_t *in);
bool amf_gen_healthy(const gcu_config_t *cfg, const gcu_inputs_t *in);

#endif /* AMF_FSM_H */
