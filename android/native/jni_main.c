/* JNI host for the lifted game: runs tm_run on a big-stack thread, mirrors
 * the runtime log into logcat and a ring the UI polls, reports fatal signals
 * with the guest state of the faulting thread, hands the activity's Surface to
 * the DXVK window-system glue and forwards gamepad/keyboard input. */
#define _GNU_SOURCE
#include "tmrt.h"
#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <pthread.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_status[512] = "";
#define TAIL_LINES 60
static char g_tail[TAIL_LINES][256];
static int g_tail_n;
static char g_paths[4][1024];
static FILE *g_logfile;

static void log_sink(int level, const char *line) {
    int prio = level == TM_LOG_ERR ? ANDROID_LOG_ERROR : level == TM_LOG_WARN ? ANDROID_LOG_WARN : ANDROID_LOG_INFO;
    __android_log_print(prio, "TMRecomp", "[%u] %s", tm_thread_id(), line);
    pthread_mutex_lock(&g_mu);
    snprintf(g_tail[g_tail_n % TAIL_LINES], 256, "%s", line);
    g_tail_n++;
    if (g_logfile) { fprintf(g_logfile, "[%u] %s\n", tm_thread_id(), line); fflush(g_logfile); }
    pthread_mutex_unlock(&g_mu);
}
static void status_sink(const char *line) {
    pthread_mutex_lock(&g_mu);
    snprintf(g_status, sizeof g_status, "%s", line);
    pthread_mutex_unlock(&g_mu);
}

static void on_signal(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    uintptr_t a = (uintptr_t)si->si_addr, b = (uintptr_t)g_mem_base;
    char msg[256];
    uint32_t fn = tm_cpu_bound() ? g_cur_func : 0;      /* host-only threads have no guest state */
    if (a >= b && a < b + 0x100000000ull)
        snprintf(msg, sizeof msg, "signal %d at guest address %08X in guest function %08X", sig, (unsigned)(a - b), fn);
    else
        snprintf(msg, sizeof msg, "signal %d at host address %p in guest function %08X", sig, si->si_addr, fn);
    status_sink(msg);
    TM_ERR("CRASH: %s", msg);
    tm_dump_state(msg);
    sleep(30);   /* keep the process (and status) visible for the UI poll */
    _exit(128 + sig);
}

static void fatal_hold(const char *why) {
    (void)why;
    /* keep the activity alive with the stop reason on screen */
    for (;;) pause();
}

static void *boot(void *arg) {
    (void)arg;
    tm_path_set_roots(g_paths[1], g_paths[2], g_paths[3]);
    char lp[1100]; snprintf(lp, sizeof lp, "%s/tmrecomp.log", g_paths[2]);
    g_logfile = fopen(lp, "w");
    int r = tm_run(g_paths[0]);
    tm_status("Guest entry returned %d", r);
    return NULL;
}

static void rumble_init(JNIEnv *env, jclass c);
JNIEXPORT void JNICALL Java_ch_plum_tmturbo_recomp_MainActivity_nativeStart(JNIEnv *env, jclass c, jstring exe, jstring game, jstring user, jstring temp) {
    (void)c;
    jstring s[4] = { exe, game, user, temp };
    for (int i = 0; i < 4; i++) { const char *u = (*env)->GetStringUTFChars(env, s[i], NULL); snprintf(g_paths[i], 1024, "%s", u); (*env)->ReleaseStringUTFChars(env, s[i], u); }
    /* DXVK writes its log and pipeline state cache next to the user data
     * (the process working directory is not writable on Android). */
    setenv("DXVK_LOG_PATH", g_paths[2], 1);
    setenv("DXVK_STATE_CACHE_PATH", g_paths[2], 1);
    /* DXVK, OpenAL Soft and the C++ runtime report on stderr, which goes nowhere
     * on Android: keep it next to the runtime log. */
    { char ep[1100]; snprintf(ep, sizeof ep, "%s/stderr.log", g_paths[2]);
      if (freopen(ep, "w", stderr)) setvbuf(stderr, NULL, _IOLBF, 0); }
    tm_log_sink = log_sink;
    tm_status_sink = status_sink;
    rumble_init(env, c);
    tm_fatal_hook = fatal_hold;
    if (getenv("TM_TRACE")) tm_log_level = TM_LOG_TRACE;
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGFPE, &sa, NULL);
    pthread_attr_t a; pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 256u << 20);
    pthread_t t; pthread_create(&t, &a, boot, NULL);
    pthread_attr_destroy(&a);
}

JNIEXPORT jstring JNICALL Java_ch_plum_tmturbo_recomp_MainActivity_nativeStatus(JNIEnv *env, jclass c) {
    (void)c;
    pthread_mutex_lock(&g_mu);
    jstring r = (*env)->NewStringUTF(env, g_status);
    pthread_mutex_unlock(&g_mu);
    return r;
}

JNIEXPORT jstring JNICALL Java_ch_plum_tmturbo_recomp_MainActivity_nativeLogTail(JNIEnv *env, jclass c) {
    (void)c;
    static char buf[TAIL_LINES * 258];
    pthread_mutex_lock(&g_mu);
    size_t k = 0;
    int start = g_tail_n > TAIL_LINES ? g_tail_n - TAIL_LINES : 0;
    for (int i = start; i < g_tail_n; i++) k += (size_t)snprintf(buf + k, sizeof buf - k, "%s\n", g_tail[i % TAIL_LINES]);
    buf[k] = 0;
    pthread_mutex_unlock(&g_mu);
    return (*env)->NewStringUTF(env, buf);
}

/* ---- presentation target + input ---- */
void *tm_wsi_set_window(ANativeWindow *window);     /* gfx/dxvk/tm_wsi.cpp */
uint32_t tm_gfx_present_count(void);                /* gfx/d3d11_bridge.cpp */
void tm_user32_set_screen(int w, int h);
void tm_user32_set_fill(int on);

JNIEXPORT void JNICALL Java_ch_plum_tmturbo_recomp_MainActivity_nativeSetSurface(JNIEnv *env, jclass c, jobject surface) {
    (void)c;
    ANativeWindow *w = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    if (w) {
        int ww = ANativeWindow_getWidth(w), hh = ANativeWindow_getHeight(w);
        tm_user32_set_screen(ww, hh);
        tm_user32_set_fill(1);          /* the game window is the whole surface */
        __android_log_print(ANDROID_LOG_INFO, "TMRecomp", "surface %dx%d", ww, hh);
        /* Ask for a steady 60 Hz (TM_REFRESH_HZ to change, 0 = leave it to the system):
         * in motion the game renders 60-75 frames per second on this class of device,
         * which a 120 Hz mode would show with uneven pacing. API 30+, looked up at run time. */
        int32_t (*set_rate)(ANativeWindow *, float, int8_t) = (int32_t (*)(ANativeWindow *, float, int8_t))dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate");
        const char *hz = getenv("TM_REFRESH_HZ");
        float rate = hz && *hz ? (float)atof(hz) : 60.0f;
        if (set_rate && rate > 0) set_rate(w, rate, 0 /* ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT */);
    }
    tm_wsi_set_window(w);
    if (w) ANativeWindow_release(w);                /* tm_wsi holds its own reference */
}

JNIEXPORT jint JNICALL Java_ch_plum_tmturbo_recomp_MainActivity_nativePresentCount(JNIEnv *env, jclass c) {
    (void)env; (void)c;
    return (jint)tm_gfx_present_count();
}

JNIEXPORT void JNICALL Java_ch_plum_tmturbo_recomp_MainActivity_nativePad(JNIEnv *env, jclass c, jint index, jboolean connected, jint buttons, jint lt, jint rt, jint lx, jint ly, jint rx, jint ry) {
    (void)env; (void)c;
    tm_input_pad_slot(index, connected ? 1 : 0, (uint32_t)buttons, lt, rt, lx, ly, rx, ry);
}

/* Vibration: XInputSetState runs on a game thread; MainActivity.onRumble(slot, low, high)
   is called on it (attached to the VM once, detached when the thread ends). */
static JavaVM *g_vm;
static jclass g_activity_class;
static jmethodID g_on_rumble;
static pthread_key_t g_jni_key;
static void jni_detach(void *p) { (void)p; if (g_vm) (*g_vm)->DetachCurrentThread(g_vm); }
static void rumble_sink(int pad, int low, int high) {
    if (!g_vm || !g_on_rumble) return;
    JNIEnv *e = NULL;
    if ((*g_vm)->GetEnv(g_vm, (void **)&e, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_vm)->AttachCurrentThread(g_vm, &e, NULL) != JNI_OK) return;
        pthread_setspecific(g_jni_key, (void *)1);
    }
    (*e)->CallStaticVoidMethod(e, g_activity_class, g_on_rumble, pad, low, high);
    if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
}
static void rumble_init(JNIEnv *env, jclass c) {
    if (g_vm) return;
    (*env)->GetJavaVM(env, &g_vm);
    g_activity_class = (jclass)(*env)->NewGlobalRef(env, c);
    g_on_rumble = (*env)->GetStaticMethodID(env, c, "onRumble", "(III)V");
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); g_on_rumble = NULL; }
    pthread_key_create(&g_jni_key, jni_detach);
    tm_rumble_sink = rumble_sink;
}

void tm_gfx_set_max_fps(int fps);   /* gfx/d3d11_bridge.cpp */
JNIEXPORT void JNICALL Java_ch_plum_tmturbo_recomp_MainActivity_nativeSetMaxFps(JNIEnv *env, jclass c, jint fps) {
    (void)env; (void)c;
    tm_gfx_set_max_fps(fps);
}

JNIEXPORT void JNICALL Java_ch_plum_tmturbo_recomp_MainActivity_nativeMouse(JNIEnv *env, jclass c, jfloat nx, jfloat ny, jint buttons, jint wheel) {
    (void)env; (void)c;
    tm_input_mouse(nx, ny, buttons, wheel);
}

JNIEXPORT void JNICALL Java_ch_plum_tmturbo_recomp_MainActivity_nativeKey(JNIEnv *env, jclass c, jint vk, jboolean down) {
    (void)env; (void)c;
    tm_input_key(vk, down ? 1 : 0);
}

/* Development switches (TM_WATCHDOG, TM_TRACE, DXVK_HUD, ...) passed as intent
 * extras: must be set before nativeStart. */
/* Activity paused / resumed: hold the game at its next frame and silence the audio
 * device, so the surface can be destroyed and replaced under a running game. */
void tm_wsi_pause(int paused, int wait_ms);
void tm_audio_pause(int paused);
JNIEXPORT void JNICALL Java_ch_plum_tmturbo_recomp_MainActivity_nativePause(JNIEnv *env, jclass c, jboolean paused) {
    (void)env; (void)c;
    __android_log_print(ANDROID_LOG_INFO, "TMRecomp", "%s", paused ? "pause" : "resume");
    tm_wsi_pause(paused ? 1 : 0, 500);
    tm_audio_pause(paused ? 1 : 0);
}

JNIEXPORT void JNICALL Java_ch_plum_tmturbo_recomp_MainActivity_nativeSetEnv(JNIEnv *env, jclass c, jstring k, jstring v) {
    (void)c;
    const char *ks = (*env)->GetStringUTFChars(env, k, NULL), *vs = (*env)->GetStringUTFChars(env, v, NULL);
    setenv(ks, vs, 1);
    __android_log_print(ANDROID_LOG_INFO, "TMRecomp", "env %s=%s", ks, vs);
    (*env)->ReleaseStringUTFChars(env, k, ks); (*env)->ReleaseStringUTFChars(env, v, vs);
}
