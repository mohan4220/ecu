# ECU-25 target firmware — STM32F407VGT6

Everything that makes a decision lives in `firmware/core/` and is tested on
the PC. This directory is the part that cannot be: the register writes that
connect that logic to the board.

## Build

```
cd firmware/target/stm32f4
make                                  # arm-none-eabi-gcc on PATH
make ARM_PREFIX=/path/to/arm-none-eabi-   # or point at a toolchain
make flash                            # via OpenOCD + ST-Link
```

Output is `build/ecu25.elf` / `.bin` / `.hex`. Current size is about 11 KB of
flash and 6 KB of RAM against 1 MB and 128 KB, so there is room for the
display stack and an event log.

## Status

**The core logic is tested. The driver layer has never run on silicon.**

`plat_stm32.c` compiles clean and the register sequences follow RM0090, but
no peripheral in it has been scoped. Bring it up in this order, because each
step depends on the one before:

1. **Clock and heartbeat.** PA8 should blink at 1 Hz. If it does not, the PLL
   or the linker script is wrong and nothing else is worth checking.
2. **ADC + DMA.** Break in `plat_ac_sample()` and confirm sample sets arrive
   at 3200 Hz and that the nine AC channels sit near mid-scale (2048) with no
   input. A stuck ring here is the most likely first bug.
3. **Digital inputs**, then **relays** — with the field side disconnected.
4. **MPU capture** with a signal generator before an engine.
5. **RS485** against a Modbus master; watch DE on a scope, since a late
   release truncates the last byte.
6. **CAN** last: it needs a live engine ECU or a second node to ack a frame,
   and an unacked transmitter retries forever.

## Layout

| File | What it is |
|---|---|
| `stm32f4/startup.c` | vector table and reset entry, in C so it is type-checked |
| `stm32f4/system.c` | 168 MHz clock tree from the 8 MHz crystal, FPU enable |
| `stm32f4/plat_stm32.c` | the `ecu_platform_t` implementation — ADC/DMA, GPIO, timers, USART, CAN, watchdog |
| `stm32f4/main.c` | entry point: init, then `ecu_poll()` forever |
| `stm32f4/stm32f407.ld` | linker script |
| `vendor/cmsis/` | ST and ARM headers, unmodified |

## Porting elsewhere

`ecu_platform_t` in `core/ecu_main.h` is the entire contract: twelve function
pointers. Implement those and the controller runs. `firmware/tests/test_all.c`
contains a fake platform that does exactly that on the PC, which is the
reference for what each function is expected to return.
