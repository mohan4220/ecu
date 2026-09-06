/*
 * ac_sense.h — AC measurement layer: raw ADC samples in, RMS / frequency /
 * real power / power factor out.
 *
 * Pure logic, like j1939.c and modbus.c: no ADC, no DMA, no HAL. The platform
 * hands over one synchronised set of samples per conversion trigger and this
 * module accumulates a measurement window, so the whole thing runs and is
 * tested on the PC.
 *
 * All nine AC channels are sampled together and share a bias: the six voltage
 * dividers and the three CT amplifiers all sit on the VREF_MID 1.65 V rail,
 * so a "zero" sample is mid-scale, not 0. The DC offset is measured, not
 * assumed — VREF_MID drifts with temperature and the whole point of the
 * buffered star network is that it is stable, not that it is exact.
 *
 * Method, per window:
 *   RMS      sqrt(mean(x^2) - mean(x)^2)   — the identity avoids needing the
 *            mean before the samples arrive, so nothing has to be buffered
 *   power    mean(v*i) - mean(v)*mean(i)   — same identity, per phase, summed
 *   PF       |P| / sum(Vrms_i * Irms_i)
 *   freq     interpolated rising zero crossings of (x - DC) on L1
 *
 * Accumulators are 64-bit over raw counts, so the arithmetic is exact until
 * the scaling at the end of the window: 4095^2 * 3200 samples is 5.4e10, well
 * inside uint64 and hopeless in uint32.
 */
#ifndef ECU25_AC_SENSE_H
#define ECU25_AC_SENSE_H

#include <stdbool.h>
#include <stdint.h>
#include "gcu_types.h"

enum {
    AC_GEN_L1 = 0, AC_GEN_L2, AC_GEN_L3,
    AC_MAINS_L1, AC_MAINS_L2, AC_MAINS_L3,
    AC_I_L1, AC_I_L2, AC_I_L3,
    AC_CH_COUNT
};

#define AC_MAX_WINDOW 4096u

typedef struct {
    /* Scaling from one ADC count at the MCU pin to the field quantity. */
    float v_per_count;  /* line-to-neutral volts per count */
    float a_per_count;  /* primary amps per count          */

    uint32_t sample_hz; /* per-channel sample rate                    */
    uint16_t window;    /* samples per measurement window (1..4096)   */
    uint16_t mid_count; /* initial DC estimate, i.e. VREF_MID in counts */

    /* Below these an input is called dead: volts and frequency both report
     * zero rather than letting ADC noise manufacture a reading. */
    float v_dead;
    float i_dead;
} ac_cal_t;

typedef struct {
    float gen_v[3];
    float mains_v[3];
    float load_a[3];
    float gen_hz;
    float mains_hz;
    float real_power_w; /* total, SIGNED — negative is reverse power */
    float power_factor; /* 0..1, unsigned                            */
} ac_result_t;

typedef struct {
    ac_cal_t cal;

    uint64_t sum[AC_CH_COUNT];   /* Sx  over raw counts */
    uint64_t sumsq[AC_CH_COUNT]; /* Sxx over raw counts */
    uint64_t sumvi[3];           /* Svi, gen phase n against current n */
    uint32_t n;                  /* samples in the current window */

    float dc[AC_CH_COUNT];       /* DC estimate in counts, carried forward */

    /* Zero-crossing trackers: [0] = generator L1, [1] = mains L1 */
    struct {
        float prev;
        bool have_prev;
        uint32_t crossings;
        float first;  /* fractional sample index of the first rising cross */
        float last;
    } zc[2];

    ac_result_t out;
    bool valid;      /* out holds a completed window                     */
    uint32_t windows; /* completed windows since init                    */
} ac_sense_t;

/*
 * Fill cal with the ECU-25 hardware's own numbers: 12-bit ADC on a 3.3 V
 * reference, the 4x330k + 5.62k divider (1/235.9), and a CT of ct_primary:5
 * into the 0.05 ohm burden with a gain of 2. Everything else is defaulted;
 * override fields afterwards if the board is stuffed differently.
 */
void ac_cal_defaults(ac_cal_t *cal, float ct_primary);

void ac_sense_init(ac_sense_t *ac, const ac_cal_t *cal);

/*
 * Feed one synchronised sample set (raw ADC counts, AC_CH_COUNT of them).
 * Returns true when this sample completed a window, i.e. ac->out is fresh.
 */
bool ac_sense_push(ac_sense_t *ac, const uint16_t *raw);

/* Copy the last completed window into the controller's input struct. */
void ac_sense_fill(const ac_sense_t *ac, gcu_inputs_t *in);

#endif /* ECU25_AC_SENSE_H */
