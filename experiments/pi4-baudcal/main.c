/*
 * UART baud-rate calibration test for Raspberry Pi 4B (BCM2711, AArch32).
 *
 * Loaded as a one-shot payload via the existing pi4-serialboot
 * chainloader (still at 115200, unchanged -- that link stays the safe,
 * always-works fallback). Once running, this lets the host try a
 * series of candidate higher baud rates *without* reflashing the SD
 * card and *without* a power-cycle between attempts:
 *
 *   host -> 'S' <rate_index:u8>          (sent at the CURRENT baud)
 *   [both sides switch UART divisor to that rate]
 *   host -> "PING" (4 bytes, at the NEW baud)
 *   Pi   -> "PONG\n" if seen within ~1s, else silently reverts its own
 *           UART back to 115200 and prints "REVERTED\n" *there* -- so
 *           the host can fall back to 115200 and try the next
 *           candidate without touching the physical board at all.
 *
 *   host -> 'X' <size:u32> <size bytes> <crc32:u32>   (bulk stress test,
 *           at whichever baud is currently synced)
 *   Pi   -> "RXCRC OK\n" | "RXCRC FAIL\n", then echoes the same `size`
 *           bytes back verbatim -- the host does its own byte-exact
 *           comparison against what it sent, which is the real test.
 *
 * Candidate table computed for the 47MHz-nominal/48MHz-actual UART
 * clock the chainloader already programs via the mailbox (same value
 * pi4-serialboot's own uart_init() sets, inherited here since we're
 * jumped to directly, not power-cycled -- re-set anyway for safety in
 * case this is ever loaded some other way).
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

// index 0 (115200) must stay first and must match pi4-serialboot's own
// uart_init() exactly -- it's the universal fallback every revert lands
// on. The rest are exact or near-exact divisors of the 48MHz clock; see
// docs/RPI4_BRINGUP.md for the derivation.
struct baud_entry { uint32_t rate; uint32_t ibrd; uint32_t fbrd; };
static const struct baud_entry BAUD_TABLE[] = {
    { 115200,   26,  3 },
    { 230400,   13,  1 },
    { 460800,    6, 33 },
    { 921600,    3, 16 },
    { 1000000,   3,  0 },
    { 1500000,   2,  0 },
    { 2000000,   1, 32 },
    { 3000000,   1,  0 },
};
#define NUM_BAUD_RATES (sizeof(BAUD_TABLE) / sizeof(BAUD_TABLE[0]))

#define MAX_BLOCK 65536u
static uint8_t blk[MAX_BLOCK];

static void mbox_set_uart_clock(void) {
    static volatile uint32_t __attribute__((aligned(16))) mbox[9];
    mbox[0] = 9 * 4;
    mbox[1] = 0;
    mbox[2] = 0x00038002;
    mbox[3] = 12;
    mbox[4] = 8;
    mbox[5] = 2;
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
    sel |= (4u << 12) | (4u << 15);
    GPFSEL1 = sel;

    uint32_t pull = GPIO_PUP_PDN_CNTRL_REG0;
    pull &= ~((3u << 28) | (3u << 30));
    GPIO_PUP_PDN_CNTRL_REG0 = pull;
}

// PL011: disable while reprogramming the divisor, and write LCRH last --
// IBRD/FBRD only latch on the next LCRH write.
static void uart_set_baud(uint32_t ibrd, uint32_t fbrd) {
    while (UART0_FR & FR_BUSY) {}
    UART0_CR = 0;
    UART0_IBRD = ibrd;
    UART0_FBRD = fbrd;
    UART0_LCRH = (1 << 4) | (3 << 5);   // FIFO enable, 8N1
    UART0_CR = (1 << 0) | (1 << 8) | (1 << 9);
}

static void uart_init(void) {
    UART0_CR = 0;
    UART0_ICR = 0x7FF;
    gpio_uart0_pins();
    mbox_set_uart_clock();
    uart_set_baud(BAUD_TABLE[0].ibrd, BAUD_TABLE[0].fbrd);
    UART0_IMSC = 0;
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

static void uart_flush(void) {
    while (UART0_FR & FR_BUSY) {}
}

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

// Reads exactly n bytes, each allowed up to `per_byte_timeout` ticks to
// arrive. Returns 0 on success, -1 on any single byte timing out (a
// stall mid-block, not just slow overall transfer).
static int read_exact(uint8_t *buf, uint32_t n, uint64_t per_byte_timeout) {
    for (uint32_t i = 0; i < n; i++) {
        int c = uart_getc_timeout(per_byte_timeout);
        if (c < 0) {
            return -1;
        }
        buf[i] = (uint8_t)c;
    }
    return 0;
}

static int read_u32_le(uint32_t *out, uint64_t per_byte_timeout) {
    uint8_t b[4];
    if (read_exact(b, 4, per_byte_timeout) < 0) {
        return -1;
    }
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}

static uint32_t crc32_update(uint32_t crc, uint8_t byte) {
    crc ^= byte;
    for (int i = 0; i < 8; i++) {
        crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return crc;
}

static uint32_t crc32_block(const uint8_t *data, uint32_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        crc = crc32_update(crc, data[i]);
    }
    return ~crc;
}

// Command 'S': switch to the requested candidate rate, then require a
// "PING" within one_sec ticks at that new rate before trusting it --
// otherwise silently revert to 115200 so the host can retry the next
// candidate without a power-cycle.
static void handle_switch(uint64_t one_sec) {
    uint8_t idx;
    if (read_exact(&idx, 1, one_sec) < 0) {
        uart_puts("ER timeout reading rate index\n");
        return;
    }
    if (idx >= NUM_BAUD_RATES) {
        uart_puts("ER badidx\n");
        return;
    }

    uart_set_baud(BAUD_TABLE[idx].ibrd, BAUD_TABLE[idx].fbrd);

    static const char magic[4] = { 'P', 'I', 'N', 'G' };
    int matched = 0;
    uint64_t deadline = timer_now() + one_sec;
    int ok = 0;
    while (timer_now() < deadline) {
        if (!(UART0_FR & FR_RXFE)) {
            char c = (char)(UART0_DR & 0xFF);
            if (c == magic[matched]) {
                matched++;
                if (matched == 4) {
                    ok = 1;
                    break;
                }
            } else {
                matched = (c == magic[0]) ? 1 : 0;
            }
        }
    }

    if (ok) {
        uart_puts("PONG\n");
    } else {
        uart_set_baud(BAUD_TABLE[0].ibrd, BAUD_TABLE[0].fbrd);
        uart_puts("REVERTED\n");
    }
}

// Command 'X': bulk echo-and-verify stress test at the current baud.
static void handle_stress(uint64_t one_sec) {
    uint32_t size;
    if (read_u32_le(&size, one_sec) < 0) {
        uart_puts("ER timeout reading size\n");
        return;
    }
    if (size == 0 || size > MAX_BLOCK) {
        uart_puts("ER size\n");
        return;
    }

    // A stall mid-block at a marginal baud rate is exactly the failure
    // this is meant to catch, so keep the per-byte timeout tight (not
    // the generous 2x one_sec the chainloader itself uses for a whole
    // firmware image) rather than waiting a long time to notice.
    uint64_t per_byte = one_sec / 4;
    if (read_exact(blk, size, per_byte) < 0) {
        uart_puts("ER timeout mid-block\n");
        return;
    }

    uint32_t want_crc;
    if (read_u32_le(&want_crc, one_sec) < 0) {
        uart_puts("ER timeout reading trailer crc\n");
        return;
    }

    uint32_t got_crc = crc32_block(blk, size);
    if (got_crc == want_crc) {
        uart_puts("RXCRC OK\n");
    } else {
        uart_puts("RXCRC FAIL\n");
    }

    // Echo back regardless -- the host's own byte-exact comparison of
    // this against what it sent is the real, authoritative check (it
    // catches TX-direction corruption too, which the RXCRC line above
    // can't).
    for (uint32_t i = 0; i < size; i++) {
        uart_putc((char)blk[i]);
    }
    uart_flush();
}

void boot_main(uint32_t r0, uint32_t r1, uint32_t r2) {
    (void)r0; (void)r1; (void)r2;
    uart_init();
    const uint64_t one_sec = timer_freq();

    uart_puts("BAUDCAL ready\n");

    for (;;) {
        int c = uart_getc_timeout(one_sec * 30);
        if (c < 0) {
            continue;  // idle; no periodic reprint, keeps the link quiet
        }
        if (c == 'S') {
            handle_switch(one_sec);
        } else if (c == 'X') {
            handle_stress(one_sec);
        }
        // Unrecognized bytes are ignored rather than erroring -- keeps
        // this resilient to any stray noise on the line at a marginal
        // rate instead of getting stuck reporting errors about it.
    }
}
