/*
 * gcu_types.h — shared types for ECU-25 pure logic modules.
 *
 * These modules are hardware-free: inputs in, decisions out, fixed tick.
 * The same code runs on the host (SIL) and on the STM32 target.
 */
#ifndef GCU_TYPES_H
#define GCU_TYPES_H

#include <stdbool.h>
#include <stdint.h>

/* One logic tick = 10 ms. All timers count ticks. */
#define GCU_TICK_MS 10U
#define GCU_MS_TO_TICKS(ms) ((uint32_t)((ms) / GCU_TICK_MS))

/* ------------------------------------------------------------------ inputs */

typedef struct {
    /* Engine measurements (source-selected upstream: J1939 or analog) */
    float rpm;              /* engine speed, 0 when stopped            */
    bool rpm_valid;         /* false = sensor/PGN missing              */
    float oil_pressure_bar; /* 0 when stopped                          */
    bool oil_pressure_valid;
    float coolant_temp_c;
    bool coolant_temp_valid;
    float fuel_level_pct; /* 0..100                                  */
    bool fuel_level_valid;

    /* Electrical measurements */
    float gen_v[3];   /* generator L-N RMS volts per phase       */
    float gen_hz;     /* generator frequency (0 when dead)       */
    float mains_v[3]; /* mains L-N RMS volts per phase           */
    float mains_hz;
    float load_a[3]; /* per-phase load current, amps            */
    float real_power_w;  /* total real power, mean(v*i). 0 = not measured */
    float power_factor;  /* 0..1. 0 = not measured                       */
    float battery_v;
    float dplus_v; /* charge alternator D+                    */

    /* Digital inputs (already debounced) */
    bool emergency_stop; /* true = e-stop pressed (fail-safe read)  */
    bool remote_start;   /* external start request (AUTO mode)      */
    bool low_oil_switch; /* engine's own LOP switch                 */
    bool high_coolant_switch;
    bool low_coolant_level;

    /* Operator (from HMI keys) */
    bool key_start; /* momentary                               */
    bool key_stop;  /* momentary; also resets latched alarms   */
    bool mode_auto; /* AUTO selected (else MANUAL)             */

    /* J1939 supervisor mode (all false in legacy analog mode) */
    bool ecu_red_lamp;   /* engine ECU DM1 stop lamp — shutdown  */
    bool ecu_amber_lamp; /* engine ECU DM1 warning lamp          */
    bool ecu_comms_lost; /* engine PGNs silent while run enabled */
} gcu_inputs_t;

/* ----------------------------------------------------------------- outputs */

typedef struct {
    bool run_enable;     /* K1 — engine ECU run / fuel solenoid     */
    bool starter;        /* K2                                       */
    bool gen_contactor;  /* K3                                       */
    bool mains_contactor;/* K4                                       */
    /* K5/K6 are configurable. aux1/aux2 carry whatever gcu_aux_fn_t the
     * config assigns; horn and preheat are the defaults. The engine ECU owns
     * the cold-start aid on a CRDi engine, so a hard-wired preheat channel
     * would often sit idle. */
    bool aux1;           /* K5 — default: horn                       */
    bool aux2;           /* K6 — default: preheat                    */
    /* Display backlight duty, 0-100 %. Not a relay: it is the one load the
     * controller can shed, and the crank hold-up budget depends on it. */
    uint8_t backlight_pct;
} gcu_outputs_t;

/* What a configurable AUX relay follows. */
typedef enum {
    AUX_OFF = 0,      /* channel unused                              */
    AUX_HORN,         /* audible alarm: any shutdown, or unacked warn */
    AUX_PREHEAT,      /* energised during the PREHEAT state           */
    AUX_RUNNING,      /* closed whenever the engine is running        */
    AUX_FAULT,        /* closed on any latched shutdown               */
} gcu_aux_fn_t;

/* ------------------------------------------------------------------ config */

typedef struct {
    /* Engine sequencing (times in ms) */
    uint32_t preheat_ms;
    uint32_t crank_ms;            /* max time per crank attempt       */
    uint32_t crank_rest_ms;
    uint8_t crank_attempts;
    float crank_disconnect_rpm;   /* engine fired above this          */
    float crank_disconnect_oil_bar;
    uint32_t warmup_ms;
    uint32_t cooldown_ms;
    uint32_t stop_timeout_ms;     /* rpm must reach 0 within this     */
    float nominal_rpm;

    /* Configurable relay outputs K5/K6 */
    gcu_aux_fn_t aux1_fn;
    gcu_aux_fn_t aux2_fn;

    /* Display backlight and its crank-time load shed */
    uint8_t backlight_pct;    /* normal duty                          */
    float backlight_shed_v;   /* shed at or below this battery volts  */
    float backlight_restore_v;/* restore above this (hysteresis)      */

    /* Protections */
    float overspeed_rpm;
    float low_oil_bar;            /* shutdown below this when running */
    uint32_t low_oil_delay_ms;
    float high_coolant_c;
    float gen_under_v;            /* per-phase L-N                    */
    float gen_over_v;
    uint32_t gen_volt_delay_ms;
    float gen_under_hz;
    float gen_over_hz;
    uint32_t gen_freq_delay_ms;
    float overcurrent_a;
    uint32_t overcurrent_delay_ms;
    float batt_low_v;
    float batt_high_v;
    uint32_t batt_delay_ms;
    float charge_fail_ratio;      /* D+ below this fraction of Vbatt  */
    uint32_t charge_fail_delay_ms;
    uint32_t protections_arm_delay_ms; /* after warmup: volt/freq arm  */

    /* AMF */
    float mains_under_v;
    float mains_over_v;
    float mains_under_hz;
    float mains_over_hz;
    uint32_t mains_fail_qualify_ms;   /* mains must be bad this long   */
    uint32_t mains_return_qualify_ms; /* mains must be good this long  */
    uint32_t transfer_break_ms;       /* dead time, break-before-make  */
    uint32_t gen_ready_qualify_ms;    /* gen volts/freq healthy time   */
} gcu_config_t;

/* Factory defaults: 25 kVA, 415 V, 50 Hz, 12 V electrical system. */
void gcu_config_defaults(gcu_config_t *cfg);

#endif /* GCU_TYPES_H */
