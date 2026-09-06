/*
 * system.c — clock tree for the STM32F407VGT6 on an 8 MHz HSE crystal.
 *
 * 8 MHz / 8 = 1 MHz VCO input (ST's recommended value), x336 = 336 MHz VCO,
 * /2 = 168 MHz SYSCLK. AHB /1 = 168, APB1 /4 = 42, APB2 /2 = 84 — the
 * maxima for this part. Flash needs 5 wait states at 168 MHz on a 3.3 V
 * supply; setting them AFTER the switch is a classic way to brick a board.
 */
#include "stm32f4xx.h"

uint32_t SystemCoreClock = 168000000u;

void SystemInit(void)
{
    /* FPU: CP10/CP11 full access. The control logic is full of floats and
     * without this the first one faults. */
    SCB->CPACR |= (0xFu << 20);

    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY)) {
    }
    RCC->CFGR = 0;
    RCC->CR &= ~(RCC_CR_PLLON | RCC_CR_HSEON | RCC_CR_CSSON);
    RCC->PLLCFGR = 0x24003010u;   /* reset value */
    RCC->CIR = 0;

    RCC->CR |= RCC_CR_HSEON;
    for (uint32_t t = 0; !(RCC->CR & RCC_CR_HSERDY); t++) {
        if (t > 0x5000u) {
            return;   /* no crystal: stay on the 16 MHz HSI rather than hang */
        }
    }

    PWR->CR |= PWR_CR_VOS;        /* scale 1, required above 144 MHz */
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;

    RCC->CFGR |= RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV4 |
                 RCC_CFGR_PPRE2_DIV2;

    RCC->PLLCFGR = (8u << RCC_PLLCFGR_PLLM_Pos) |
                   (336u << RCC_PLLCFGR_PLLN_Pos) |
                   (0u << RCC_PLLCFGR_PLLP_Pos) |     /* 00 = /2 */
                   (7u << RCC_PLLCFGR_PLLQ_Pos) |
                   RCC_PLLCFGR_PLLSRC_HSE;

    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {
    }

    FLASH->ACR = FLASH_ACR_ICEN | FLASH_ACR_DCEN | FLASH_ACR_PRFTEN |
                 FLASH_ACR_LATENCY_5WS;
    while ((FLASH->ACR & FLASH_ACR_LATENCY) != FLASH_ACR_LATENCY_5WS) {
    }

    RCC->CFGR &= ~RCC_CFGR_SW;
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
    }
}
