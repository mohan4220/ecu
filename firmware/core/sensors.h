/*
 * sensors.h — raw counts to physical units, plus input debouncing.
 *
 * The piece that sat between the ADC and gcu_inputs_t and did not exist:
 * every scenario test so far has handed the control logic ready-made bar and
 * degrees. Pure logic, so the conversions and the debounce are tested on the
 * PC rather than discovered on an engine.
 *
 * Sender curves are DATA, not code. The tables below are the common VDO /
 * Datcon shapes and are a starting point only — the resistance of the sender
 * actually screwed into your block is what matters, so measure it at two or
 * three known points and replace the table. A curve that is wrong by 20 %
 * silently shifts every oil-pressure trip.
 */
#ifndef ECU25_SENSORS_H
#define ECU25_SENSORS_H

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------- sender curves */

typedef struct {
    float ohms;  /* sender resistance                     */
    float value; /* physical quantity at that resistance  */
} sensor_point_t;

/*
 * Which end of the table is a FAULT, rather than just the end of the range.
 * This has to be stated per sender, not inferred, because it depends on what
 * the reading means and not on the shape of the curve:
 *
 *   oil pressure   short reads 0 bar and open reads full scale — both are
 *                  faults, and 0 bar in particular looks exactly like a real
 *                  low-oil shutdown
 *   fuel float     same in principle — but note that a 0-190 ohm float has
 *                  0 ohm INSIDE its range, so a short is physically
 *                  indistinguishable from a full tank and no code can tell
 *                  them apart. A sender whose table starts above 0 can
 *   NTC coolant    a short reads HOT. That is the safe direction to be wrong
 *                  in, so it stays valid and lets the over-temperature
 *                  shutdown fire; only the open circuit (which reads cold and
 *                  would mask an overheat) is a fault
 */
#define SENSOR_FAULT_LOW  0x1u   /* below the table is a broken sender */
#define SENSOR_FAULT_HIGH 0x2u   /* above the table is a broken sender */

typedef struct {
    const sensor_point_t *pts; /* ascending in ohms, at least 2 points */
    uint8_t n;
    uint8_t fault_ends;        /* SENSOR_FAULT_LOW | SENSOR_FAULT_HIGH */
} sensor_curve_t;

/*
 * Linear interpolation between table points. Outside the table the value is
 * clamped to the end point and *valid is cleared: an open circuit or a dead
 * short is a broken sender, not a reading of zero, and the control logic
 * treats invalid and zero very differently.
 */
float sensor_lookup(const sensor_curve_t *c, float ohms, bool *valid);

/* Typical curves. Verify against your senders before trusting a trip. */
extern const sensor_curve_t SENSOR_OIL_VDO_10BAR;   /* 10-184 ohm, 0-10 bar */
extern const sensor_curve_t SENSOR_TEMP_VDO_NTC;    /* ~300-30 ohm, 40-120 C */
extern const sensor_curve_t SENSOR_FUEL_0_190;      /* 0-190 ohm, 100-0 %   */

/*
 * Sender resistance from an ADC reading, given the excitation current the
 * front end forces through it. counts * lsb = volts across the sender.
 */
float sensor_ohms(uint16_t counts, float adc_lsb_v, float excite_a);

/* Divider input (battery, D+): counts -> volts at the terminal. */
float sensor_divider_v(uint16_t counts, float adc_lsb_v, float ratio);

/* ------------------------------------------------------------ engine speed */

/*
 * Magnetic pickup: the comparator gives one pulse per flywheel tooth, and
 * the timer captures the period. 0 us means no pulse since the last look,
 * which is a stopped engine, so this returns 0 rpm rather than dividing by
 * zero.
 */
float sensor_rpm(uint32_t period_us, uint16_t flywheel_teeth);

/* ---------------------------------------------------------- digital inputs */

#define DIN_COUNT 8

typedef struct {
    uint8_t stable;            /* last debounced state, bit per channel */
    uint8_t candidate;         /* state currently being confirmed       */
    uint8_t agree[DIN_COUNT];  /* consecutive samples agreeing          */
} din_debounce_t;

void din_init(din_debounce_t *d, uint8_t initial);

/*
 * Feed one raw sample of all eight channels. A channel changes state only
 * after `need` consecutive samples agree, so a single noisy read cannot
 * start an engine. At the 10 ms tick, need=3 is 30 ms.
 */
uint8_t din_update(din_debounce_t *d, uint8_t raw, uint8_t need);

#endif /* ECU25_SENSORS_H */
