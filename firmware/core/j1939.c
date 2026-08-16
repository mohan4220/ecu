#include "j1939.h"

#include <string.h>

#define PGN_EEC1   61444U
#define PGN_EFLP1  65263U
#define PGN_ET1    65262U
#define PGN_DD     65276U
#define PGN_DM1    65226U
#define PGN_CLAIM  60928U
#define PGN_RQST   59904U

uint32_t j1939_pgn(uint32_t id)
{
    uint8_t pf = (id >> 16) & 0xFFU;
    if (pf < 240U) {
        return (id >> 8) & 0x3FF00U;  /* PDU1: DA byte is not part of PGN */
    }
    return (id >> 8) & 0x3FFFFU;      /* PDU2 */
}

static uint8_t src_addr(uint32_t id) { return id & 0xFFU; }

static void age_reset(uint32_t *age) { *age = 0; }

void j1939_init(j1939_state_t *j, uint8_t self_addr, uint8_t engine_addr)
{
    memset(j, 0, sizeof(*j));
    j->self_addr = self_addr;
    j->engine_addr = engine_addr;
    /* Arbitrary fixed NAME: identity fields chosen once for this design;
     * what matters for contention is that it is stable. */
    j->name = 0x8000A0287D010001ULL;
    j->rpm_age = UINT32_MAX;
    j->oil_age = UINT32_MAX;
    j->coolant_age = UINT32_MAX;
    j->fuel_age = UINT32_MAX;
    j->dm1_age = UINT32_MAX;
    j->claim_due = true;  /* announce ourselves at power-up */
}

static uint64_t name_of(const j1939_frame_t *f)
{
    uint64_t n = 0;
    for (int i = 7; i >= 0; i--) {
        n = (n << 8) | f->data[i];
    }
    return n;
}

void j1939_rx(j1939_state_t *j, const j1939_frame_t *f)
{
    uint32_t pgn = j1939_pgn(f->id);
    uint8_t sa = src_addr(f->id);

    if (pgn == PGN_CLAIM) {
        if (sa == j->self_addr && f->dlc >= 8) {
            /* Contention: lower NAME keeps the address. */
            if (name_of(f) < j->name) {
                j->addr_conflict = true;
            } else {
                j->claim_due = true;  /* defend the address */
            }
        }
        return;
    }
    if (pgn == PGN_RQST && f->dlc >= 3) {
        uint32_t req = (uint32_t)f->data[0] | ((uint32_t)f->data[1] << 8) |
                       ((uint32_t)f->data[2] << 16);
        if (req == PGN_CLAIM && !j->addr_conflict) {
            j->claim_due = true;
        }
        return;
    }

    if (sa != j->engine_addr) {
        return;  /* only the engine ECU's broadcasts matter */
    }

    switch (pgn) {
    case PGN_EEC1: {   /* SPN 190: bytes 4-5 little endian, 0.125 rpm/bit */
        if (f->dlc < 5) break;
        uint16_t raw = (uint16_t)f->data[3] | ((uint16_t)f->data[4] << 8);
        if (raw <= 0xFAFFU) {          /* FBxx..FFxx = error / not available */
            j->rpm = 0.125f * (float)raw;
            age_reset(&j->rpm_age);
        }
        break;
    }
    case PGN_EFLP1: {  /* SPN 100: byte 4, 4 kPa/bit */
        if (f->dlc < 4) break;
        uint8_t raw = f->data[3];
        if (raw <= 0xFAU) {
            j->oil_bar = 0.04f * (float)raw;
            age_reset(&j->oil_age);
        }
        break;
    }
    case PGN_ET1: {    /* SPN 110: byte 1, 1 C/bit, -40 offset */
        if (f->dlc < 1) break;
        uint8_t raw = f->data[0];
        if (raw <= 0xFAU) {
            j->coolant_c = (float)raw - 40.0f;
            age_reset(&j->coolant_age);
        }
        break;
    }
    case PGN_DD: {     /* SPN 96: byte 2, 0.4 %/bit */
        if (f->dlc < 2) break;
        uint8_t raw = f->data[1];
        if (raw <= 0xFAU) {
            j->fuel_pct = 0.4f * (float)raw;
            age_reset(&j->fuel_age);
        }
        break;
    }
    case PGN_DM1: {    /* byte 1 lamps: bits 7-6 MIL, 5-4 red, 3-2 amber */
        if (f->dlc < 2) break;
        j->red_lamp = ((f->data[0] >> 4) & 0x3U) == 0x1U;
        j->amber_lamp = ((f->data[0] >> 2) & 0x3U) == 0x1U;
        j->dtc_spn = 0;
        j->dtc_fmi = 0;
        if (f->dlc >= 6) {
            uint32_t spn = (uint32_t)f->data[2] |
                           ((uint32_t)f->data[3] << 8) |
                           (((uint32_t)f->data[4] >> 5) << 16);
            if (spn != 0 && spn != 0x7FFFFU) {
                j->dtc_spn = spn;
                j->dtc_fmi = f->data[4] & 0x1FU;
            }
        }
        age_reset(&j->dm1_age);
        break;
    }
    default:
        break;
    }
}

bool j1939_tick(j1939_state_t *j, j1939_frame_t *tx)
{
    uint32_t *ages[] = {&j->rpm_age, &j->oil_age, &j->coolant_age,
                        &j->fuel_age, &j->dm1_age};
    for (unsigned i = 0; i < sizeof(ages) / sizeof(ages[0]); i++) {
        if (*ages[i] < UINT32_MAX) {
            (*ages[i])++;
        }
    }

    if (j->claim_due && !j->addr_conflict) {
        j->claim_due = false;
        memset(tx, 0, sizeof(*tx));
        tx->id = 0x18EEFF00U | j->self_addr;   /* prio 6, claim, DA=global */
        tx->dlc = 8;
        for (int i = 0; i < 8; i++) {
            tx->data[i] = (uint8_t)(j->name >> (8 * i));
        }
        return true;
    }
    return false;
}

void j1939_fill_inputs(const j1939_state_t *j, bool run_enabled,
                       gcu_inputs_t *in)
{
    in->rpm = j->rpm;
    in->rpm_valid = j->rpm_age <= J1939_RPM_TIMEOUT_TICKS;
    in->oil_pressure_bar = j->oil_bar;
    in->oil_pressure_valid = j->oil_age <= J1939_OIL_TIMEOUT_TICKS;
    in->coolant_temp_c = j->coolant_c;
    in->coolant_temp_valid = j->coolant_age <= J1939_COOLANT_TIMEOUT_TICKS;
    if (j->fuel_age <= J1939_FUEL_TIMEOUT_TICKS) {
        in->fuel_level_pct = j->fuel_pct;
    }
    /* Lamps hold only while DM1 is fresh — stale severity is no severity,
     * the comms-lost warning covers the silence instead. */
    bool dm1_fresh = j->dm1_age <= J1939_DM1_TIMEOUT_TICKS;
    in->ecu_red_lamp = dm1_fresh && j->red_lamp;
    in->ecu_amber_lamp = dm1_fresh && j->amber_lamp;
    in->ecu_comms_lost =
        run_enabled && j->rpm_age > J1939_RPM_TIMEOUT_TICKS;
}
