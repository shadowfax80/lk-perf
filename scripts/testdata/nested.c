/* Nested-call test program for validating DWARF-CFI-based unwinding,
 * compiled fully optimized WITHOUT frame pointers -- the whole point
 * of using DWARF CFI is that it doesn't need them, unlike an FP-chain
 * walker. */
volatile int sink;

__attribute__((noinline)) int leaf_func(int x) {
    for (volatile int i = 0; i < 1000; i++) {
        sink += i * x;
    }
    return sink;
}

__attribute__((noinline)) int mid_func(int x) {
    return leaf_func(x) + leaf_func(x + 1);
}

__attribute__((noinline)) int outer_func(int x) {
    return mid_func(x) + mid_func(x + 1);
}

int main(void) {
    return outer_func(1);
}
