/*
 * sensors.c — raw counts to physical units. See sensors.h.
 */
#include "sensors.h"

#include <string.h>

/* ------------------------------------------------------- sender curves */

/* VDO/Datcon 10 bar pressure sender: ~10 ohm at 0 bar rising to ~184 ohm at
 * full scale, close enough to linear that four points carry it. */
static const sensor_point_t OIL_PTS[] = {
    {10.0f, 0.0f}, {62.0f, 3.0f}, {123.0f, 6.5f}, {184.0f, 10.0f},
};
const sensor_curve_t SENSOR_OIL_VDO_10BAR = {OIL_PTS, 4};

/* NTC coolant sender: resistance FALLS as temperature rises, so the table is
 * still ascending in ohms but descending in degrees. Six points, because an
 * NTC is nothing like linear. */
static const sensor_point_t TEMP_PTS[] = {
    {22.0f, 120.0f}, {32.0f, 110.0f}, {51.0f, 100.0f},
    {96.0f, 80.0f},  {197.0f, 60.0f}, {323.0f, 40.0f},
};
const sensor_curve_t SENSOR_TEMP_VDO_NTC = {TEMP_PTS, 6};

/* Fuel float, 0 ohm full to 190 ohm empty (the European convention; the US
 * one is 240-33 and inverted, which is exactly why this is a table). */
static const sensor_point_t FUEL_PTS[] = {
    {0.0f, 100.0f}, {95.0f, 50.0f}, {190.0f, 0.0f},
};
const sensor_curve_t SENSOR_FUEL_0_190 = {FUEL_PTS, 3};

float sensor_lookup(const sensor_curve_t *c, float ohms, bool *valid)
{
    if (valid) {
        *valid = true;
    }
    if (!c || c->n < 2 || !c->pts) {
        if (valid) {
            *valid = false;
        }
        return 0.0f;
    }
    /*
     * Outside the table, with a tolerance band.
     *
     * A sender sitting exactly at the end of its range — an oil sender at
     * 0 bar is right on the bottom point — must not flicker invalid because
     * one ADC count of noise pushed it 0.01 ohm under. So the value clamps
     * and stays VALID within a band, and only a genuine short (under half
     * the bottom point) or open circuit (over 1.5x the top) is called a
     * broken sender. Those two are what actually happen in a loom: chafed
     * insulation to the block, or a spade pushed off a terminal.
     */
    float lo = c->pts[0].ohms, hi = c->pts[c->n - 1].ohms;
    if (ohms < lo) {
        if (valid && ohms < lo * 0.5f) {
            *valid = false;
        }
        return c->pts[0].value;
    }
    if (ohms > hi) {
        if (valid && ohms > hi * 1.5f) {
            *valid = false;
        }
        return c->pts[c->n - 1].value;
    }
    for (uint8_t i = 1; i < c->n; i++) {
        if (ohms <= c->pts[i].ohms) {
            float r0 = c->pts[i - 1].ohms, r1 = c->pts[i].ohms;
            float v0 = c->pts[i - 1].value, v1 = c->pts[i].value;
            float span = r1 - r0;
            if (span <= 0.0f) {
                return v1;
            }
            return v0 + (v1 - v0) * (ohms - r0) / span;
        }
    }
    return c->pts[c->n - 1].value;
}

float sensor_ohms(uint16_t counts, float adc_lsb_v, float excite_a)
{
    if (excite_a <= 0.0f) {
        return 0.0f;
    }
    return ((float)counts * adc_lsb_v) / excite_a;
}

float sensor_divider_v(uint16_t counts, float adc_lsb_v, float ratio)
{
    return (float)counts * adc_lsb_v * ratio;
}

/* ------------------------------------------------------------ engine speed */

float sensor_rpm(uint32_t period_us, uint16_t flywheel_teeth)
{
    if (period_us == 0 || flywheel_teeth == 0) {
        return 0.0f;
    }
    /* One tooth per period: teeth per second / teeth per rev * 60. */
    float teeth_per_s = 1000000.0f / (float)period_us;
    return teeth_per_s * 60.0f / (float)flywheel_teeth;
}

/* ---------------------------------------------------------- digital inputs */

void din_init(din_debounce_t *d, uint8_t initial)
{
    memset(d, 0, sizeof(*d));
    d->stable = initial;
    d->candidate = initial;
}

uint8_t din_update(din_debounce_t *d, uint8_t raw, uint8_t need)
{
    if (need == 0) {
        need = 1;
    }
    for (int i = 0; i < DIN_COUNT; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        bool now = (raw & bit) != 0;
        bool stable = (d->stable & bit) != 0;
        if (now == stable) {
            d->agree[i] = 0;   /* already there; nothing to confirm */
            continue;
        }
        d->agree[i]++;
        if (d->agree[i] >= need) {
            d->stable = now ? (uint8_t)(d->stable | bit)
                            : (uint8_t)(d->stable & ~bit);
            d->agree[i] = 0;
        }
    }
    d->candidate = raw;
    return d->stable;
}
