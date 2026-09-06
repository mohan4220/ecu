/*
 * ecu_main.h — the runtime: the glue that turns raw hardware readings into
 * gcu_inputs_t, runs the 10 ms control tick, and drives the outputs.
 *
 * The platform is a table of function pointers rather than link-time
 * symbols, for one reason: the whole runtime — sensor conversion, debounce,
 * scheduling, protocol servicing — can then be exercised on the PC against a
 * fake platform. Porting to a new MCU means filling in ecu_platform_t and
 * nothing else.
 *
 * Call ecu_poll() as fast as the main loop can go. It drains the sample and
 * protocol queues every time and runs the control tick when the 10 ms
 * deadline arrives.
 */
#ifndef ECU25_ECU_MAIN_H
#define ECU25_ECU_MAIN_H

#include <stddef.h>

#include "ac_sense.h"
#include "gcu_app.h"
#include "j1939.h"
#include "modbus.h"
#include "sensors.h"

/* Slow ADC channels, read once per tick. */
enum {
    PLAT_DC_OIL = 0,
    PLAT_DC_FUEL,
    PLAT_DC_TEMP,
    PLAT_DC_VBAT,
    PLAT_DC_DPLUS,
    PLAT_DC_COUNT
};

typedef struct {
    /* Free-running millisecond counter. */
    uint32_t (*millis)(void);

    /* One synchronised AC sample set (AC_CH_COUNT counts). Returns false
     * when the DMA queue is empty. */
    bool (*ac_sample)(uint16_t *out);

    /* Latest conversion of one slow channel, in ADC counts. */
    uint16_t (*dc_channel)(int idx);

    /* Eight digital inputs, bit 0 = DIN1. Bit set = input at 12 V. */
    uint8_t (*din_raw)(void);

    /* Tooth period from the magnetic pickup, microseconds. 0 = stopped. */
    uint32_t (*rpm_period_us)(void);

    /* HMI keys. Any may be NULL if there is no panel yet. */
    bool (*keys)(bool *start, bool *stop, bool *mode_auto);

    void (*relays)(const gcu_outputs_t *out);
    void (*backlight)(uint8_t pct);

    bool (*can_rx)(j1939_frame_t *f);
    void (*can_tx)(const j1939_frame_t *f);

    /* One complete Modbus RTU frame, or 0 if none is assembled yet. */
    size_t (*rs485_rx)(uint8_t *buf, size_t max);
    void (*rs485_tx)(const uint8_t *buf, size_t n);

    /* Optional non-volatile run hours. Either may be NULL. */
    uint32_t (*nvm_load_hours)(void);
    void (*nvm_save_hours)(uint32_t hours);
} ecu_platform_t;

typedef struct {
    float adc_lsb_v;        /* volts per ADC count                     */
    float oil_excite_a;     /* sender current source, oil channel      */
    float fuel_excite_a;
    float temp_excite_a;
    float divider_ratio;    /* battery and D+ sense                    */
    uint16_t flywheel_teeth;
    uint8_t modbus_address;
    uint8_t din_debounce;   /* samples that must agree (ticks)         */
    sensor_curve_t oil;
    sensor_curve_t fuel;
    sensor_curve_t temp;
    ac_cal_t ac;
} ecu_rt_cfg_t;

/* Sensible defaults for the ECU-25 board with 50:5 CTs and a 118-tooth ring
 * gear. Override fields afterwards for a different set. */
void ecu_rt_defaults(ecu_rt_cfg_t *cfg);

typedef struct {
    const ecu_platform_t *plat;
    ecu_rt_cfg_t cfg;

    gcu_app_t app;
    gcu_inputs_t in;
    gcu_outputs_t out;

    ac_sense_t ac;
    j1939_state_t j1939;
    modbus_t modbus;
    din_debounce_t din;

    uint16_t iregs[MODBUS_IREG_COUNT];

    uint32_t last_tick_ms;
    uint32_t ticks;         /* control ticks since boot            */
    uint32_t run_ticks;     /* ticks with the engine running       */
    uint32_t run_hours;
    bool started;
    bool din_seeded;
} ecu_t;

void ecu_init(ecu_t *e, const ecu_platform_t *plat, const ecu_rt_cfg_t *cfg);

/* Service everything pending; run the control tick when it is due. Returns
 * true if a control tick ran on this call. */
bool ecu_poll(ecu_t *e);

#endif /* ECU25_ECU_MAIN_H */
