/*
 * plat_stm32.c — ecu_platform_t for the STM32F407VGT6 on the ECU-25 board.
 *
 * Pin assignment comes from hardware/kicad/gen/gen_ecu25.py (PINMAP) and is
 * mirrored in docs/io-map.md. If you change one, change all three.
 *
 *   ADC1 IN10-15  PC0-PC5   gen L1-3, mains L1-3
 *   ADC1 IN0-2    PA0-PA2   CT1-3
 *   ADC1 IN3-7    PA3-PA7   oil, fuel, temp, vbat, D+
 *   TIM2 TRGO               3200 Hz conversion trigger
 *   DMA2 Stream0 Ch0        ADC1 -> ring of sample sets
 *   TIM4_CH1      PB6       magnetic pickup period capture
 *   TIM1_CH2      PE11      backlight PWM
 *   USART3        PD8/PD9   Modbus RTU, DE on PD10
 *   CAN1          PD0/PD1   J1939, 250 kbit/s
 *   GPIOD 2-7               relay drivers K1-K6
 *   GPIOE 0-7               digital inputs DIN1-8
 *   GPIOA 8                 heartbeat LED
 *
 * NOT YET VERIFIED ON SILICON. This compiles and the register sequences
 * follow RM0090, but no part of it has run on a board — treat every
 * peripheral here as unproven until it is scoped.
 */
#include "stm32f4xx.h"

#include "ecu_main.h"

/* ------------------------------------------------------------- constants */

#define AHB_HZ      168000000u
#define APB1_HZ     42000000u   /* timers on APB1 run at 2x = 84 MHz */
#define APB2_HZ     84000000u   /* timers on APB2 run at 2x = 168 MHz */

#define ADC_SEQ_LEN 14u         /* 9 AC + 5 slow, one pass per trigger */
#define ADC_SETS    8u          /* ring depth in sample sets           */

#define MODBUS_BAUD 19200u
/* RTU says 3.5 character times of silence ends a frame. At 19200 8N1 that
 * is 2.0 ms; the millisecond tick can only resolve whole ms, so 3 ms is the
 * first safe value above it. */
#define RTU_GAP_MS  3u

/* ----------------------------------------------------------------- state */

static volatile uint32_t s_millis;

static volatile uint16_t s_adc[ADC_SETS][ADC_SEQ_LEN];
static uint32_t s_adc_tail;             /* next set the runtime will read */
static uint16_t s_slow[PLAT_DC_COUNT];  /* latest slow channels           */

static volatile uint32_t s_mpu_period_us;
static volatile uint32_t s_mpu_last_ms;

static volatile uint8_t s_rx[MODBUS_MAX_FRAME];
static volatile uint16_t s_rx_len;
static volatile uint32_t s_rx_last_ms;

/*
 * The ADC sequence order. Index i of this table is conversion i, and the
 * value is the ADC channel number. The first nine must match the AC_*
 * enum order in ac_sense.h, because ac_sample() copies them straight across.
 */
static const uint8_t ADC_SEQ[ADC_SEQ_LEN] = {
    10, 11, 12,   /* AC_GEN_L1..L3   PC0-PC2 */
    13, 14, 15,   /* AC_MAINS_L1..L3 PC3-PC5 */
    0, 1, 2,      /* AC_I_L1..L3     PA0-PA2 */
    3, 4, 5, 6, 7 /* oil, fuel, temp, vbat, D+ */
};

/* --------------------------------------------------------------- helpers */

static void gpio_mode(GPIO_TypeDef *port, uint32_t pin, uint32_t mode,
                      uint32_t af)
{
    port->MODER &= ~(3u << (pin * 2));
    port->MODER |= (mode << (pin * 2));
    port->OSPEEDR |= (2u << (pin * 2));           /* high speed */
    if (mode == 2u) {                              /* alternate function */
        uint32_t idx = pin >> 3, sh = (pin & 7u) * 4u;
        port->AFR[idx] &= ~(0xFu << sh);
        port->AFR[idx] |= (af << sh);
    }
}

void SysTick_Handler(void)
{
    s_millis++;
}

static uint32_t plat_millis(void)
{
    return s_millis;
}

/* ------------------------------------------------------------------- ADC */

static void adc_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOCEN |
                    RCC_AHB1ENR_DMA2EN;
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;

    for (uint32_t p = 0; p <= 7; p++) {
        gpio_mode(GPIOA, p, 3u, 0);   /* analog */
    }
    for (uint32_t p = 0; p <= 5; p++) {
        gpio_mode(GPIOC, p, 3u, 0);
    }

    /* ADCCLK = APB2 / 4 = 21 MHz (max 36). */
    ADC->CCR = (1u << ADC_CCR_ADCPRE_Pos);

    /* 15-cycle sample time on every channel: with 21 MHz ADCCLK that is
     * 1.29 us per conversion, 18 us for the whole sequence — comfortably
     * inside the 312 us trigger period, and long enough for the ~5 k source
     * impedance of the AC dividers to settle. */
    ADC1->SMPR1 = 0x00249249u;   /* channels 10..18 -> 001 each */
    ADC1->SMPR2 = 0x09249249u;   /* channels 0..9   -> 001 each */

    ADC1->SQR1 = ((ADC_SEQ_LEN - 1u) << ADC_SQR1_L_Pos);
    ADC1->SQR2 = 0;
    ADC1->SQR3 = 0;
    for (uint32_t i = 0; i < ADC_SEQ_LEN; i++) {
        uint32_t ch = ADC_SEQ[i];
        if (i < 6) {
            ADC1->SQR3 |= ch << (i * 5);
        } else if (i < 12) {
            ADC1->SQR2 |= ch << ((i - 6) * 5);
        } else {
            ADC1->SQR1 |= ch << ((i - 12) * 5);
        }
    }

    ADC1->CR1 = ADC_CR1_SCAN;
    /* Trigger on TIM2 TRGO (EXTSEL 0110), rising edge, DMA in circular
     * mode so the ring keeps filling without CPU help. */
    ADC1->CR2 = ADC_CR2_DMA | ADC_CR2_DDS |
                (6u << ADC_CR2_EXTSEL_Pos) | (1u << ADC_CR2_EXTEN_Pos);

    DMA2_Stream0->CR = 0;
    while (DMA2_Stream0->CR & DMA_SxCR_EN) {
    }
    DMA2_Stream0->PAR = (uint32_t)&ADC1->DR;
    DMA2_Stream0->M0AR = (uint32_t)s_adc;
    DMA2_Stream0->NDTR = ADC_SETS * ADC_SEQ_LEN;
    DMA2_Stream0->CR = (0u << DMA_SxCR_CHSEL_Pos) |  /* channel 0 = ADC1 */
                       DMA_SxCR_MINC | DMA_SxCR_CIRC |
                       (1u << DMA_SxCR_PSIZE_Pos) |  /* 16-bit */
                       (1u << DMA_SxCR_MSIZE_Pos) |
                       (2u << DMA_SxCR_PL_Pos);
    DMA2_Stream0->CR |= DMA_SxCR_EN;

    ADC1->CR2 |= ADC_CR2_ADON;

    /* TIM2 update at 3200 Hz -> TRGO. 84 MHz / 26250 = 3200. */
    TIM2->PSC = 0;
    TIM2->ARR = 26249;
    TIM2->CR2 = (2u << TIM_CR2_MMS_Pos);   /* update event as TRGO */
    TIM2->CR1 = TIM_CR1_CEN;
}

/*
 * Hand the runtime one complete sample set, oldest first. The DMA counter
 * says how much of the ring is filled; anything the runtime has not taken
 * yet is still valid because the ring is eight sets deep and the runtime
 * drains it every pass through the main loop.
 */
static bool plat_ac_sample(uint16_t *out)
{
    /* NDTR counts DOWN and reloads to the full length when the circular
     * transfer wraps, so head is an index into the ring, not a monotonic
     * counter — the tail has to be an index too. Keeping the tail monotonic
     * looked fine until the first wrap, after which the two could never be
     * equal again and the runtime would replay the whole ring forever. */
    uint32_t written = (ADC_SETS * ADC_SEQ_LEN) - DMA2_Stream0->NDTR;
    uint32_t head = written / ADC_SEQ_LEN;
    if (head >= ADC_SETS) {
        head = 0;   /* NDTR has just reloaded */
    }
    if (s_adc_tail == head) {
        return false;
    }

    uint32_t idx = s_adc_tail;
    for (uint32_t i = 0; i < AC_CH_COUNT; i++) {
        out[i] = s_adc[idx][i];
    }
    for (uint32_t i = 0; i < PLAT_DC_COUNT; i++) {
        s_slow[i] = s_adc[idx][AC_CH_COUNT + i];
    }
    s_adc_tail = (s_adc_tail + 1u) % ADC_SETS;
    return true;
}

static uint16_t plat_dc_channel(int idx)
{
    if (idx < 0 || idx >= PLAT_DC_COUNT) {
        return 0;
    }
    return s_slow[idx];
}

/* -------------------------------------------------------- digital inputs */

static void din_init_hw(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOEEN;
    for (uint32_t p = 0; p <= 7; p++) {
        gpio_mode(GPIOE, p, 0u, 0);   /* input, no pull: the divider sets it */
    }
}

static uint8_t plat_din_raw(void)
{
    return (uint8_t)(GPIOE->IDR & 0xFFu);
}

/* --------------------------------------------------------------- relays */

static void relay_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;
    for (uint32_t p = 2; p <= 7; p++) {
        gpio_mode(GPIOD, p, 1u, 0);   /* output */
        GPIOD->BSRR = (1u << (p + 16));   /* start with every relay OPEN */
    }
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    gpio_mode(GPIOA, 8, 1u, 0);       /* heartbeat LED */
}

static void plat_relays(const gcu_outputs_t *o)
{
    /* One BSRR write drives all six coils, so the relays change together
     * rather than in a staggered sequence a scope would catch. */
    const bool on[6] = {
        o->run_enable,      /* PD2 K1 FUEL  */
        o->starter,         /* PD3 K2 START */
        o->gen_contactor,   /* PD4 K3 GEN   */
        o->mains_contactor, /* PD5 K4 MAINS */
        o->aux1,            /* PD6 K5 AUX1  */
        o->aux2,            /* PD7 K6 AUX2  */
    };
    uint32_t bsrr = 0;
    for (uint32_t i = 0; i < 6; i++) {
        uint32_t pin = i + 2;
        bsrr |= on[i] ? (1u << pin) : (1u << (pin + 16));
    }
    GPIOD->BSRR = bsrr;
}

/* ------------------------------------------------------------- backlight */

static void backlight_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOEEN;
    RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;
    gpio_mode(GPIOE, 11, 2u, 1u);   /* AF1 = TIM1_CH2 */

    TIM1->PSC = 167;                /* 168 MHz -> 1 MHz  */
    TIM1->ARR = 999;                /* 1 kHz PWM         */
    TIM1->CCR2 = 0;
    TIM1->CCMR1 |= (6u << TIM_CCMR1_OC2M_Pos) | TIM_CCMR1_OC2PE;
    TIM1->CCER |= TIM_CCER_CC2E;
    TIM1->BDTR |= TIM_BDTR_MOE;     /* advanced timer: main output enable */
    TIM1->EGR = TIM_EGR_UG;
    TIM1->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
}

static void plat_backlight(uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }
    TIM1->CCR2 = (uint32_t)pct * 10u;
}

/* ----------------------------------------------------------- MPU capture */

static void mpu_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    RCC->APB1ENR |= RCC_APB1ENR_TIM4EN;
    gpio_mode(GPIOB, 6, 2u, 2u);    /* AF2 = TIM4_CH1 */

    TIM4->PSC = 83;                 /* 84 MHz -> 1 MHz, so ticks are us */
    TIM4->ARR = 0xFFFF;
    TIM4->CCMR1 = (1u << TIM_CCMR1_CC1S_Pos);   /* IC1 on TI1 */
    TIM4->CCER = TIM_CCER_CC1E;                 /* rising edge */
    TIM4->DIER = TIM_DIER_CC1IE | TIM_DIER_UIE;
    TIM4->CR1 = TIM_CR1_CEN;
    NVIC_EnableIRQ(TIM4_IRQn);
}

void TIM4_IRQHandler(void)
{
    static uint16_t last;
    static bool have_last;

    if (TIM4->SR & TIM_SR_CC1IF) {
        uint16_t now = (uint16_t)TIM4->CCR1;
        if (have_last) {
            s_mpu_period_us = (uint16_t)(now - last);
        }
        last = now;
        have_last = true;
        s_mpu_last_ms = s_millis;
        TIM4->SR = (uint16_t)~TIM_SR_CC1IF;
    }
    if (TIM4->SR & TIM_SR_UIF) {
        TIM4->SR = (uint16_t)~TIM_SR_UIF;
    }
}

static uint32_t plat_rpm_period_us(void)
{
    /* No tooth for 200 ms is a stopped engine, not a very slow one: at
     * 118 teeth that is under 3 rpm. Without this the last captured period
     * would sit there forever and the controller would believe the engine
     * was still turning. */
    if ((uint32_t)(s_millis - s_mpu_last_ms) > 200u) {
        return 0;
    }
    return s_mpu_period_us;
}

/* --------------------------------------------------------- RS485 / Modbus */

static void rs485_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART3EN;

    gpio_mode(GPIOD, 8, 2u, 7u);    /* AF7 USART3_TX */
    gpio_mode(GPIOD, 9, 2u, 7u);    /* AF7 USART3_RX */
    gpio_mode(GPIOD, 10, 1u, 0);    /* DE as plain output */
    GPIOD->BSRR = (1u << (10 + 16));

    USART3->BRR = (APB1_HZ + MODBUS_BAUD / 2u) / MODBUS_BAUD;
    USART3->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;
    NVIC_EnableIRQ(USART3_IRQn);
}

void USART3_IRQHandler(void)
{
    if (USART3->SR & USART_SR_RXNE) {
        uint8_t b = (uint8_t)USART3->DR;
        if (s_rx_len < MODBUS_MAX_FRAME) {
            s_rx[s_rx_len++] = b;
        }
        s_rx_last_ms = s_millis;
    }
    if (USART3->SR & (USART_SR_ORE | USART_SR_FE | USART_SR_NE)) {
        (void)USART3->DR;   /* reading DR after SR clears the error flags */
    }
}

/*
 * The RTU frame boundary is silence, so the buffer is only handed over once
 * the line has been idle for a gap. That also makes the copy safe without
 * disabling interrupts: the ISR cannot be appending to a buffer that has
 * had no byte for three milliseconds.
 */
static size_t plat_rs485_rx(uint8_t *buf, size_t max)
{
    if (s_rx_len == 0) {
        return 0;
    }
    if ((uint32_t)(s_millis - s_rx_last_ms) < RTU_GAP_MS) {
        return 0;   /* frame still arriving */
    }
    size_t n = s_rx_len;
    if (n > max) {
        n = max;
    }
    for (size_t i = 0; i < n; i++) {
        buf[i] = s_rx[i];
    }
    s_rx_len = 0;
    return n;
}

static void plat_rs485_tx(const uint8_t *buf, size_t n)
{
    GPIOD->BSRR = (1u << 10);            /* DE high: drive the bus */
    for (size_t i = 0; i < n; i++) {
        while (!(USART3->SR & USART_SR_TXE)) {
        }
        USART3->DR = buf[i];
    }
    /* Wait for the last STOP bit before releasing DE, or the final byte is
     * truncated on the wire — the classic half-duplex bug. */
    while (!(USART3->SR & USART_SR_TC)) {
    }
    GPIOD->BSRR = (1u << (10 + 16));
}

/* ------------------------------------------------------------------- CAN */

static void can_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;
    RCC->APB1ENR |= RCC_APB1ENR_CAN1EN;
    gpio_mode(GPIOD, 0, 2u, 9u);    /* AF9 CAN1_RX */
    gpio_mode(GPIOD, 1, 2u, 9u);    /* AF9 CAN1_TX */

    CAN1->MCR &= ~CAN_MCR_SLEEP;
    CAN1->MCR |= CAN_MCR_INRQ;
    while (!(CAN1->MSR & CAN_MSR_INAK)) {
    }
    /* 250 kbit/s from 42 MHz: 14 tq of 12 prescaler ticks.
     * tq = 12/42MHz = 285.7 ns; 1 + 10 + 3 = 14 tq = 4 us = 250 kbit/s.
     * Sample point at 11/14 = 78.6 %, which is where J1939 wants it. */
    CAN1->BTR = ((12u - 1u) << CAN_BTR_BRP_Pos) |
                ((10u - 1u) << CAN_BTR_TS1_Pos) |
                ((3u - 1u) << CAN_BTR_TS2_Pos) |
                ((1u - 1u) << CAN_BTR_SJW_Pos);
    CAN1->MCR &= ~CAN_MCR_INRQ;
    while (CAN1->MSR & CAN_MSR_INAK) {
    }

    /* One filter, accept everything: J1939 addressing is done in software
     * and a genset bus is quiet enough not to need hardware filtering. */
    CAN1->FMR |= CAN_FMR_FINIT;
    CAN1->FA1R &= ~1u;
    CAN1->FS1R |= 1u;
    CAN1->sFilterRegister[0].FR1 = 0;
    CAN1->sFilterRegister[0].FR2 = 0;
    CAN1->FM1R &= ~1u;
    CAN1->FFA1R &= ~1u;
    CAN1->FA1R |= 1u;
    CAN1->FMR &= ~CAN_FMR_FINIT;
}

static bool plat_can_rx(j1939_frame_t *f)
{
    if ((CAN1->RF0R & CAN_RF0R_FMP0) == 0) {
        return false;
    }
    CAN_FIFOMailBox_TypeDef *m = &CAN1->sFIFOMailBox[0];
    if ((m->RIR & CAN_RI0R_IDE) == 0) {
        CAN1->RF0R |= CAN_RF0R_RFOM0;   /* 11-bit: not J1939, drop it */
        return false;
    }
    f->id = m->RIR >> 3;
    f->dlc = (uint8_t)(m->RDTR & 0xFu);
    if (f->dlc > 8) {
        f->dlc = 8;
    }
    uint32_t lo = m->RDLR, hi = m->RDHR;
    f->data[0] = (uint8_t)lo;         f->data[1] = (uint8_t)(lo >> 8);
    f->data[2] = (uint8_t)(lo >> 16); f->data[3] = (uint8_t)(lo >> 24);
    f->data[4] = (uint8_t)hi;         f->data[5] = (uint8_t)(hi >> 8);
    f->data[6] = (uint8_t)(hi >> 16); f->data[7] = (uint8_t)(hi >> 24);
    CAN1->RF0R |= CAN_RF0R_RFOM0;
    return true;
}

static void plat_can_tx(const j1939_frame_t *f)
{
    int mb = -1;
    for (int i = 0; i < 3; i++) {
        if (CAN1->TSR & (CAN_TSR_TME0 << i)) {
            mb = i;
            break;
        }
    }
    if (mb < 0) {
        return;   /* all mailboxes busy: drop, the claim is re-sent anyway */
    }
    CAN_TxMailBox_TypeDef *m = &CAN1->sTxMailBox[mb];
    m->TDTR = f->dlc;
    m->TDLR = (uint32_t)f->data[0] | ((uint32_t)f->data[1] << 8) |
              ((uint32_t)f->data[2] << 16) | ((uint32_t)f->data[3] << 24);
    m->TDHR = (uint32_t)f->data[4] | ((uint32_t)f->data[5] << 8) |
              ((uint32_t)f->data[6] << 16) | ((uint32_t)f->data[7] << 24);
    m->TIR = (f->id << 3) | CAN_TI0R_IDE | CAN_TI0R_TXRQ;
}

void CAN1_RX0_IRQHandler(void)
{
    /* Polled in plat_can_rx(); the vector exists so an unexpected interrupt
     * does not land in the default trap. */
}

/* ------------------------------------------------------------------ misc */

void plat_heartbeat(bool on)
{
    GPIOA->BSRR = on ? (1u << 8) : (1u << (8 + 16));
}

static void systick_init(void)
{
    SysTick->LOAD = (AHB_HZ / 1000u) - 1u;
    SysTick->VAL = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk |
                    SysTick_CTRL_ENABLE_Msk;
}

static void iwdg_init(void)
{
    /* ~1 s timeout from the 32 kHz LSI: prescaler 32 gives 1 kHz, reload
     * 1000. Kicked once per control tick, so a stalled loop resets the board
     * and every relay drops. */
    RCC->CSR |= RCC_CSR_LSION;
    while (!(RCC->CSR & RCC_CSR_LSIRDY)) {
    }
    IWDG->KR = 0x5555;
    IWDG->PR = 3;        /* /32 */
    IWDG->RLR = 1000;
    IWDG->KR = 0xAAAA;
    IWDG->KR = 0xCCCC;
}

void plat_watchdog_kick(void)
{
    IWDG->KR = 0xAAAA;
}

void plat_init(void)
{
    systick_init();
    adc_init();
    din_init_hw();
    relay_init();
    backlight_init();
    mpu_init();
    rs485_init();
    can_init();
    iwdg_init();
}

const ecu_platform_t PLAT_STM32 = {
    .millis = plat_millis,
    .ac_sample = plat_ac_sample,
    .dc_channel = plat_dc_channel,
    .din_raw = plat_din_raw,
    .rpm_period_us = plat_rpm_period_us,
    .keys = 0,               /* display board is a separate project */
    .relays = plat_relays,
    .backlight = plat_backlight,
    .can_rx = plat_can_rx,
    .can_tx = plat_can_tx,
    .rs485_rx = plat_rs485_rx,
    .rs485_tx = plat_rs485_tx,
    .nvm_load_hours = 0,     /* M95M02 driver still to be written */
    .nvm_save_hours = 0,
};
