/*
 * modbus.c — Modbus RTU slave, pure logic. See modbus.h for the register map.
 */
#include "modbus.h"

#include <string.h>

/* ------------------------------------------------------------------- CRC */

/*
 * Standard Modbus CRC-16 (poly 0xA001, init 0xFFFF), computed bitwise. A
 * table would be faster but this runs at 9600-115200 baud on a 168 MHz part:
 * the cost is irrelevant next to 256 bytes of flash.
 */
uint16_t modbus_crc(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u)
                             : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

void modbus_init(modbus_t *mb, uint8_t address)
{
    memset(mb, 0, sizeof(*mb));
    /* 0 is broadcast-only and 248+ is reserved: either would make the
     * slave silently answer nothing. */
    mb->address = (address >= 1 && address <= 247) ? address : 1;
    mb->hold[0] = 2; /* default mode: AUTO */
}

/* --------------------------------------------------------------- publish */

static uint16_t sat_u16(float v)
{
    if (v <= 0.0f) return 0;
    if (v >= 65535.0f) return 65535;
    return (uint16_t)(v + 0.5f);
}

static uint16_t sat_s16(float v)
{
    if (v <= -32768.0f) return (uint16_t)(int16_t)-32768;
    if (v >= 32767.0f) return (uint16_t)(int16_t)32767;
    return (uint16_t)(int16_t)(v >= 0 ? v + 0.5f : v - 0.5f);
}

void modbus_publish(const gcu_inputs_t *in, const gcu_app_t *app,
                    uint32_t run_hours, uint16_t *iregs)
{
    for (int i = 0; i < 3; i++) {
        iregs[0 + i] = sat_u16(in->gen_v[i] * 10.0f);
        iregs[3 + i] = sat_u16(in->mains_v[i] * 10.0f);
        iregs[6 + i] = sat_u16(in->load_a[i] * 10.0f);
    }
    iregs[9]  = sat_u16(in->gen_hz * 100.0f);
    iregs[10] = in->rpm_valid ? sat_u16(in->rpm) : 0;
    iregs[11] = in->oil_pressure_valid ? sat_u16(in->oil_pressure_bar * 100.0f) : 0;
    iregs[12] = in->coolant_temp_valid ? sat_s16(in->coolant_temp_c * 10.0f) : 0;
    iregs[13] = in->fuel_level_valid ? sat_u16(in->fuel_level_pct) : 0;
    iregs[14] = sat_u16(in->battery_v * 100.0f);
    iregs[15] = (uint16_t)app->engine.state;
    iregs[16] = (uint16_t)app->amf.state;

    /* Alarm bitmap: bit n = alarm_id_t n. ALARM_COUNT is comfortably under
     * 32 today; the split into two words leaves room to grow. */
    uint32_t mask = 0;
    for (int i = 0; i < ALARM_COUNT && i < 32; i++) {
        if (app->prot.active[i]) {
            mask |= (1u << i);
        }
    }
    iregs[17] = (uint16_t)(mask & 0xFFFFu);
    iregs[18] = (uint16_t)(mask >> 16);

    /* Real power and PF come from ac_sense.c, which computes mean(v*i) on
     * the phase-matched channels. Publishing an assumed PF here would put a
     * constant 0.800 into a SCADA trend and overstate kW at the light loads
     * a genset actually runs at. */
    /* SIGNED: sat_u16 clamped reverse power to 0, which reads as "no load"
     * — the one condition register 19 exists to make visible. */
    iregs[19] = sat_s16(in->real_power_w / 100.0f); /* 0.1 kW steps, signed */
    iregs[20] = sat_u16(in->power_factor * 1000.0f); /* 0.001 steps  */
    iregs[21] = (uint16_t)(run_hours & 0xFFFFu);
    iregs[22] = (uint16_t)(run_hours >> 16);
}

/* ---------------------------------------------------------------- decode */

static size_t exception(modbus_t *mb, uint8_t fn, uint8_t code,
                        uint8_t *resp)
{
    mb->exceptions++;
    resp[0] = mb->address;
    resp[1] = (uint8_t)(fn | 0x80u);
    resp[2] = code;
    uint16_t crc = modbus_crc(resp, 3);
    resp[3] = (uint8_t)(crc & 0xFFu);
    resp[4] = (uint8_t)(crc >> 8);
    return 5;
}

static size_t finish(modbus_t *mb, uint8_t *resp, size_t n)
{
    (void)mb;
    uint16_t crc = modbus_crc(resp, n);
    resp[n]     = (uint8_t)(crc & 0xFFu);
    resp[n + 1] = (uint8_t)(crc >> 8);
    return n + 2;
}

static size_t read_regs(modbus_t *mb, uint8_t fn, const uint8_t *req,
                        const uint16_t *iregs, uint8_t *resp, size_t resp_max)
{
    uint16_t start = (uint16_t)((req[2] << 8) | req[3]);
    uint16_t count = (uint16_t)((req[4] << 8) | req[5]);
    const uint16_t limit = (fn == 0x04) ? MODBUS_IREG_COUNT : MODBUS_HREG_COUNT;
    const uint16_t *src  = (fn == 0x04) ? iregs : mb->hold;

    if (count == 0 || count > 125) {
        return exception(mb, fn, MODBUS_EX_ILLEGAL_VALUE, resp);
    }
    /* Checked as a sum in 32-bit to keep a huge start+count from wrapping. */
    if ((uint32_t)start + count > limit) {
        return exception(mb, fn, MODBUS_EX_ILLEGAL_ADDR, resp);
    }
    /* The request is legal; we simply cannot fit the answer. That is a
     * slave-side resource failure (0x04), not the master's fault — telling
     * it ILLEGAL DATA VALUE would send it hunting for a bad register count.
     * The transport should always hand us a 256-byte buffer. */
    if (3u + 2u * count + 2u > resp_max) {
        return exception(mb, fn, MODBUS_EX_SLAVE_FAILURE, resp);
    }

    resp[0] = mb->address;
    resp[1] = fn;
    resp[2] = (uint8_t)(2 * count);
    for (uint16_t i = 0; i < count; i++) {
        resp[3 + 2 * i]     = (uint8_t)(src[start + i] >> 8);
        resp[3 + 2 * i + 1] = (uint8_t)(src[start + i] & 0xFFu);
    }
    return finish(mb, resp, (size_t)(3 + 2 * count));
}

/* Apply one holding-register write. Returns false on an illegal value. */
static bool apply_hold(modbus_t *mb, uint16_t addr, uint16_t val)
{
    switch (addr) {
    case 0: /* mode */
        if (val > 3) return false;
        mb->hold[0] = val;
        mb->mode_from_remote = true;
        return true;
    case 1: /* remote start */
        if (val > 1) return false;
        mb->hold[1] = val;
        return true;
    case 2: /* alarm reset: edge command, register always reads back 0 */
        if (val > 1) return false;
        if (val) mb->cmd_alarm_reset = true;
        mb->hold[2] = 0;
        return true;
    case 3: /* lamp test */
        if (val > 1) return false;
        mb->hold[3] = val;
        mb->cmd_lamp_test = (val != 0);
        return true;
    default:
        return false;
    }
}

size_t modbus_rx(modbus_t *mb, const uint8_t *req, size_t len,
                 const uint16_t *iregs, uint8_t *resp, size_t resp_max)
{
    /* Shortest legal RTU frame is addr + fn + 2 CRC. */
    /* 8, not 5: the FC06 and FC16 echo paths write resp[0..7]. Guarding
     * only the 5-byte exception frame let them run three bytes past the
     * caller's buffer. */
    if (len < 4 || len > MODBUS_MAX_FRAME || resp_max < 8) {
        return 0;
    }
    uint16_t rx_crc = (uint16_t)(req[len - 2] | (req[len - 1] << 8));
    if (modbus_crc(req, len - 2) != rx_crc) {
        mb->crc_errors++;
        return 0; /* corrupt: stay silent, the master will time out */
    }

    const uint8_t addr = req[0];
    const uint8_t fn   = req[1];
    if (addr != mb->address && addr != MODBUS_ADDR_BROADCAST) {
        return 0; /* not for us */
    }
    mb->rx_frames++;
    /* A broadcast is executed but never answered. */
    const bool silent = (addr == MODBUS_ADDR_BROADCAST);

    switch (fn) {
    case 0x03: /* read holding  */
    case 0x04: /* read input    */
        if (len != 8 || silent) return 0;
        return read_regs(mb, fn, req, iregs, resp, resp_max);

    case 0x06: { /* write single */
        if (len != 8) return 0;
        uint16_t a = (uint16_t)((req[2] << 8) | req[3]);
        uint16_t v = (uint16_t)((req[4] << 8) | req[5]);
        if (a >= MODBUS_HREG_COUNT) {
            return silent ? 0 : exception(mb, fn, MODBUS_EX_ILLEGAL_ADDR, resp);
        }
        if (!apply_hold(mb, a, v)) {
            return silent ? 0 : exception(mb, fn, MODBUS_EX_ILLEGAL_VALUE, resp);
        }
        if (silent) return 0;
        /* Echo the request back, as the spec requires. */
        memcpy(resp, req, 6);
        resp[0] = mb->address;
        return finish(mb, resp, 6);
    }

    case 0x10: { /* write multiple */
        if (len < 9) return 0;
        uint16_t a = (uint16_t)((req[2] << 8) | req[3]);
        uint16_t n = (uint16_t)((req[4] << 8) | req[5]);
        uint8_t bytes = req[6];
        if (n == 0 || n > 123 || bytes != 2 * n || len != 9u + bytes) {
            return silent ? 0 : exception(mb, fn, MODBUS_EX_ILLEGAL_VALUE, resp);
        }
        if ((uint32_t)a + n > MODBUS_HREG_COUNT) {
            return silent ? 0 : exception(mb, fn, MODBUS_EX_ILLEGAL_ADDR, resp);
        }
        /* Validate every value BEFORE applying any, so a bad word in the
         * middle cannot leave the slave half-written. */
        for (uint16_t i = 0; i < n; i++) {
            uint16_t v = (uint16_t)((req[7 + 2 * i] << 8) | req[7 + 2 * i + 1]);
            modbus_t probe = *mb;
            if (!apply_hold(&probe, (uint16_t)(a + i), v)) {
                return silent ? 0
                              : exception(mb, fn, MODBUS_EX_ILLEGAL_VALUE, resp);
            }
        }
        for (uint16_t i = 0; i < n; i++) {
            uint16_t v = (uint16_t)((req[7 + 2 * i] << 8) | req[7 + 2 * i + 1]);
            (void)apply_hold(mb, (uint16_t)(a + i), v);
        }
        if (silent) return 0;
        resp[0] = mb->address;
        resp[1] = fn;
        resp[2] = req[2]; resp[3] = req[3];
        resp[4] = req[4]; resp[5] = req[5];
        return finish(mb, resp, 6);
    }

    default:
        return silent ? 0 : exception(mb, fn, MODBUS_EX_ILLEGAL_FN, resp);
    }
}
