#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int recomp_game_stack_trace_on;
extern void recomp_game_stack_trace_init(void);
extern void recomp_game_stack_log(const char *emitter, const char *format, ...);

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
    return n;
}

static void run_case(const char *path, int enabled)
{
    FILE *f;
    _putenv_s("RECOMP_GAME_STACK_TRACE", enabled ? "1" : "");
    recomp_game_stack_trace_init();
    f = freopen(path, "w", stdout);
    if (!f) exit(2);
    recomp_game_stack_log("sub_test", "[GAME_STACK] depth=%u\\n", 7u);
    fflush(stdout);
    fclose(f);
}

int main(void)
{
    const char *off = "game_stack_trace_off.tmp";
    const char *on = "game_stack_trace_on.tmp";
    long n;
    char text[128] = {0};
    FILE *f;

    run_case(off, 0);
    n = file_size(off);
    if (n != 0) return 10;
    remove(off);

    run_case(on, 1);
    n = file_size(on);
    if (n <= 0) return 11;
    f = fopen(on, "rb");
    if (!f) return 12;
    fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    remove(on);
    if (strstr(text, "[GAME_STACK] depth=7") == NULL) return 13;
    if (!recomp_game_stack_trace_on) return 14;
    return 0;
}
