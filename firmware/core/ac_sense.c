/*
 * ac_sense.c — AC measurement layer. See ac_sense.h for the method.
 */
#include "ac_sense.h"

#include <math.h>
#include <string.h>

void ac_cal_defaults(ac_cal_t *cal, float ct_primary)
{
    memset(cal, 0, sizeof(*cal));

    /* 12-bit ADC, 3.3 V reference. */
    const float lsb = 3.3f / 4095.0f;

    /* Voltage chain: 4 x 330k over 5.62k, so the pin sees line/235.9. */
    cal->v_per_count = lsb * 235.9f;

    /* Current chain: CT steps ct_primary down to 5 A, the 0.05 ohm burden
     * turns that into volts, and the MCP6002 stage has a gain of 2. */
    const float burden = 0.05f, gain = 2.0f;
    cal->a_per_count = lsb / (burden * gain) * (ct_primary / 5.0f);

    /* 3200 Hz gives 64 samples per 50 Hz cycle; a 320-sample window is
     * 100 ms, which is exactly 5 cycles at 50 Hz and 6 at 60 Hz — a whole
     * number either way, so no scalloping in the RMS. */
    cal->sample_hz = 3200;
    cal->window = 320;
    cal->mid_count = 2048;

    /* 20 V is far below any under-voltage trip and far above ADC noise;
     * 0.5 A likewise sits under the smallest load worth reporting. */
    cal->v_dead = 20.0f;
    cal->i_dead = 0.5f;
}

void ac_sense_init(ac_sense_t *ac, const ac_cal_t *cal)
{
    memset(ac, 0, sizeof(*ac));
    ac->cal = *cal;
    if (ac->cal.window == 0 || ac->cal.window > AC_MAX_WINDOW) {
        ac->cal.window = 320;
    }
    if (ac->cal.sample_hz == 0) {
        ac->cal.sample_hz = 3200;
    }
    for (int c = 0; c < AC_CH_COUNT; c++) {
        ac->dc[c] = (float)ac->cal.mid_count;
    }
}

void ac_sense_reset(ac_sense_t *ac)
{
    memset(ac->sum, 0, sizeof(ac->sum));
    memset(ac->sumsq, 0, sizeof(ac->sumsq));
    memset(ac->sumvi, 0, sizeof(ac->sumvi));
    memset(ac->zc, 0, sizeof(ac->zc));
    ac->n = 0;
    ac->gaps++;
}

/* Track rising zero crossings of one channel, in fractional sample index. */
static void zc_step(ac_sense_t *ac, int slot, float x, uint32_t idx)
{
    if (ac->zc[slot].have_prev) {
        float prev = ac->zc[slot].prev;
        if (prev < 0.0f && x >= 0.0f) {
            /* Linear interpolation between the straddling samples: without
             * it the frequency quantises to sample_hz/N and a 50.0 Hz set
             * would read 49.2 or 50.8 with nothing in between. */
            float frac = (x != prev) ? (-prev / (x - prev)) : 0.0f;
            float pos = (float)(idx - 1) + frac;
            if (ac->zc[slot].crossings == 0) {
                ac->zc[slot].first = pos;
            }
            ac->zc[slot].last = pos;
            ac->zc[slot].crossings++;
        }
    }
    ac->zc[slot].prev = x;
    ac->zc[slot].have_prev = true;
}

static float zc_freq(const ac_sense_t *ac, int slot)
{
    if (ac->zc[slot].crossings < 2) {
        return 0.0f;
    }
    float span = ac->zc[slot].last - ac->zc[slot].first;
    if (span <= 0.0f) {
        return 0.0f;
    }
    float cycles = (float)(ac->zc[slot].crossings - 1);
    return cycles * (float)ac->cal.sample_hz / span;
}

/* mean(x) and sqrt(mean(x^2) - mean(x)^2) in raw counts. */
static void chan_stats(const ac_sense_t *ac, int ch, float *mean, float *rms)
{
    float n = (float)ac->n;
    float m = (float)ac->sum[ch] / n;
    float ms = (float)ac->sumsq[ch] / n;
    float var = ms - m * m;
    if (var < 0.0f) {
        var = 0.0f; /* rounding only; a real variance cannot be negative */
    }
    *mean = m;
    *rms = sqrtf(var);
}

static void finish_window(ac_sense_t *ac)
{
    ac_result_t r;
    memset(&r, 0, sizeof(r));

    float mean[AC_CH_COUNT], rms[AC_CH_COUNT];
    for (int c = 0; c < AC_CH_COUNT; c++) {
        chan_stats(ac, c, &mean[c], &rms[c]);
        ac->dc[c] = mean[c]; /* exact bias, carried into the next window */
    }

    for (int i = 0; i < 3; i++) {
        float v = rms[AC_GEN_L1 + i] * ac->cal.v_per_count;
        float m = rms[AC_MAINS_L1 + i] * ac->cal.v_per_count;
        float a = rms[AC_I_L1 + i] * ac->cal.a_per_count;
        r.gen_v[i] = (v >= ac->cal.v_dead) ? v : 0.0f;
        r.mains_v[i] = (m >= ac->cal.v_dead) ? m : 0.0f;
        r.load_a[i] = (a >= ac->cal.i_dead) ? a : 0.0f;
    }

    /* Real power per phase: mean(v*i) - mean(v)*mean(i), in counts squared,
     * then scaled once. A phase whose voltage is dead contributes nothing —
     * otherwise CT noise on an open contactor reads as power. */
    float n = (float)ac->n;
    float p_total = 0.0f, s_total = 0.0f;
    for (int i = 0; i < 3; i++) {
        /* Both halves must be live. Summing power from a phase whose
         * current was suppressed by i_dead put watts into the total against
         * a published 0.0 A, and pushed the PF ratio over 1 often enough to
         * hit the clamp. */
        if (r.gen_v[i] == 0.0f || r.load_a[i] == 0.0f) {
            continue;
        }
        float mvi = (float)ac->sumvi[i] / n;
        float cov = mvi - mean[AC_GEN_L1 + i] * mean[AC_I_L1 + i];
        p_total += cov * ac->cal.v_per_count * ac->cal.a_per_count;
        s_total += r.gen_v[i] * r.load_a[i];
    }
    r.real_power_w = p_total;
    if (s_total > 0.0f) {
        float pf = fabsf(p_total) / s_total;
        r.power_factor = (pf > 1.0f) ? 1.0f : pf;
    }

    r.gen_hz = (r.gen_v[0] > 0.0f) ? zc_freq(ac, 0) : 0.0f;
    r.mains_hz = (r.mains_v[0] > 0.0f) ? zc_freq(ac, 1) : 0.0f;

    ac->out = r;
    ac->valid = true;
    ac->windows++;

    memset(ac->sum, 0, sizeof(ac->sum));
    memset(ac->sumsq, 0, sizeof(ac->sumsq));
    memset(ac->sumvi, 0, sizeof(ac->sumvi));
    memset(ac->zc, 0, sizeof(ac->zc));
    ac->n = 0;
}

bool ac_sense_push(ac_sense_t *ac, const uint16_t *raw)
{
    for (int c = 0; c < AC_CH_COUNT; c++) {
        uint64_t v = raw[c];
        ac->sum[c] += v;
        ac->sumsq[c] += v * v;
    }
    for (int i = 0; i < 3; i++) {
        ac->sumvi[i] += (uint64_t)raw[AC_GEN_L1 + i] * raw[AC_I_L1 + i];
    }

    /* Zero crossings use the DC estimate carried from the previous window;
     * the first window after init uses mid_count and self-corrects. */
    zc_step(ac, 0, (float)raw[AC_GEN_L1] - ac->dc[AC_GEN_L1], ac->n);
    zc_step(ac, 1, (float)raw[AC_MAINS_L1] - ac->dc[AC_MAINS_L1], ac->n);

    ac->n++;
    if (ac->n >= ac->cal.window) {
        finish_window(ac);
        return true;
    }
    return false;
}

void ac_sense_fill(const ac_sense_t *ac, gcu_inputs_t *in)
{
    if (!ac->valid) {
        return; /* nothing measured yet: leave the caller's defaults alone */
    }
    for (int i = 0; i < 3; i++) {
        in->gen_v[i] = ac->out.gen_v[i];
        in->mains_v[i] = ac->out.mains_v[i];
        in->load_a[i] = ac->out.load_a[i];
    }
    in->gen_hz = ac->out.gen_hz;
    in->mains_hz = ac->out.mains_hz;
    in->real_power_w = ac->out.real_power_w;
    in->power_factor = ac->out.power_factor;
}
