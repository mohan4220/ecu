/*
 * modbus.h — Modbus RTU slave, pure logic.
 *
 * No UART, no timers, no HAL: the caller hands over a received frame and gets
 * a response frame back, exactly like j1939.c takes and returns CAN frames.
 * That keeps the protocol testable on the PC simulator.
 *
 * The transport layer owns RTU framing (the 3.5-character idle gap that
 * delimits frames) and simply calls modbus_rx() once a frame has been
 * assembled. On RS485 it must also drive DE around the response.
 *
 * Register map (16-bit, big-endian on the wire as Modbus requires):
 *
 *   Input registers, FC 04 — live measurements, read-only
 *     0..2   generator L-N volts, phases 1..3        0.1 V
 *     3..5   mains L-N volts, phases 1..3            0.1 V
 *     6..8   generator current, phases 1..3          0.1 A
 *     9      generator frequency                     0.01 Hz
 *     10     engine speed                            1 rpm
 *     11     oil pressure                            0.01 bar
 *     12     coolant temperature (signed)            0.1 degC
 *     13     fuel level                              1 %
 *     14     battery voltage                         0.01 V
 *     15     engine state (gcu_engine_state_t)
 *     16     AMF state (gcu_amf_state_t)
 *     17     alarm bitmap, low word
 *     18     alarm bitmap, high word
 *     19     total real power                        0.1 kW
 *     20     power factor                            0.001
 *            (19 and 20 read 0 until the AC sampling layer exists — see
 *            docs/io-map.md. Zero means "not measured", not "no load".)
 *     21     run hours, low word                     1 h
 *     22     run hours, high word                    1 h
 *
 *   Holding registers, FC 03 / 06 / 16 — control
 *     0      mode: 0 off, 1 manual, 2 auto, 3 test
 *     1      remote start request (0/1)
 *     2      alarm reset — write 1 to pulse, always reads 0
 *     3      lamp test (0/1)
 */
#ifndef ECU25_MODBUS_H
#define ECU25_MODBUS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "gcu_types.h"
#include "gcu_app.h"

#define MODBUS_ADDR_BROADCAST   0
#define MODBUS_MAX_FRAME        256

/* Exception codes we can raise */
#define MODBUS_EX_ILLEGAL_FN    0x01
#define MODBUS_EX_ILLEGAL_ADDR  0x02
#define MODBUS_EX_ILLEGAL_VALUE 0x03
#define MODBUS_EX_SLAVE_FAILURE 0x04

#define MODBUS_IREG_COUNT       23
#define MODBUS_HREG_COUNT       4

typedef struct {
    uint8_t  address;        /* 1..247                                     */
    uint16_t hold[MODBUS_HREG_COUNT];
    /* latched one-shot commands, cleared by the app once consumed */
    bool     cmd_alarm_reset;
    bool     cmd_lamp_test;
    /* diagnostics */
    uint32_t rx_frames;
    uint32_t crc_errors;
    uint32_t exceptions;
} modbus_t;

void     modbus_init(modbus_t *mb, uint8_t address);
uint16_t modbus_crc(const uint8_t *buf, size_t len);

/* Publish the current measurement snapshot into the input-register image. */
void modbus_publish(const gcu_inputs_t *in, const gcu_app_t *app,
                    uint32_t run_hours, uint16_t *iregs);

/*
 * Handle one received RTU frame.
 *   returns the response length written to resp, or 0 when the slave must
 *   stay silent (frame for another address, broadcast, or a CRC error).
 */
size_t modbus_rx(modbus_t *mb, const uint8_t *req, size_t len,
                 const uint16_t *iregs, uint8_t *resp, size_t resp_max);

#endif /* ECU25_MODBUS_H */
