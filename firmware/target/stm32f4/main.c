/*
 * main.c — target entry point.
 *
 * Deliberately thin: bring the peripherals up, hand the runtime the platform
 * table, then loop. Everything that decides anything lives in core/, where
 * it is tested on the PC.
 */
#include <stdbool.h>
#include <stdint.h>

#include "ecu_main.h"

extern const ecu_platform_t PLAT_STM32;
void plat_init(void);
void plat_watchdog_kick(void);
void plat_heartbeat(bool on);

static ecu_t g_ecu;

int main(void)
{
    plat_init();

    ecu_rt_cfg_t cfg;
    ecu_rt_defaults(&cfg);
    /* Defaults already carry ECU25_CT_PRIMARY_A; duplicating the ratio here
     * is how the firmware and the schematic drifted apart in the first place.
     * Check the ring gear tooth count against the engine before trusting
     * rpm — 118 is an assumption. */
    cfg.flywheel_teeth = 118;

    ecu_init(&g_ecu, &PLAT_STM32, &cfg);

    uint32_t beat = 0;
    for (;;) {
        if (ecu_poll(&g_ecu)) {
            /* Kick the watchdog only from a completed control tick. Kicking
             * it in the bare loop would keep a board alive whose control
             * logic had stopped running, which is precisely the failure the
             * watchdog exists to catch. */
            plat_watchdog_kick();
            if (++beat >= 50) {     /* 1 Hz at the 10 ms tick */
                beat = 0;
                static bool on;
                on = !on;
                plat_heartbeat(on);
            }
        }
    }
}
