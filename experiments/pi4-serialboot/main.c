/*
 * Serial chainloader for Raspberry Pi 4B (BCM2711), AArch32.
 *
 * Lives on the SD card as kernel7l.img. Waits for an image over PL011,
 * writes it to 0x8000, checks it, and jumps to it with the firmware's
 * original r0/r1/r2 -- so a new build goes Pi-ward over the serial
 * cable instead of by moving the SD card. Host side:
 * scripts/pi4_serial_boot.py.
 *
 * Protocol (all integers little-endian):
 *   Pi   -> "SBOOT?\n"                 once a second while idle
 *   host -> "LKBT" <size:u32> <crc32:u32>
 *   Pi   -> "OK\n" | "ER <reason>\n"   (informational lines may precede)
 *   host -> <size> payload bytes
 *   Pi   -> "CRC OK, jumping to 0x00008000\n" and jumps,
 *           or "ER ..." and goes back to waiting
 * crc32 is the standard IEEE CRC-32 (zlib.crc32 / binascii.crc32).
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

#define FR_BUSY (1u << 3)
#define FR_RXFE (1u << 4)
#define FR_TXFF (1u << 5)

#define UART_CLOCK_HZ 48000000u

#define PAYLOAD_ADDR 0x00008000u
// Where an overlapping DTB is moved to: just above this loader's own
// 1MB slot at 0x02000000, well clear of any payload.
#define DTB_SAFE_ADDR 0x02100000u
#define DTB_SAFE_MAX  0x00100000u
#define FDT_MAGIC     0xD00DFEEDu

extern uint8_t __image_start[];
extern void jump_to_payload(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t entry)
    __attribute__((noreturn));

// Same pin/clock bring-up as experiments/pi4-baremetal (see that main.c
// for why GPIO14/15 have to be muxed to ALT0 here), but with the UART
// clock at 48MHz rather than 3MHz so higher baud rates stay reachable
// later without touching the clock again.
static void mbox_set_uart_clock(void) {
    static volatile uint32_t __attribute__((aligned(16))) mbox[9];
    mbox[0] = 9 * 4;
    mbox[1] = 0;
    mbox[2] = 0x00038002;   // tag: set clock rate
    mbox[3] = 12;
    mbox[4] = 8;
    mbox[5] = 2;            // clock id: UART
    mbox[6] = UART_CLOCK_HZ;
    mbox[7] = 0;
    mbox[8] = 0;

    uint32_t addr = (uint32_t)(uintptr_t)&mbox[0];

    while (MBOX_STATUS & MBOX_FULL) {}
    MBOX_WRITE = (addr & ~0xFu) | MBOX_CH_PROP;

    for (;;) {
        while (MBOX_STATUS & MBOX_EMPTY) {}
        if ((MBOX_READ & 0xFu) == MBOX_CH_PROP) {
            break;
        }
    }
}

static void gpio_uart0_pins(void) {
    uint32_t sel = GPFSEL1;
    sel &= ~((7u << 12) | (7u << 15));
    sel |= (4u << 12) | (4u << 15);     // GPIO14/15 -> ALT0
    GPFSEL1 = sel;

    uint32_t pull = GPIO_PUP_PDN_CNTRL_REG0;
    pull &= ~((3u << 28) | (3u << 30)); // no pulls on GPIO14/15
    GPIO_PUP_PDN_CNTRL_REG0 = pull;
}

static void uart_init(void) {
    UART0_CR = 0;
    UART0_ICR = 0x7FF;

    gpio_uart0_pins();
    mbox_set_uart_clock();

    // 48000000 / (16 * 115200) = 26.0417 -> IBRD=26, FBRD=round(0.0417*64)=3
    UART0_IBRD = 26;
    UART0_FBRD = 3;

    UART0_LCRH = (1 << 4) | (3 << 5);   // FIFO enable, 8N1
    UART0_IMSC = 0;
    UART0_CR = (1 << 0) | (1 << 8) | (1 << 9);
}

static void uart_putc(char c) {
    while (UART0_FR & FR_TXFF) {}
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

static void uart_puthex(uint32_t v) {
    static const char digits[] = "0123456789abcdef";
    uart_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        uart_putc(digits[(v >> shift) & 0xF]);
    }
}

static void uart_flush(void) {
    while (UART0_FR & FR_BUSY) {}
}

// ARM generic timer. The Pi 4 firmware starts the system counter and
// programs CNTFRQ (54MHz); fall back to that if CNTFRQ reads as zero.
static inline uint64_t timer_now(void) {
    uint32_t lo, hi;
    __asm__ volatile("isb\n\tmrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint32_t timer_freq(void) {
    uint32_t f;
    __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(f));
    return f ? f : 54000000u;
}

// Returns the next byte, or -1 if nothing arrives within `timeout` ticks.
static int uart_getc_timeout(uint64_t timeout) {
    uint64_t start = timer_now();
    while (UART0_FR & FR_RXFE) {
        if (timer_now() - start > timeout) {
            return -1;
        }
    }
    return (int)(UART0_DR & 0xFF);
}

static uint32_t crc32_update(uint32_t crc, uint8_t byte) {
    crc ^= byte;
    for (int i = 0; i < 8; i++) {
        crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return crc;
}

static uint32_t be32(const volatile uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static const char *mode_name(void) {
    uint32_t cpsr;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    switch (cpsr & 0x1F) {
    case 0x1A: return "HYP";
    case 0x13: return "SVC";
    case 0x16: return "MON";
    default:   return "other";
    }
}

// Blocks until a full "LKBT" header arrives, printing the ready prompt
// once a second meanwhile. Returns 0 on success, -1 on a timeout
// mid-header (caller prints the error and starts over).
static int wait_for_header(uint64_t one_sec, uint32_t *size, uint32_t *crc) {
    static const char magic[4] = { 'L', 'K', 'B', 'T' };
    int matched = 0;
    uint64_t last_prompt = timer_now() - one_sec;

    while (matched < 4) {
        if (!(UART0_FR & FR_RXFE)) {
            char c = (char)(UART0_DR & 0xFF);
            if (c == magic[matched]) {
                matched++;
            } else {
                matched = (c == magic[0]) ? 1 : 0;
            }
        } else if (matched == 0 && timer_now() - last_prompt >= one_sec) {
            uart_puts("SBOOT?\n");
            last_prompt = timer_now();
        }
    }

    uint32_t fields[2] = { 0, 0 };
    for (int f = 0; f < 2; f++) {
        for (int i = 0; i < 4; i++) {
            int c = uart_getc_timeout(one_sec);
            if (c < 0) {
                return -1;
            }
            fields[f] |= (uint32_t)c << (8 * i);
        }
    }
    *size = fields[0];
    *crc = fields[1];
    return 0;
}

void boot_main(uint32_t r0, uint32_t r1, uint32_t r2) {
    uart_init();

    const uint32_t payload_max = (uint32_t)(uintptr_t)__image_start - PAYLOAD_ADDR;
    const uint64_t one_sec = timer_freq();

    uart_puts("\n\nlk-perf serial boot (pi4, AArch32) mode=");
    uart_puts(mode_name());
    uart_puts(" r0=");
    uart_puthex(r0);
    uart_puts(" r1=");
    uart_puthex(r1);
    uart_puts(" r2=");
    uart_puthex(r2);
    uart_puts("\n");

    for (;;) {
        uint32_t size, want_crc;
        if (wait_for_header(one_sec, &size, &want_crc) < 0) {
            uart_puts("ER timeout in header\n");
            continue;
        }
        if (size == 0 || size > payload_max) {
            uart_puts("ER size ");
            uart_puthex(size);
            uart_puts(" (max ");
            uart_puthex(payload_max);
            uart_puts(")\n");
            continue;
        }

        // The payload is about to overwrite [PAYLOAD_ADDR, +size). If
        // the firmware's DTB sits in there, move it somewhere safe
        // first and hand the payload the new address instead.
        const volatile uint8_t *dtb = (const volatile uint8_t *)(uintptr_t)r2;
        if (r2 != 0 && be32(dtb) == FDT_MAGIC) {
            uint32_t dtb_size = be32(dtb + 4);
            if (r2 < PAYLOAD_ADDR + size && r2 + dtb_size > PAYLOAD_ADDR) {
                if (dtb_size > DTB_SAFE_MAX) {
                    uart_puts("ER dtb in the way and too big to move\n");
                    continue;
                }
                volatile uint8_t *dst = (volatile uint8_t *)DTB_SAFE_ADDR;
                for (uint32_t i = 0; i < dtb_size; i++) {
                    dst[i] = dtb[i];
                }
                uart_puts("dtb moved ");
                uart_puthex(r2);
                uart_puts(" -> ");
                uart_puthex(DTB_SAFE_ADDR);
                uart_puts("\n");
                r2 = DTB_SAFE_ADDR;
            }
        }

        uart_puts("OK\n");

        volatile uint8_t *dst = (volatile uint8_t *)PAYLOAD_ADDR;
        uint32_t crc = 0xFFFFFFFFu;
        uint32_t got = 0;
        while (got < size) {
            int c = uart_getc_timeout(2 * one_sec);
            if (c < 0) {
                break;
            }
            dst[got++] = (uint8_t)c;
            crc = crc32_update(crc, (uint8_t)c);
        }
        crc = ~crc;

        if (got < size) {
            uart_puts("ER timeout after ");
            uart_puthex(got);
            uart_puts(" bytes\n");
            continue;
        }
        if (crc != want_crc) {
            uart_puts("ER crc got ");
            uart_puthex(crc);
            uart_puts(" want ");
            uart_puthex(want_crc);
            uart_puts("\n");
            continue;
        }

        uart_puts("CRC OK, jumping to ");
        uart_puthex(PAYLOAD_ADDR);
        uart_puts("\n");
        uart_flush();
        jump_to_payload(r0, r1, r2, PAYLOAD_ADDR);
    }
}
