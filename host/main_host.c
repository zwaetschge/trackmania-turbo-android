/* Desktop host: runs the lifted game headless on Linux x86-64 for bring-up.
 * usage: tmhost <TrackmaniaTurbo.exe> [user_dir] */
#include "tmrt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
/* g_mem_base and g_cur_func come from recomp_types.h (via tmrt.h) */

static void on_sig(int sig, siginfo_t *si, void *uc) {
    (void)uc; uintptr_t a=(uintptr_t)si->si_addr, b=(uintptr_t)g_mem_base;
    char m[128];
    if (a>=b && a<b+0x100000000ull) snprintf(m,sizeof m,"signal %d at guest %08X in func %08X", sig, (unsigned)(a-b), g_cur_func);
    else snprintf(m,sizeof m,"signal %d at host %p in func %08X", sig, si->si_addr, g_cur_func);
    tm_dump_state(m); fflush(stderr); _exit(139);
}
int main(int argc, char **argv) {
    struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_sigaction=on_sig; sa.sa_flags=SA_SIGINFO;
    sigaction(SIGSEGV,&sa,0); sigaction(SIGBUS,&sa,0); sigaction(SIGFPE,&sa,0); sigaction(SIGILL,&sa,0);
    if (argc < 2) { fprintf(stderr, "usage: %s TrackmaniaTurbo.exe [user_dir]\n", argv[0]); return 2; }
    if (getenv("TM_TRACE")) tm_log_level = TM_LOG_TRACE;
    char game[1024]; snprintf(game, sizeof game, "%s", argv[1]);
    char *s = strrchr(game, '/'); if (s) *s = 0; else strcpy(game, ".");
    const char *user = argc > 2 ? argv[2] : "/tmp/tm-user";
    char temp[1100]; snprintf(temp, sizeof temp, "%s/Temp", user);
    tm_path_set_roots(game, user, temp);
    void* tm_wsi_host_open(unsigned int w, unsigned int h);
    unsigned sw = 1280, sh = 720;                 /* TM_HOST_SCREEN=WxH: a smaller "desktop" renders faster on lavapipe */
    { const char *e = getenv("TM_HOST_SCREEN"); unsigned a, b; if (e && sscanf(e, "%ux%u", &a, &b) == 2 && a >= 160 && b >= 90) { sw = a; sh = b; } }
    tm_wsi_host_open(sw, sh);
    void tm_user32_set_screen(int w, int h);
    tm_user32_set_screen((int)sw, (int)sh);   /* the X window is the "desktop"; TM_WINDOW_FILL=1 makes the game fill it */
    int r = tm_run(argv[1]);
    fprintf(stderr, "tm_run returned %d\n", r);
    return r;
}
