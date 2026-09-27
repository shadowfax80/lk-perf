#include <lk/console_cmd.h>
#include <stdio.h>

static int cmd_love(int argc, const console_cmd_args *argv) {
    printf("\n");
    printf("  **    **\n");
    printf(" ****  ****\n");
    printf("************\n");
    printf(" **********\n");
    printf("  ********\n");
    printf("   ******\n");
    printf("    ****\n");
    printf("     **\n");
    printf("\n");
    printf("   I LOVE U\n");
    printf("\n");
    return 0;
}

STATIC_COMMAND_START
STATIC_COMMAND("love", "print an I LOVE U banner", &cmd_love)
STATIC_COMMAND_END(love);
