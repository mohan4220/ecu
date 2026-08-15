#include "amf_fsm.h"

void amf_fsm_init(amf_fsm_t *a)
{
    a->state = AMF_ON_MAINS;
    a->state_ticks = 0;
    a->mains_bad_ticks = 0;
    a->mains_good_ticks = 0;
    a->gen_good_ticks = 0;
    a->mains_ctr_latched_open = false;
}

const char *amf_state_name(amf_state_t s)
{
    static const char *names[] = {
        "ON_MAINS", "STARTING", "ON_GEN", "RETRANSFER", "XFER_TO_GEN", "IDLE",
    };
    return ((unsigned)s < sizeof(names) / sizeof(names[0])) ? names[s] : "?";
}

bool amf_mains_healthy(const gcu_config_t *cfg, const gcu_inputs_t *in)
{
    for (int i = 0; i < 3; i++) {
        if (in->mains_v[i] < cfg->mains_under_v ||
            in->mains_v[i] > cfg->mains_over_v) {
            return false;
        }
    }
    return in->mains_hz >= cfg->mains_under_hz &&
           in->mains_hz <= cfg->mains_over_hz;
}

bool amf_gen_healthy(const gcu_config_t *cfg, const gcu_inputs_t *in)
{
    for (int i = 0; i < 3; i++) {
        if (in->gen_v[i] < cfg->gen_under_v || in->gen_v[i] > cfg->gen_over_v) {
            return false;
        }
    }
    return in->gen_hz >= cfg->gen_under_hz && in->gen_hz <= cfg->gen_over_hz;
}

static void enter(amf_fsm_t *a, amf_state_t s)
{
    a->state = s;
    a->state_ticks = 0;
}

void amf_fsm_tick(amf_fsm_t *a, const gcu_config_t *cfg,
                  const gcu_inputs_t *in, bool auto_mode,
                  bool engine_available, amf_demand_t *demand)
{
    if (a->state_ticks < UINT32_MAX) {
        a->state_ticks++;
    }

    /* Qualification counters run continuously (saturating). */
    if (amf_mains_healthy(cfg, in)) {
        if (a->mains_good_ticks < UINT32_MAX) a->mains_good_ticks++;
        a->mains_bad_ticks = 0;
    } else {
        if (a->mains_bad_ticks < UINT32_MAX) a->mains_bad_ticks++;
        a->mains_good_ticks = 0;
    }
    if (amf_gen_healthy(cfg, in) && engine_available) {
        if (a->gen_good_ticks < UINT32_MAX) a->gen_good_ticks++;
    } else {
        a->gen_good_ticks = 0;
    }

    /* Hysteresis latch for the non-AUTO mains contactor: open on a
     * qualified failure, re-close only after 2 s of healthy mains. */
    if (a->mains_bad_ticks >= GCU_MS_TO_TICKS(cfg->mains_fail_qualify_ms)) {
        a->mains_ctr_latched_open = true;
    } else if (a->mains_good_ticks >= GCU_MS_TO_TICKS(2000)) {
        a->mains_ctr_latched_open = false;
    }

    if (!auto_mode) {
        enter(a, AMF_IDLE);
        demand->engine_start = false;
        demand->gen_contactor = false;
        /* Qualified, chatter-free: stay closed through short dips; after a
         * qualified failure re-close only after 2 s of healthy mains. */
        demand->mains_contactor = !a->mains_ctr_latched_open;
        return;
    }

    bool mains_failed =
        a->mains_bad_ticks >= GCU_MS_TO_TICKS(cfg->mains_fail_qualify_ms);
    bool mains_returned =
        a->mains_good_ticks >= GCU_MS_TO_TICKS(cfg->mains_return_qualify_ms);
    bool gen_ready =
        a->gen_good_ticks >= GCU_MS_TO_TICKS(cfg->gen_ready_qualify_ms);

    switch (a->state) {
    case AMF_IDLE:
        enter(a, AMF_ON_MAINS);
        break;

    case AMF_ON_MAINS:
        if (mains_failed) {
            enter(a, AMF_STARTING);
        }
        break;

    case AMF_STARTING:
        if (mains_returned) {
            enter(a, AMF_ON_MAINS); /* flicker: mains came back first */
        } else if (gen_ready) {
            enter(a, AMF_TRANSFER_TO_GEN);
        }
        break;

    case AMF_TRANSFER_TO_GEN:
        /* Both contactors open for the dead time. */
        if (a->state_ticks >= GCU_MS_TO_TICKS(cfg->transfer_break_ms)) {
            enter(a, AMF_ON_GEN);
        }
        break;

    case AMF_ON_GEN:
        if (!engine_available) {
            /* Engine died under us: drop load, try mains if it is back. */
            enter(a, AMF_ON_MAINS);
        } else if (mains_returned) {
            enter(a, AMF_RETRANSFER);
        }
        break;

    case AMF_RETRANSFER:
        if (a->state_ticks >= GCU_MS_TO_TICKS(cfg->transfer_break_ms)) {
            enter(a, AMF_ON_MAINS); /* engine_start drops → cooldown → stop */
        }
        break;
    }

    demand->engine_start = (a->state == AMF_STARTING ||
                            a->state == AMF_TRANSFER_TO_GEN ||
                            a->state == AMF_ON_GEN ||
                            a->state == AMF_RETRANSFER);
    demand->gen_contactor = (a->state == AMF_ON_GEN) && engine_available;
    /* ON_MAINS keeps the contactor closed even through unqualified dips —
     * it carries no current on a dead bus, and per-tick tracking of
     * instantaneous health chatters the contactor on marginal mains.
     * ON_MAINS is only left via a qualified failure. */
    demand->mains_contactor = (a->state == AMF_ON_MAINS);

    /* Hard interlock: never both, regardless of any logic above. */
    if (demand->gen_contactor && demand->mains_contactor) {
        demand->gen_contactor = false;
        demand->mains_contactor = false;
    }
}
