/*
 * Minimal AArch32 bare-metal validation image for Raspberry Pi 4B.
 *
 * Sole purpose: prove the whole chain works (toolchain -> SD card ->
 * firmware -> AArch32 boot -> UART) before attempting any LK port.
 * Uses PL011 (UART0), not the mini-UART: with enable_uart=1 in
 * config.txt, the firmware already muxes GPIO14/15 to ALT0 (PL011)
 * for us, and PL011's clock is set deterministically via one mailbox
 * call rather than depending on an inferred core_freq side effect
 * (mini-UART's clock is tied to the core clock, and independent
 * sources disagreed on the exact value -- not worth the ambiguity
 * for a first validation image).
 */
#include <stdint.h>

#define PERIPHERAL_BASE 0xFE000000u

#define MBOX_BASE   (PERIPHERAL_BASE + 0xB880u)
#define MBOX_READ   (*(volatile uint32_t *)(MBOX_BASE + 0x00))
#define MBOX_STATUS (*(volatile uint32_t *)(MBOX_BASE + 0x18))
#define MBOX_WRITE  (*(volatile uint32_t *)(MBOX_BASE + 0x20))
#define MBOX_FULL   0x80000000u
#define MBOX_EMPTY  0x40000000u
#define MBOX_CH_PROP 8u

#define GPIO_BASE   (PERIPHERAL_BASE + 0x200000u)
#define GPFSEL1     (*(volatile uint32_t *)(GPIO_BASE + 0x04))
#define GPIO_PUP_PDN_CNTRL_REG0 (*(volatile uint32_t *)(GPIO_BASE + 0xE4))

#define UART0_BASE  (PERIPHERAL_BASE + 0x201000u)
#define UART0_DR    (*(volatile uint32_t *)(UART0_BASE + 0x00))
#define UART0_FR    (*(volatile uint32_t *)(UART0_BASE + 0x18))
#define UART0_IBRD  (*(volatile uint32_t *)(UART0_BASE + 0x24))
#define UART0_FBRD  (*(volatile uint32_t *)(UART0_BASE + 0x28))
#define UART0_LCRH  (*(volatile uint32_t *)(UART0_BASE + 0x2C))
#define UART0_CR    (*(volatile uint32_t *)(UART0_BASE + 0x30))
#define UART0_IMSC  (*(volatile uint32_t *)(UART0_BASE + 0x38))
#define UART0_ICR   (*(volatile uint32_t *)(UART0_BASE + 0x44))

// Property-tag mailbox call to pin the UART clock to a known,
// deterministic 3MHz -- without this the baud-rate math below has no
// reliable reference to compute from (firmware's default UART clock
// rate is otherwise undocumented/variable).
static void mbox_set_uart_clock(void) {
    static volatile uint32_t __attribute__((aligned(16))) mbox[9];
    mbox[0] = 9 * 4;        // total buffer size in bytes
    mbox[1] = 0;            // request code
    mbox[2] = 0x00038002;   // tag: set clock rate
    mbox[3] = 12;           // value buffer size
    mbox[4] = 8;            // request size
    mbox[5] = 2;            // clock id: UART
    mbox[6] = 3000000;      // rate, Hz
    mbox[7] = 0;            // clear turbo
    mbox[8] = 0;            // end tag

    uint32_t addr = (uint32_t)(uintptr_t)&mbox[0];

    while (MBOX_STATUS & MBOX_FULL) {}
    MBOX_WRITE = (addr & ~0xFu) | MBOX_CH_PROP;

    for (;;) {
        while (MBOX_STATUS & MBOX_EMPTY) {}
        uint32_t resp = MBOX_READ;
        if ((resp & 0xFu) == MBOX_CH_PROP) {
            break;
        }
    }
}

// On Pi 4, PL011 is wired to the onboard Bluetooth by default and
// GPIO14/15 carry the mini-UART (ALT5) -- enable_uart=1 alone does not
// change that. Mux GPIO14/15 to ALT0 (PL011 TXD0/RXD0) here so output
// reaches the header regardless of whether dtoverlay=disable-bt took
// effect in firmware.
static void gpio_uart0_pins(void) {
    uint32_t sel = GPFSEL1;
    sel &= ~((7u << 12) | (7u << 15));  // FSEL14, FSEL15
    sel |= (4u << 12) | (4u << 15);     // ALT0
    GPFSEL1 = sel;

    // BCM2711 pull control (replaces the BCM283x GPPUD/GPPUDCLK dance):
    // 2 bits per pin, 00 = no pull. GPIO14 -> [29:28], GPIO15 -> [31:30].
    uint32_t pull = GPIO_PUP_PDN_CNTRL_REG0;
    pull &= ~((3u << 28) | (3u << 30));
    GPIO_PUP_PDN_CNTRL_REG0 = pull;
}

static void uart_init(void) {
    UART0_CR = 0;               // disable UART while configuring
    UART0_ICR = 0x7FF;          // clear pending interrupts

    gpio_uart0_pins();

    mbox_set_uart_clock();      // UART clock = 3MHz, fixed

    // 3000000 / (16 * 115200) = 1.627... -> IBRD=1, FBRD=round(0.627*64)=40
    UART0_IBRD = 1;
    UART0_FBRD = 40;

    UART0_LCRH = (1 << 4) | (3 << 5);   // FIFO enable, 8N1 (WLEN=11)
    UART0_IMSC = 0;                     // polling, no interrupts
    UART0_CR = (1 << 0) | (1 << 8) | (1 << 9); // UART enable, TXE, RXE
}

static void uart_putc(char c) {
    while (UART0_FR & (1 << 5)) {} // wait while TX FIFO full
    UART0_DR = (uint32_t)c;
}

static void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}

static void delay(volatile uint32_t count) {
    while (count--) {
        __asm__ volatile("nop");
    }
}

int main(void) {
    uart_init();
    uart_puts("\n\npi4-baremetal: AArch32 boot OK, PL011 UART live\n");
    uart_puts("core: 0 (others parked)\n");

    uint32_t tick = 0;
    for (;;) {
        uart_puts("heartbeat ");
        // crude decimal print, no libc
        char buf[12];
        int i = 11;
        buf[i] = '\0';
        uint32_t n = tick;
        if (n == 0) {
            buf[--i] = '0';
        } else {
            while (n) {
                buf[--i] = '0' + (n % 10);
                n /= 10;
            }
        }
        uart_puts(&buf[i]);
        uart_puts("\n");
        tick++;
        delay(30000000);
    }
}
