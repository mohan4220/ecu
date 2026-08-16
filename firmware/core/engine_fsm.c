#include "engine_fsm.h"

void engine_fsm_init(engine_fsm_t *f)
{
    f->state = ENG_STOPPED;
    f->state_ticks = 0;
    f->attempt = 0;
    f->fail_to_start = false;
    f->fail_to_stop = false;
    f->standstill_seen = false;
}

bool engine_fsm_running(const engine_fsm_t *f)
{
    return f->state == ENG_WARMUP || f->state == ENG_RUNNING ||
           f->state == ENG_COOLDOWN;
}

const char *engine_state_name(engine_state_t s)
{
    static const char *names[] = {
        "STOPPED", "PREHEAT",  "CRANK",    "CRANK_REST", "WARMUP",
        "RUNNING", "COOLDOWN", "STOPPING", "SHUTDOWN",
    };
    return ((unsigned)s < sizeof(names) / sizeof(names[0])) ? names[s] : "?";
}

static void enter(engine_fsm_t *f, engine_state_t s)
{
    f->state = s;
    f->state_ticks = 0;
}

void engine_fsm_tick(engine_fsm_t *f, const gcu_config_t *cfg,
                     const gcu_inputs_t *in, const engine_cmd_t *cmd,
                     gcu_outputs_t *out)
{
    /* Event flags last one tick. */
    f->fail_to_start = false;
    f->fail_to_stop = false;
    if (f->state_ticks < UINT32_MAX) {
        f->state_ticks++;
    }

    /* An immediate stop overrides everything except staying stopped. */
    if (cmd->immediate_stop && f->state != ENG_STOPPED &&
        f->state != ENG_SHUTDOWN) {
        enter(f, ENG_SHUTDOWN);
    }

    /* Three independent crank-disconnect / running-detect sources: rpm,
     * oil pressure, and generator frequency (protects against rpm+oil
     * sensor loss — industry practice). */
    bool fired = (in->rpm_valid && in->rpm >= cfg->crank_disconnect_rpm) ||
                 (in->oil_pressure_valid &&
                  in->oil_pressure_bar >= cfg->crank_disconnect_oil_bar) ||
                 (in->gen_hz > 20.0f);

    /* Standstill memory: while stopping/shut down, a fresh rpm reading
     * below 10 proves the engine reached standstill — remember it, because
     * a battery-powered J1939 engine ECU then goes to sleep and rpm turns
     * stale before the operator walks over to reset. */
    if ((f->state == ENG_STOPPING || f->state == ENG_SHUTDOWN) &&
        in->rpm_valid && in->rpm < 10.0f) {
        f->standstill_seen = true;
    }

    switch (f->state) {
    case ENG_STOPPED:
        f->attempt = 0;
        f->standstill_seen = false;
        if (cmd->start_requested && !cmd->immediate_stop) {
            /* Rotation inhibit: never crank an engine that is already
             * turning (e.g. controller rebooted while set was running). */
            enter(f, fired ? ENG_WARMUP : ENG_PREHEAT);
        }
        break;

    case ENG_PREHEAT:
        if (!cmd->start_requested) {
            enter(f, ENG_STOPPED);
        } else if (fired) {
            enter(f, ENG_WARMUP); /* already rotating: do not crank */
        } else if (f->state_ticks >= GCU_MS_TO_TICKS(cfg->preheat_ms)) {
            f->attempt = 1;
            enter(f, ENG_CRANK);
        }
        break;

    case ENG_CRANK:
        if (!cmd->start_requested) {
            enter(f, ENG_STOPPING);
        } else if (fired) {
            enter(f, ENG_WARMUP); /* crank disconnect */
        } else if (f->state_ticks >= GCU_MS_TO_TICKS(cfg->crank_ms)) {
            if (f->attempt >= cfg->crank_attempts) {
                f->fail_to_start = true;
                enter(f, ENG_SHUTDOWN);
            } else {
                enter(f, ENG_CRANK_REST);
            }
        }
        break;

    case ENG_CRANK_REST:
        if (!cmd->start_requested) {
            enter(f, ENG_STOPPED);
        } else if (f->state_ticks >= GCU_MS_TO_TICKS(cfg->crank_rest_ms)) {
            f->attempt++;
            enter(f, ENG_CRANK);
        }
        break;

    case ENG_WARMUP:
        if (!cmd->start_requested || cmd->stop_requested) {
            enter(f, ENG_STOPPING); /* not warm: no cooldown needed */
        } else if (f->state_ticks >= GCU_MS_TO_TICKS(cfg->warmup_ms)) {
            enter(f, ENG_RUNNING);
        }
        break;

    case ENG_RUNNING:
        if (!cmd->start_requested || cmd->stop_requested) {
            enter(f, cmd->skip_cooldown ? ENG_STOPPING : ENG_COOLDOWN);
        }
        break;

    case ENG_COOLDOWN:
        if (cmd->start_requested && !cmd->stop_requested) {
            enter(f, ENG_RUNNING); /* demand returned during cooldown */
        } else if (cmd->skip_cooldown ||
                   f->state_ticks >= GCU_MS_TO_TICKS(cfg->cooldown_ms)) {
            enter(f, ENG_STOPPING);
        }
        break;

    case ENG_STOPPING:
        if (in->rpm_valid && in->rpm < 10.0f) {
            enter(f, ENG_STOPPED);
        } else if (f->state_ticks >= GCU_MS_TO_TICKS(cfg->stop_timeout_ms)) {
            f->fail_to_stop = true;
            enter(f, ENG_SHUTDOWN);
        }
        break;

    case ENG_SHUTDOWN: {
        /* Fuel already cut below. Leave only via operator reset, and only
         * with positive evidence the engine is stopped. A lost rpm sensor
         * is NOT evidence of standstill: require dead generator output
         * plus a conservative spin-down time instead. */
        bool stopped_evidence =
            (in->rpm_valid && in->rpm < 10.0f) || f->standstill_seen ||
            (!in->rpm_valid && in->gen_hz < 5.0f &&
             f->state_ticks >= GCU_MS_TO_TICKS(cfg->stop_timeout_ms));
        if (cmd->alarm_reset && stopped_evidence) {
            enter(f, ENG_STOPPED);
        }
        break;
    }
    }

    /* Outputs derive purely from state. */
    out->preheat = (f->state == ENG_PREHEAT);
    out->starter = (f->state == ENG_CRANK);
    /* run_enable already during PREHEAT: gives a J1939 engine ECU its boot
     * time before the starter engages (so crank-disconnect has live rpm),
     * and merely energizes the fuel solenoid early in legacy mode. */
    out->run_enable = (f->state == ENG_PREHEAT || f->state == ENG_CRANK ||
                       f->state == ENG_WARMUP || f->state == ENG_RUNNING ||
                       f->state == ENG_COOLDOWN);
}
