/*
 * j1939.h — J1939 engine-ECU interface (supervisor side).
 *
 * ECU-25 does not manage the engine; the common-rail engine ECU does.
 * This module listens to the engine's broadcast PGNs, scales the SPNs,
 * ages every value, and hands the result to the same gcu_inputs_t the
 * legacy analog path fills — the control logic never knows the source.
 *
 * Received:
 *   EEC1   61444 (0xF004)  SPN 190 engine speed        (0.125 rpm/bit)
 *   EFL/P1 65263 (0xFEEF)  SPN 100 oil pressure        (4 kPa/bit)
 *   ET1    65262 (0xFEEE)  SPN 110 coolant temperature (1 C/bit, -40)
 *   DD     65276 (0xFEFC)  SPN  96 fuel level          (0.4 %/bit)
 *   DM1    65226 (0xFECA)  lamp status + first DTC (single frame only;
 *                          TP.BAM multi-DTC transfers are not reassembled
 *                          — the lamp bits still carry the severity)
 *   AC     60928 (0xEE00)  address claim (contention by lower NAME)
 *   RQST   59904 (0xEA00)  request (answered for the address claim PGN)
 *
 * Transmitted: our own address claim (SA 234, "generator set controller").
 *
 * Pure logic: no CAN driver here. The platform feeds frames into
 * j1939_rx() and drains j1939_tick()'s TX request into its driver.
 */
#ifndef J1939_H
#define J1939_H

#include "gcu_types.h"

typedef struct {
    uint32_t id;     /* 29-bit extended identifier */
    uint8_t dlc;
    uint8_t data[8];
} j1939_frame_t;

/* Ages saturate; a value is stale once its age exceeds its timeout. */
#define J1939_RPM_TIMEOUT_TICKS     GCU_MS_TO_TICKS(500)   /* EEC1 @ 10-20ms */
#define J1939_OIL_TIMEOUT_TICKS     GCU_MS_TO_TICKS(2000)  /* EFL/P1 @ 500ms */
#define J1939_COOLANT_TIMEOUT_TICKS GCU_MS_TO_TICKS(3000)  /* ET1 @ 1s      */
#define J1939_FUEL_TIMEOUT_TICKS    GCU_MS_TO_TICKS(5000)  /* DD @ 1s       */
#define J1939_DM1_TIMEOUT_TICKS     GCU_MS_TO_TICKS(3000)  /* DM1 @ 1s      */

#define J1939_ADDR_GENSET_CONTROLLER 234U
#define J1939_ADDR_ENGINE_1          0U
#define J1939_ADDR_NULL              254U  /* cannot-claim source address */

/* Grace after run-enable before silence counts as comms lost: the engine
 * ECU needs boot time (run_enable is asserted from PREHEAT to provide it). */
#define J1939_ECU_BOOT_GRACE_TICKS  GCU_MS_TO_TICKS(2000)

typedef struct {
    uint8_t self_addr;
    uint8_t engine_addr;
    uint64_t name;       /* our NAME; lower NAME wins address contention */

    /* Decoded engine data + ticks since last reception (saturating). */
    float rpm;
    float oil_bar;
    float coolant_c;
    float fuel_pct;
    uint32_t rpm_age;
    uint32_t oil_age;
    uint32_t coolant_age;
    uint32_t fuel_age;

    /* DM1 */
    bool red_lamp;       /* stop-engine lamp                  */
    bool amber_lamp;     /* warning lamp                      */
    uint32_t dm1_age;
    uint32_t dtc_spn;    /* first DTC of the last DM1, 0 = none */
    uint8_t dtc_fmi;

    /* Address claim (J1939-81, fixed address / AAC=0) */
    bool addr_conflict;     /* lost contention: no normal TX allowed  */
    bool claim_due;         /* address-claim frame wants transmitting */
    bool cannot_claim_due;  /* cannot-claim (SA 254) wants transmitting */

    uint32_t run_on_ticks;  /* ticks run_enable has been continuously on */
} j1939_state_t;

void j1939_init(j1939_state_t *j, uint8_t self_addr, uint8_t engine_addr);

/* Feed one received frame (any frame on the bus; non-engine ones ignored). */
void j1939_rx(j1939_state_t *j, const j1939_frame_t *f);

/*
 * One 10 ms tick: age the data, track run-enable time for the boot grace.
 * Returns true when *tx must be sent (address claim or cannot-claim).
 */
bool j1939_tick(j1939_state_t *j, bool run_enabled, j1939_frame_t *tx);

/*
 * Overwrite the engine fields of *in from J1939 data (value + valid flag
 * from age). Comms-lost asserts only after run-enable has been on past the
 * ECU boot grace: with K1 off the engine ECU sleeps and silence is normal.
 */
void j1939_fill_inputs(const j1939_state_t *j, gcu_inputs_t *in);

/* PGN of a 29-bit id (PDU2 keeps its group extension; PDU1 drops the DA). */
uint32_t j1939_pgn(uint32_t id);

#endif /* J1939_H */
