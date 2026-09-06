#include "gcu_types.h"

void gcu_config_defaults(gcu_config_t *cfg)
{
    /* Engine sequencing */
    cfg->preheat_ms = 5000;
    /* K5/K6 defaults preserve the old fixed behaviour */
    /* Backlight: 80 % is readable in a panel without cooking the LEDs.
     * Shed below 10.0 V and restore above 11.0 V — the 1.0 V gap is wider
     * than any plausible ripple, so the panel cannot flicker. */
    cfg->backlight_pct = 80;
    cfg->backlight_shed_v = 10.0f;
    cfg->backlight_restore_v = 11.0f;

    cfg->aux1_fn = AUX_HORN;
    cfg->aux2_fn = AUX_PREHEAT;
    cfg->crank_ms = 8000;
    cfg->crank_rest_ms = 10000;
    cfg->crank_attempts = 3;
    cfg->crank_disconnect_rpm = 500.0f;
    cfg->crank_disconnect_oil_bar = 1.0f;
    cfg->warmup_ms = 10000;
    cfg->cooldown_ms = 120000;
    cfg->stop_timeout_ms = 45000;
    cfg->nominal_rpm = 1500.0f;

    /* Protections */
    cfg->overspeed_rpm = 1725.0f; /* 115 % of nominal */
    cfg->low_oil_bar = 1.5f;
    cfg->low_oil_delay_ms = 2000;
    cfg->high_coolant_c = 98.0f;
    cfg->gen_under_v = 196.0f; /* -18 % of 240 L-N */
    cfg->gen_over_v = 276.0f;  /* +15 % of 240 L-N */
    cfg->gen_volt_delay_ms = 5000;
    cfg->gen_under_hz = 45.0f;
    cfg->gen_over_hz = 55.0f;
    cfg->gen_freq_delay_ms = 5000;
    cfg->overcurrent_a = 40.0f; /* ~115 % of 34.8 A at 25 kVA */
    cfg->overcurrent_delay_ms = 20000;
    /* 12 V system: rest 12.6 V, charging 14.2-14.8 V, crank sag to ~8.5 V.
     * The old 22/30 V pair was left over from the 24 V design and made
     * batt_low permanently true, which pinned the horn relay on. */
    cfg->batt_low_v = 11.0f;
    cfg->batt_high_v = 15.5f;
    cfg->batt_delay_ms = 60000;
    cfg->charge_fail_ratio = 0.5f;
    cfg->charge_fail_delay_ms = 15000;
    /* Must be <= gen_ready_qualify + transfer_break so AC protections
     * are armed before the gen contactor can close. */
    cfg->protections_arm_delay_ms = 5000;

    /* AMF (mains window on 240 V L-N) */
    cfg->mains_under_v = 196.0f;
    cfg->mains_over_v = 276.0f;
    cfg->mains_under_hz = 47.0f;
    cfg->mains_over_hz = 53.0f;
    cfg->mains_fail_qualify_ms = 5000;
    cfg->mains_return_qualify_ms = 30000;
    cfg->transfer_break_ms = 1000;
    cfg->gen_ready_qualify_ms = 5000;
}
