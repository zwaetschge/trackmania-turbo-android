// tm_wsi.cpp - DXVK window-system integration for this port.
//
//   Android: presents to the activity's ANativeWindow.
//   Host:    presents to an X window (real, or virtual under Xvfb) so the full
//            pipeline including the swap chain can be tested headless.
//
// The game's Win32 HWND (from user32 CreateWindowExW) is only a token; the real
// presentation target is set by the platform layer (tm_wsi_set_window on
// Android, tm_wsi_host_open on the host).
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <chrono>

#ifdef __ANDROID__
#include <android/native_window.h>
#define VK_USE_PLATFORM_ANDROID_KHR
#else
#define VK_USE_PLATFORM_XLIB_KHR
#include <X11/Xlib.h>
#undef None
#undef Always
#undef Status
#undef Bool
#endif

#include "src/wsi/wsi_presenter.h"
#include "src/wsi/wsi_window.h"
#include "src/wsi/wsi_monitor.h"
#include "src/dxvk/dxvk_platform_exts.h"

namespace {
std::mutex g_mu;
uint32_t g_screen_w = 1280, g_screen_h = 720;
uint32_t g_buf_w = 0, g_buf_h = 0;
HWND g_handle = nullptr;
uintptr_t g_generation = 0x1000;
#ifdef __ANDROID__
ANativeWindow* g_window = nullptr;
#else
Display* g_display = nullptr;
Window g_xwindow = 0;
#endif
HMONITOR monitor() { return reinterpret_cast<HMONITOR>(uintptr_t{1}); }
bool have_target() {
#ifdef __ANDROID__
    return g_window != nullptr;
#else
    return g_display != nullptr && g_xwindow != 0;
#endif
}
}

extern "C" {
void tm_wsi_get_screen(uint32_t* w, uint32_t* h) { std::lock_guard<std::mutex> l(g_mu); if (w) *w = g_screen_w; if (h) *h = g_screen_h; }
void* tm_wsi_window_handle(void) { std::lock_guard<std::mutex> l(g_mu); return g_handle; }

#ifdef __ANDROID__
void* tm_wsi_set_window(ANativeWindow* window) {
    std::lock_guard<std::mutex> l(g_mu);
    if (window == g_window) return g_handle;
    if (window) { ANativeWindow_acquire(window); g_screen_w = (uint32_t)ANativeWindow_getWidth(window); g_screen_h = (uint32_t)ANativeWindow_getHeight(window); }
    if (g_window) ANativeWindow_release(g_window);
    g_window = window;
    g_buf_w = g_buf_h = 0;
    // The HWND token stays the same for the life of the process: the swap chain keeps
    // it, and a new surface after a pause must still be "its" window.
    if (window && !g_handle) g_handle = reinterpret_cast<HWND>(++g_generation);
    return g_handle;
}
#else
// Open a real X window (virtual under Xvfb). Returns the HWND token.
void* tm_wsi_host_open(uint32_t w, uint32_t h) {
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_display) {
        XInitThreads();
        g_display = XOpenDisplay(nullptr);
        if (!g_display) return nullptr;
    }
    if (!g_xwindow) {
        int s = DefaultScreen(g_display);
        g_xwindow = XCreateSimpleWindow(g_display, RootWindow(g_display, s), 0, 0, w, h, 0,
                                        BlackPixel(g_display, s), BlackPixel(g_display, s));
        XMapWindow(g_display, g_xwindow);
        XSync(g_display, 0);
    }
    g_screen_w = w; g_screen_h = h;
    g_handle = reinterpret_cast<HWND>(++g_generation);
    return g_handle;
}
void tm_wsi_host_display(void** dpy, unsigned long* win) {
    std::lock_guard<std::mutex> l(g_mu);
    if (dpy) *dpy = g_display;
    if (win) *win = (unsigned long)g_xwindow;
}
#endif
}  // extern "C"

// ---- present gate ------------------------------------------------------------
// While the app is paused or has no surface, the game's Present call waits here
// (with the machine lock released by the caller) instead of presenting to a window
// that is going away. tm_wsi_pause(1, ms) returns once no Present is in flight, or
// after ms milliseconds.
namespace {
std::mutex g_gate_mu;
std::condition_variable g_gate_cv;
bool g_paused = false, g_presenting = false;
bool target_now() { std::lock_guard<std::mutex> l(g_mu); return have_target(); }
}
extern "C" {
void tm_wsi_present_begin(void) {
    std::unique_lock<std::mutex> l(g_gate_mu);
    while (g_paused || !target_now()) g_gate_cv.wait_for(l, std::chrono::milliseconds(100));
    g_presenting = true;
}
void tm_wsi_present_end(void) {
    std::lock_guard<std::mutex> l(g_gate_mu);
    g_presenting = false;
    g_gate_cv.notify_all();
}
void tm_wsi_pause(int paused, int wait_ms) {
    std::unique_lock<std::mutex> l(g_gate_mu);
    g_paused = paused != 0;
    g_gate_cv.notify_all();
    if (paused && wait_ms > 0) g_gate_cv.wait_for(l, std::chrono::milliseconds(wait_ms), [] { return !g_presenting; });
}
}

namespace dxvk {
DxvkPlatformExts DxvkPlatformExts::s_instance;
std::string_view DxvkPlatformExts::getName() { return "TM WSI"; }
DxvkNameSet DxvkPlatformExts::getInstanceExtensions() {
    DxvkNameSet names;
    names.add(VK_KHR_SURFACE_EXTENSION_NAME);
#ifdef __ANDROID__
    names.add(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#else
    names.add(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#endif
    return names;
}
DxvkNameSet DxvkPlatformExts::getDeviceExtensions(uint32_t) { return {}; }
void DxvkPlatformExts::initInstanceExtensions() {}
void DxvkPlatformExts::initDeviceExtensions(const DxvkInstance*) {}
}

namespace dxvk::wsi {

VkResult createSurface(HWND window, const Rc<vk::InstanceFn>& vki, VkSurfaceKHR* surface) {
    std::lock_guard<std::mutex> l(g_mu);
    if (!have_target()) return VK_ERROR_SURFACE_LOST_KHR;
#ifdef __ANDROID__
    (void)window;
    auto create = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(vkGetInstanceProcAddr(vki->instance(), "vkCreateAndroidSurfaceKHR"));
    if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkAndroidSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    info.window = g_window;
    return create(vki->instance(), &info, nullptr, surface);
#else
    (void)window;
    auto create = reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(vkGetInstanceProcAddr(vki->instance(), "vkCreateXlibSurfaceKHR"));
    if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkXlibSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
    info.dpy = g_display; info.window = g_xwindow;
    return create(vki->instance(), &info, nullptr, surface);
#endif
}

HMONITOR getDefaultMonitor() { return monitor(); }
HMONITOR enumMonitors(uint32_t index) { return index ? nullptr : monitor(); }
bool getDisplayName(HMONITOR h, WCHAR (&name)[32]) {
    if (h != monitor()) return false;
    const wchar_t d[] = L"\\\\.\\DISPLAY1"; std::fill(std::begin(name), std::end(name), 0); std::copy(std::begin(d), std::end(d), name);
    return true;
}
bool getDesktopCoordinates(HMONITOR h, RECT* r) {
    if (h != monitor() || !r) return false;
    std::lock_guard<std::mutex> l(g_mu); *r = {0, 0, (LONG)g_screen_w, (LONG)g_screen_h}; return true;
}
bool getCurrentDisplayMode(HMONITOR h, WsiMode* m) {
    if (h != monitor() || !m) return false;
    std::lock_guard<std::mutex> l(g_mu); *m = {g_screen_w, g_screen_h, {60, 1}, 32, false}; return true;
}
bool getDesktopDisplayMode(HMONITOR h, WsiMode* m) { return getCurrentDisplayMode(h, m); }
bool getDisplayMode(HMONITOR h, uint32_t i, WsiMode* m) {
    if (h != monitor() || !m) return false;
    static constexpr uint32_t modes[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}};
    std::lock_guard<std::mutex> l(g_mu);
    uint32_t n = 0;
    for (auto& md : modes) { if (md[0] > g_screen_w || md[1] > g_screen_h) continue; if (n++ == i) { *m = {md[0], md[1], {60, 1}, 32, false}; return true; } }
    if (n == i) { *m = {g_screen_w, g_screen_h, {60, 1}, 32, false}; return true; }
    return false;
}
void getWindowSize(HWND window, uint32_t* w, uint32_t* h) {
    std::lock_guard<std::mutex> l(g_mu);
    bool ok = have_target() && window == g_handle;
    if (w) *w = ok ? (g_buf_w ? g_buf_w : g_screen_w) : 0;
    if (h) *h = ok ? (g_buf_h ? g_buf_h : g_screen_h) : 0;
}
void resizeWindow(HWND window, DxvkWindowState*, uint32_t w, uint32_t h) {
    std::lock_guard<std::mutex> l(g_mu);
#ifdef __ANDROID__
    if (g_window && window == g_handle && ANativeWindow_setBuffersGeometry(g_window, (int32_t)w, (int32_t)h, 0) == 0) { g_buf_w = w; g_buf_h = h; }
#else
    (void)window; (void)w; (void)h;
#endif
}
bool isWindow(HWND window) { std::lock_guard<std::mutex> l(g_mu); return have_target() && window == g_handle; }
HMONITOR getWindowMonitor(HWND window) { return isWindow(window) ? monitor() : nullptr; }
bool setWindowMode(HMONITOR h, HWND window, const WsiMode*, bool) { return h == monitor() && isWindow(window); }
bool enterFullscreenMode(HMONITOR h, HWND window, DxvkWindowState*, bool) { return h == monitor() && isWindow(window); }
bool leaveFullscreenMode(HWND window, DxvkWindowState*) { return isWindow(window); }
bool restoreDisplayMode(HMONITOR h) { return h == monitor(); }

}  // namespace dxvk::wsi
