/*
 * plant.h — fake 25 kVA genset for SIL testing.
 *
 * Simple first-order physics, one 10 ms step at a time: starter spins the
 * engine, fuel + enough RPM fires it, RPM seeks nominal, oil pressure and
 * generator volts/frequency follow RPM, coolant heats while running,
 * battery sags under crank. Fault injection knobs let scenarios break
 * things on purpose.
 */
#ifndef PLANT_H
#define PLANT_H

#include "../core/gcu_types.h"

typedef struct {
    /* State */
    float rpm;
    float coolant_c;
    float battery_v;
    bool fired; /* combustion self-sustaining */

    /* Fault injection */
    bool fuel_blocked;      /* engine will never fire            */
    bool oil_pump_broken;   /* oil pressure stays 0 when running */
    bool charge_alt_broken; /* D+ stays low                      */
    bool governor_runaway;  /* rpm climbs past nominal           */
    bool stuck_fuel;        /* engine keeps running after fuel cut */
    bool mains_on;          /* utility present                   */
    float load_pct;         /* 0..100 applied when gen contactor closed */
} plant_t;

void plant_init(plant_t *p);

/* Advance one tick under the controller's outputs; fill controller inputs. */
void plant_step(plant_t *p, const gcu_outputs_t *out, gcu_inputs_t *in);

#endif /* PLANT_H */
