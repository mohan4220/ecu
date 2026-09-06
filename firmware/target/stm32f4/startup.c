/*
 * startup.c — vector table and reset entry for the STM32F407VGT6.
 *
 * Written in C rather than assembly so it is readable and so the vector
 * table is type-checked against the handlers that actually exist. Only the
 * vectors this firmware uses are named; everything else falls through to a
 * trap that halts, which is what you want on a genset controller — a silent
 * return from an unexpected interrupt is how a fault becomes a mystery.
 */
#include <stdint.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;

int main(void);
void SystemInit(void);

void Reset_Handler(void);
void SysTick_Handler(void);
void TIM4_IRQHandler(void);
void USART3_IRQHandler(void);
void CAN1_RX0_IRQHandler(void);

static void Default_Handler(void);
static void Fault_Handler(void);

void Reset_Handler(void)
{
    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata) {
        *dst++ = *src++;
    }
    for (dst = &_sbss; dst < &_ebss; dst++) {
        *dst = 0;
    }
    SystemInit();
    (void)main();
    for (;;) {
        /* main() must not return. */
    }
}

static void Default_Handler(void)
{
    for (;;) {
    }
}

static void Fault_Handler(void)
{
    /* Deliberately a hard stop: the independent watchdog resets the board,
     * which drops every relay including run-enable. Limping on after a bus
     * fault with the starter energised is the failure mode to avoid. */
    for (;;) {
    }
}

typedef void (*vector_t)(void);

__attribute__((section(".isr_vector"), used))
const vector_t g_vectors[] = {
    (vector_t)&_estack,
    Reset_Handler,
    Fault_Handler,      /* NMI          */
    Fault_Handler,      /* HardFault    */
    Fault_Handler,      /* MemManage    */
    Fault_Handler,      /* BusFault     */
    Fault_Handler,      /* UsageFault   */
    0, 0, 0, 0,
    Default_Handler,    /* SVCall       */
    Default_Handler,    /* DebugMonitor */
    0,
    Default_Handler,    /* PendSV       */
    SysTick_Handler,

    /* External interrupts 0..: only the three we enable are named. */
    [16 + 20] = CAN1_RX0_IRQHandler,   /* IRQ 20 */
    [16 + 30] = TIM4_IRQHandler,       /* IRQ 30 */
    [16 + 39] = USART3_IRQHandler,     /* IRQ 39 */
    [16 + 81] = Default_Handler,       /* pad the table to full length */
};
