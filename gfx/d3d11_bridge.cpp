// d3d11_bridge.cpp - Direct3D 11 + DXGI for the 32-bit guest, backed by native
// DXVK (Vulkan). Each guest COM object wraps a native DXVK interface pointer;
// methods marshal 32-bit guest arguments to native and wrap returned interfaces.
//
// POD structs without pointers share the 32-bit guest and native layout, so
// they pass through by host pointer. Only structs/arrays carrying pointers or
// interface handles are rebuilt (subresource data, input layout, interface
// arrays, Map result).
#define _GNU_SOURCE
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <time.h>
#include <vector>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#include <d3d11.h>
#include <dxgi.h>

extern "C" {
#include "tmrt.h"
#include "tmcom.h"
}

extern "C" HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID, void**);
extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID, void**);
extern "C" void* tm_wsi_window_handle(void);
extern "C" void tm_wsi_present_begin(void);
extern "C" void tm_wsi_present_end(void);
extern "C" void tm_machine_unlock(void);
extern "C" int g_tm_dxvk_implicit_flush;         // DXVK's timer-driven flush (patched in, see gfx/dxvk/prepare.py)
extern "C" int tm_diag_on(void);
extern "C" uint64_t tm_diag_take(int kind);
extern "C" void tm_machine_lock(void);
extern "C" void tm_map_flush(uint32_t res, uint32_t sub);
extern "C" void tm_map_invalidate(uint32_t res);
extern "C" void tm_map_forget(uint32_t res);
extern "C" uint32_t tm_heap_alloc(uint32_t h, uint32_t flags, uint32_t size);
extern "C" uint32_t tm_heap_process(void);

#define HR_OK 0
#define HR_FAIL 0x80004005u
#define HR_NOINTERFACE 0x80004002u

// ---------------------------------------------------------------- wrap cache
namespace {
std::mutex g_mu;
std::unordered_map<void*, uint32_t> g_wrap;    // native ptr -> guest obj VA
tm_com_class g_cls[40];
enum ClassId {
    C_DEVICE, C_CONTEXT, C_BUFFER, C_TEX1D, C_TEX2D, C_TEX3D, C_RTV, C_DSV, C_SRV, C_UAV,
    C_INPUTLAYOUT, C_VS, C_PS, C_GS, C_HS, C_DS, C_CS, C_SAMPLER, C_BLEND, C_DEPTHSTENCIL,
    C_RASTERIZER, C_QUERY, C_CLASSLINKAGE,
    C_DXGIFACTORY, C_DXGIADAPTER, C_DXGIOUTPUT, C_DXGIDEVICE, C_DXGISWAPCHAIN, C_DXGISURFACE,
    C_COUNT
};
}

// forward
static uint32_t wrap(void* native, int cls);
static inline void* nat(uint32_t gobj) { return gobj ? tm_com_host(gobj) : nullptr; }

template <class T> static inline T* N(uint32_t gobj) { return reinterpret_cast<T*>(nat(gobj)); }

// Read a guest array of interface pointers into a native array (host-side vector).
static void unwrap_array(uint32_t gptr, uint32_t count, std::vector<void*>& out) {
    out.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t g = gptr ? rd32(gptr + 4 * i) : 0;
        out[i] = nat(g);
    }
}

// ================================================================= IUnknown
static IUnknown* as_unknown(uint32_t gobj) { return reinterpret_cast<IUnknown*>(nat(gobj)); }

static void m_AddRef(void) {
    IUnknown* u = as_unknown(ARG(0));
    ULONG r = u ? u->AddRef() : 1;
    tm_com_addref(ARG(0));
    RET(r, 1);
}
static int class_for_iid(uint32_t riid);
static void m_Release(void) {
    uint32_t o = ARG(0);
    IUnknown* u = as_unknown(o);
    ULONG r = u ? u->Release() : 0;
    // Mirror into the guest wrapper. On the last guest reference drop the cache
    // entry and staging buffers, then let tm_com_release free the wrapper and
    // its host slot (it does the decrement itself - decrementing here as well
    // left every released object's slot allocated until the table filled up).
    if (rd32(o + 4) == 1) {
        tm_map_forget(o);
        std::lock_guard<std::mutex> l(g_mu);
        if (u) g_wrap.erase((void*)u);
    }
    tm_com_release(o, nullptr);
    RET(r, 1);
}
static void m_QueryInterface(void) {
    uint32_t o = ARG(0), riid = ARG(1), ppv = ARG(2);
    IUnknown* u = as_unknown(o);
    if (!u) { if (ppv) wr32(ppv, 0); RET(HR_NOINTERFACE, 3); }
    int cls = class_for_iid(riid);
    if (cls < 0) {
        // Unknown riid: hand back the same object if the game just wants IUnknown-ish.
        tm_com_addref(o); u->AddRef();
        if (ppv) wr32(ppv, o);
        RET(HR_OK, 3);
    }
    IUnknown* out = nullptr;
    GUID iid; memcpy(&iid, G2H(riid), 16);
    HRESULT hr = u->QueryInterface(iid, (void**)&out);
    if (FAILED(hr) || !out) { if (ppv) wr32(ppv, 0); RET(hr ? (uint32_t)hr : HR_NOINTERFACE, 3); }
    if (ppv) wr32(ppv, wrap(out, cls));
    RET(HR_OK, 3);
}

// DeviceChild shared tail
static void m_GetDevice(void) {
    ID3D11DeviceChild* c = N<ID3D11DeviceChild>(ARG(0));
    ID3D11Device* dev = nullptr;
    if (c) c->GetDevice(&dev);
    if (ARG(1)) wr32(ARG(1), dev ? wrap(dev, C_DEVICE) : 0);
    RET(0, 2);
}
static void m_GetPrivateData(void) { if (ARG(2)) {} RET(HR_FAIL, 4); }
static void m_SetPrivateData(void) { RET(HR_OK, 4); }
static void m_SetPrivateDataInterface(void) { RET(HR_OK, 3); }
static void res_GetType(void) {
    ID3D11Resource* r = N<ID3D11Resource>(ARG(0));
    D3D11_RESOURCE_DIMENSION d = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    if (r) r->GetType(&d);
    if (ARG(1)) wr32(ARG(1), d);
    RET(0, 2);
}
static void res_SetEvictionPriority(void) { RET(0, 2); }
static void res_GetEvictionPriority(void) { RET(0, 1); }
static void view_GetResource(void) {
    ID3D11View* v = N<ID3D11View>(ARG(0));
    ID3D11Resource* r = nullptr;
    if (v) v->GetResource(&r);
    int cls = C_TEX2D;
    if (r) {
        D3D11_RESOURCE_DIMENSION d = D3D11_RESOURCE_DIMENSION_UNKNOWN; r->GetType(&d);
        cls = d == D3D11_RESOURCE_DIMENSION_BUFFER ? C_BUFFER : d == D3D11_RESOURCE_DIMENSION_TEXTURE1D ? C_TEX1D
            : d == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? C_TEX3D : C_TEX2D;
    }
    if (ARG(1)) wr32(ARG(1), r ? wrap(r, cls) : 0);
    RET(0, 2);
}

// The desc structs are POD, so we can let native write straight into guest memory.
static void buf_GetDesc(void) { auto* o = N<ID3D11Buffer>(ARG(0)); if (o && ARG(1)) o->GetDesc((D3D11_BUFFER_DESC*)G2H(ARG(1))); RET(0, 2); }
static void t2d_GetDesc(void) { auto* o = N<ID3D11Texture2D>(ARG(0)); if (o && ARG(1)) o->GetDesc((D3D11_TEXTURE2D_DESC*)G2H(ARG(1))); RET(0, 2); }
static void t1d_GetDesc(void) { auto* o = N<ID3D11Texture1D>(ARG(0)); if (o && ARG(1)) o->GetDesc((D3D11_TEXTURE1D_DESC*)G2H(ARG(1))); RET(0, 2); }
static void t3d_GetDesc(void) { auto* o = N<ID3D11Texture3D>(ARG(0)); if (o && ARG(1)) o->GetDesc((D3D11_TEXTURE3D_DESC*)G2H(ARG(1))); RET(0, 2); }
static void smp_GetDesc(void) { auto* o = N<ID3D11SamplerState>(ARG(0)); if (o && ARG(1)) o->GetDesc((D3D11_SAMPLER_DESC*)G2H(ARG(1))); RET(0, 2); }
static void bl_GetDesc(void) { auto* o = N<ID3D11BlendState>(ARG(0)); if (o && ARG(1)) o->GetDesc((D3D11_BLEND_DESC*)G2H(ARG(1))); RET(0, 2); }
static void dss_GetDesc(void) { auto* o = N<ID3D11DepthStencilState>(ARG(0)); if (o && ARG(1)) o->GetDesc((D3D11_DEPTH_STENCIL_DESC*)G2H(ARG(1))); RET(0, 2); }
static void rs_GetDesc(void) { auto* o = N<ID3D11RasterizerState>(ARG(0)); if (o && ARG(1)) o->GetDesc((D3D11_RASTERIZER_DESC*)G2H(ARG(1))); RET(0, 2); }
static void rtv_GetDesc(void) { auto* o = N<ID3D11RenderTargetView>(ARG(0)); if (o && ARG(1)) o->GetDesc((D3D11_RENDER_TARGET_VIEW_DESC*)G2H(ARG(1))); RET(0, 2); }
static void dsv_GetDesc(void) { auto* o = N<ID3D11DepthStencilView>(ARG(0)); if (o && ARG(1)) o->GetDesc((D3D11_DEPTH_STENCIL_VIEW_DESC*)G2H(ARG(1))); RET(0, 2); }
static void srv_GetDesc(void) { auto* o = N<ID3D11ShaderResourceView>(ARG(0)); if (o && ARG(1)) o->GetDesc((D3D11_SHADER_RESOURCE_VIEW_DESC*)G2H(ARG(1))); RET(0, 2); }
static void uav_GetDesc(void) { auto* o = N<ID3D11UnorderedAccessView>(ARG(0)); if (o && ARG(1)) o->GetDesc((D3D11_UNORDERED_ACCESS_VIEW_DESC*)G2H(ARG(1))); RET(0, 2); }


// ================================================================= frame capture (diagnostics)
// F12 (tm_gfx_capture_request) dumps one frame: every time the game switches
// render targets, the targets it is leaving are copied to a staging texture and
// written to <dir>/cap_NNN_{rt<i>|ds}.bin, plus cap_index.txt with the draw
// count per pass. Comparing a capture from the Linux host with one from the
// device shows the first pass where a driver renders something different.
// Files: "TMCAP w h bpp format\n" + w*h*bpp bytes, subsampled to <= 480 wide.
#include <sys/stat.h>
namespace {
std::unordered_map<void*, std::vector<uint8_t>> g_shader_code;   // native shader -> DXBC (for capture dumps)
std::unordered_map<void*, std::string> g_layout_desc;            // native input layout -> element list
std::mutex g_reg_mu;
void* g_cap_vs = nullptr; void* g_cap_ps = nullptr;              // shaders of the pass's last draw
std::atomic<int> g_cap_req{0};
int g_cap_defer_req = 0;
int g_cap_active = 0, g_cap_pass = 0, g_cap_draws = 0;
ID3D11RenderTargetView* g_cap_rtv[8] = {};
ID3D11DepthStencilView* g_cap_dsv = nullptr;
UINT g_cap_nrt = 0;
char g_cap_dir[512];
FILE* g_cap_index = nullptr;
std::string g_cap_last;

// Deferred capture (F8, or F12 with TM_CAPTURE_DEFER=1): the targets are copied on the GPU
// while the frame is built and read back only after its last pass, so the frame is
// submitted the way it normally is (the immediate capture waits for the GPU after every
// pass, which hides anything that depends on several passes sharing one submission).
// Only colour targets at least 640 wide are kept, from the first pass on that renders
// to a format listed in TM_CAPTURE_FROM_FMT (default 34), for at most 16 passes.
int g_cap_defer = 0, g_cap_defer_on = 0, g_cap_defer_left = 0, g_cap_defer_fmt = 34, g_cap_defer_minw = 640;
struct CapDeferred { ID3D11Texture2D* tex; int pass; char tag[16]; UINT w, h; DXGI_FORMAT fmt; };
std::vector<CapDeferred> g_cap_deferred;
void cap_dump_tex(ID3D11DeviceContext* c, ID3D11Texture2D* tex, UINT sub, UINT w, UINT h, DXGI_FORMAT fmt, int pass, const char* tag) {
    D3D11_TEXTURE2D_DESC sd{}; sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = fmt; sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev);
    ID3D11Texture2D* st = nullptr;
    HRESULT hr = dev->CreateTexture2D(&sd, nullptr, &st);
    if (SUCCEEDED(hr) && st) {
        c->CopySubresourceRegion(st, 0, 0, 0, 0, tex, sub, nullptr);
        D3D11_MAPPED_SUBRESOURCE m{};
        hr = c->Map(st, 0, D3D11_MAP_READ, 0, &m);
        if (SUCCEEDED(hr) && m.pData) {
            UINT bpp = m.RowPitch / w; if (!bpp) bpp = 1;
            UINT step = w > 480 ? (w + 479) / 480 : 1;
            UINT ow = (w + step - 1) / step, oh = (h + step - 1) / step;
            char path[640]; snprintf(path, sizeof path, "%s/cap_%03d_%s.bin", g_cap_dir, pass, tag);
            if (FILE* f = fopen(path, "wb")) {
                fprintf(f, "TMCAP %u %u %u %u\n", ow, oh, bpp, (unsigned)fmt);
                std::vector<uint8_t> row(ow * bpp);
                for (UINT y = 0; y < h; y += step) {
                    const uint8_t* src = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                    for (UINT x = 0, o = 0; x < w; x += step, o++) memcpy(&row[o * bpp], src + (size_t)x * bpp, bpp);
                    fwrite(row.data(), 1, row.size(), f);
                }
                fclose(f);
            }
            c->Unmap(st, 0);
        } else if (g_cap_index) fprintf(g_cap_index, "  %s: Map failed %08X\n", tag, (unsigned)hr);
        st->Release();
    } else if (g_cap_index) fprintf(g_cap_index, "  %s: staging create failed %08X\n", tag, (unsigned)hr);
    if (dev) dev->Release();
}
void cap_write(ID3D11DeviceContext* c, ID3D11View* view, bool depth, const char* tag) {
    ID3D11Resource* res = nullptr; view->GetResource(&res);
    if (!res) return;
    D3D11_RESOURCE_DIMENSION dim; res->GetType(&dim);
    UINT mip = 0, slice = 0;
    if (depth) { D3D11_DEPTH_STENCIL_VIEW_DESC vd; static_cast<ID3D11DepthStencilView*>(view)->GetDesc(&vd);
        if (vd.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D) mip = vd.Texture2D.MipSlice;
        else if (vd.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY) { mip = vd.Texture2DArray.MipSlice; slice = vd.Texture2DArray.FirstArraySlice; }
    } else { D3D11_RENDER_TARGET_VIEW_DESC vd; static_cast<ID3D11RenderTargetView*>(view)->GetDesc(&vd);
        if (vd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) mip = vd.Texture2D.MipSlice;
        else if (vd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) { mip = vd.Texture2DArray.MipSlice; slice = vd.Texture2DArray.FirstArraySlice; }
    }
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) { if (g_cap_index) fprintf(g_cap_index, "  %s: not a 2D texture\n", tag); res->Release(); return; }
    ID3D11Texture2D* tex = static_cast<ID3D11Texture2D*>(res);
    D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
    UINT w = d.Width >> mip, h = d.Height >> mip; if (!w) w = 1; if (!h) h = 1;
    if (g_cap_index) fprintf(g_cap_index, "  %s: %ux%u fmt %u mips %u array %u samples %u mip %u slice %u bind %X\n",
                             tag, w, h, (unsigned)d.Format, d.MipLevels, d.ArraySize, d.SampleDesc.Count, mip, slice, d.BindFlags);
    if (d.SampleDesc.Count > 1) { res->Release(); return; }
    if (g_cap_defer) {
        if (!depth && g_cap_defer_on && (int)w >= g_cap_defer_minw) {
            D3D11_TEXTURE2D_DESC cd = d; cd.Width = w; cd.Height = h; cd.MipLevels = 1; cd.ArraySize = 1;
            cd.Usage = D3D11_USAGE_DEFAULT; cd.BindFlags = D3D11_BIND_SHADER_RESOURCE; cd.CPUAccessFlags = 0; cd.MiscFlags = 0;
            ID3D11Device* dev0 = nullptr; c->GetDevice(&dev0);
            ID3D11Texture2D* copy = nullptr;
            if (dev0 && SUCCEEDED(dev0->CreateTexture2D(&cd, nullptr, &copy)) && copy) {
                c->CopySubresourceRegion(copy, 0, 0, 0, 0, tex, mip + slice * d.MipLevels, nullptr);
                CapDeferred q{}; q.tex = copy; q.pass = g_cap_pass; snprintf(q.tag, sizeof q.tag, "%s", tag); q.w = w; q.h = h; q.fmt = d.Format;
                g_cap_deferred.push_back(q);
            }
            if (dev0) dev0->Release();
        }
        res->Release();
        return;
    }
    D3D11_TEXTURE2D_DESC sd = d; sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev);
    ID3D11Texture2D* st = nullptr;
    HRESULT hr = dev->CreateTexture2D(&sd, nullptr, &st);
    if (SUCCEEDED(hr) && st) {
        c->CopySubresourceRegion(st, 0, 0, 0, 0, tex, mip + slice * d.MipLevels, nullptr);
        D3D11_MAPPED_SUBRESOURCE m{};
        hr = c->Map(st, 0, D3D11_MAP_READ, 0, &m);
        if (SUCCEEDED(hr) && m.pData) {
            UINT bpp = m.RowPitch / w; if (!bpp) bpp = 1;
            UINT step = w > 480 ? (w + 479) / 480 : 1;
            UINT ow = (w + step - 1) / step, oh = (h + step - 1) / step;
            char path[640]; snprintf(path, sizeof path, "%s/cap_%03d_%s.bin", g_cap_dir, g_cap_pass, tag);
            if (FILE* f = fopen(path, "wb")) {
                fprintf(f, "TMCAP %u %u %u %u\n", ow, oh, bpp, (unsigned)d.Format);
                std::vector<uint8_t> row(ow * bpp);
                for (UINT y = 0; y < h; y += step) {
                    const uint8_t* src = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                    for (UINT x = 0, o = 0; x < w; x += step, o++) memcpy(&row[o * bpp], src + (size_t)x * bpp, bpp);
                    fwrite(row.data(), 1, row.size(), f);
                }
                fclose(f);
            }
            c->Unmap(st, 0);
        } else if (g_cap_index) fprintf(g_cap_index, "  %s: Map failed %08X\n", tag, (unsigned)hr);
        st->Release();
    } else if (g_cap_index) fprintf(g_cap_index, "  %s: staging create failed %08X\n", tag, (unsigned)hr);
    if (dev) dev->Release();
    res->Release();
}
// The game is about to bind other targets (or present): dump the ones it leaves.
// TM_GFX_SYNC (diagnostics): at every render-target change, 1 = Flush (each pass gets its
// own submission), 2 = Flush and wait until the GPU has executed everything so far,
// 3 = only a 0.3 ms delay with the machine lock held, 4 = the same delay with it released.
extern "C" uint32_t tm_gfx_present_count(void);
static inline uint32_t g_present_count_now(void) { return tm_gfx_present_count(); }
static int g_rp_split = -1;                        // TM_RP_SPLIT, see rp_split()
static uint32_t g_rp_splits;                       // splits since the last statistics line
static uint32_t g_rp_work;                         // draws, clears and copies recorded since the last split or Present
static int g_gfx_sync = -1;
static int g_sync_lo = 0, g_sync_hi = 1 << 30;      // only render-target changes lo..hi of a frame (0 = first after Present)
static int g_pass_in_frame, g_passes_last_frame;
static int g_upd_mode;                              // debug.tm.gfxupd, see ctx_UpdateSubresource
static int g_fwatch = -1;                           // TM_FLOAT_WATCH / debug.tm.fwatch, see fwatch_draw
static int g_query_flush = -1;                      // TM_QUERY_FLUSH / debug.tm.queryflush, see ctx_GetData
static uint32_t g_draws_this_pass, g_sync_big_seen;
static uint32_t g_clears_this_pass, g_out_fmt, g_out_ncolour;   // the pass being left: colour clears, format of target 0, colour target count
static uint32_t g_sync_flushes;                                 // flushes issued by the sync modes since the last statistics line
static int g_rel = -1000;                                       // passes since the frame's single-target R16G16F pass (0 = that pass)
// ---- one-frame call trace (F10): which resource data moves in which pass ----------
// Logs every UpdateSubresource / Copy* / GenerateMips / unusual Map with the index of
// the render-target change it follows, and per pass the draw and constant-buffer map
// counts. No readbacks and no waits, so the frame is timed like any other.
static int g_trace;                                   // 0 off, 1 armed (starts at the next Present), 2 recording
static uint32_t g_trace_draws, g_trace_cbmaps, g_trace_cbbytes;
extern "C" void tm_gfx_trace_request(void) { if (!g_trace) g_trace = 1; }
static void trace_res(char* out, size_t cap, ID3D11Resource* r) {
    if (!r) { snprintf(out, cap, "null"); return; }
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN; r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) { D3D11_BUFFER_DESC d{}; static_cast<ID3D11Buffer*>(r)->GetDesc(&d); snprintf(out, cap, "buffer %u B usage %u bind %X", d.ByteWidth, d.Usage, d.BindFlags); }
    else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { D3D11_TEXTURE2D_DESC d{}; static_cast<ID3D11Texture2D*>(r)->GetDesc(&d); snprintf(out, cap, "tex2d %ux%u fmt %u mips %u usage %u bind %X", d.Width, d.Height, d.Format, d.MipLevels, d.Usage, d.BindFlags); }
    else snprintf(out, cap, "resource dim %d", (int)dim);
}
#define TRACE(...) do { if (g_trace == 2) TM_INFO("trace p%02d " __VA_ARGS__); } while (0)
static void trace_pass_end(void) {
    if (g_trace != 2) return;
    TM_INFO("trace p%02d   = %u draws, %u small buffer maps (%u bytes)", g_pass_in_frame - 1, g_trace_draws, g_trace_cbmaps, g_trace_cbbytes);
    g_trace_draws = g_trace_cbmaps = g_trace_cbbytes = 0;
}
// Once per frame: "setprop debug.tm.gfxsync '<mode> [lo hi]'" changes the mode and the
// window of render-target changes it applies to while the game runs (bisecting).
static void gfx_sync_frame(void) {
#ifdef __ANDROID__
    static char last[PROP_VALUE_MAX];
    char v[PROP_VALUE_MAX] = "";
    if (__system_property_get("debug.tm.gfxsync", v) > 0 && strcmp(v, last)) {
        int m = 0, lo = 0, hi = 1 << 30;
        int n = sscanf(v, "%d %d %d", &m, &lo, &hi);
        if (n >= 1) { g_gfx_sync = m; g_sync_lo = n >= 2 ? lo : 0; g_sync_hi = n >= 3 ? hi : 1 << 30; }
        strcpy(last, v);
        TM_INFO("TM_GFX_SYNC mode %d for render-target changes %d..%d (%d per frame)", g_gfx_sync, g_sync_lo, g_sync_hi, g_passes_last_frame);
    }
    static char lastr[PROP_VALUE_MAX];
    char rv[PROP_VALUE_MAX] = "";
    if (__system_property_get("debug.tm.rpsplit", rv) > 0 && strcmp(rv, lastr)) { g_rp_split = atoi(rv); strcpy(lastr, rv); TM_INFO("TM_RP_SPLIT is now %d", g_rp_split); }
    static char lastu[PROP_VALUE_MAX];
    char u[PROP_VALUE_MAX] = "";
    if (__system_property_get("debug.tm.gfxupd", u) > 0 && strcmp(u, lastu)) { g_upd_mode = atoi(u); strcpy(lastu, u); TM_INFO("buffer update mode %d", g_upd_mode); }
    static char lasti[PROP_VALUE_MAX];
    char iv[PROP_VALUE_MAX] = "";
    if (__system_property_get("debug.tm.implflush", iv) > 0 && strcmp(iv, lasti)) { g_tm_dxvk_implicit_flush = atoi(iv); strcpy(lasti, iv); TM_INFO("DXVK implicit flush is now %d", g_tm_dxvk_implicit_flush); }
    static char lastf[PROP_VALUE_MAX];
    char fv[PROP_VALUE_MAX] = "";
    if (__system_property_get("debug.tm.queryflush", fv) > 0 && strcmp(fv, lastf)) { g_query_flush = atoi(fv); strcpy(lastf, fv); TM_INFO("flush on a pending query: mode %d", g_query_flush); }
    static char lastw[PROP_VALUE_MAX];
    char w[PROP_VALUE_MAX] = "";
    if (__system_property_get("debug.tm.fwatch", w) > 0 && strcmp(w, lastw)) { g_fwatch = atoi(w); strcpy(lastw, w); TM_INFO("float watch mode %d", g_fwatch); }
#endif
    if (g_trace == 2) { trace_pass_end(); TM_INFO("trace end (%d render-target changes)", g_pass_in_frame); g_trace = 0; }
    else if (g_trace == 1) { g_trace = 2; g_trace_draws = g_trace_cbmaps = g_trace_cbbytes = 0; TM_INFO("trace begin, frame %u", tm_gfx_present_count()); }
    g_passes_last_frame = g_pass_in_frame; g_pass_in_frame = 0; g_sync_big_seen = 0; g_rel = -1000;
}
// Structure-relative modes (the absolute index of a pass shifts from frame to frame):
//   5 = Flush only where the game sets the SAME targets again (inside a chain of passes
//       that DXVK keeps as one Vulkan render pass), 6 = only where the targets really change,
//   7 = Flush before the first DISCARD map of a large dynamic index buffer,
//   8 = Flush at the first same-target change after a pass with more than 100 draws,
//   9 = Flush when leaving a "pass" that only cleared a colour target (no draws),
//  10 = Flush when leaving a pass whose only colour target has DXGI format <lo>,
//  11 = Flush when leaving a pass that drew into more than one colour target,
//  12 = Flush when leaving pass number lo..hi counted from the single-target R16G16F pass (= 0),
//  13 = Flush when leaving an R11G11B10-only pass that comes before that pass,
//  14 = at every real target change copy one texel between two scratch textures (a transfer
//       command between the render passes, no submission),
//  15 = Flush when the new pass has more than one colour target and the old one had a different set,
//  16 = like 15 with the scratch copy instead of the Flush.
static void gfx_scratch_copy(ID3D11DeviceContext* c) {
    static ID3D11Texture2D* a; static ID3D11Texture2D* b;
    if (!a) {
        ID3D11Device* dev = nullptr; c->GetDevice(&dev);
        D3D11_TEXTURE2D_DESC d{}; d.Width = d.Height = 4; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (dev) { dev->CreateTexture2D(&d, nullptr, &a); dev->CreateTexture2D(&d, nullptr, &b); dev->Release(); }
    }
    if (a && b) c->CopySubresourceRegion(b, 0, 0, 0, 0, a, 0, nullptr);
}
static uint32_t g_new_ncolour;      // colour targets of the pass that is about to start (set by the callers)
static void gfx_sync_point(ID3D11DeviceContext* c, bool same_targets = false) {
    if (g_gfx_sync < 0) { const char* e = getenv("TM_GFX_SYNC"); g_gfx_sync = e && *e ? atoi(e) : 0; }
    int idx = g_pass_in_frame++;
    uint32_t draws = g_draws_this_pass; g_draws_this_pass = 0;
    uint32_t clears = g_clears_this_pass; g_clears_this_pass = 0;
    if (g_trace == 2) TM_INFO("trace p%02d   (%s)", idx, same_targets ? "same targets set again" : "targets change");
    trace_pass_end();
    if (g_gfx_sync >= 5) {
        bool go = false;
        if (g_gfx_sync == 5) go = same_targets;
        else if (g_gfx_sync == 6) go = !same_targets;
        else if (g_gfx_sync == 8) { if (draws > 100) g_sync_big_seen = 1; if (g_sync_big_seen == 1 && same_targets) { go = true; g_sync_big_seen = 2; } }
        else if (g_gfx_sync == 9) go = clears > 0 && draws == 0;
        else if (g_gfx_sync == 10) go = g_out_ncolour == 1 && g_out_fmt == (uint32_t)g_sync_lo && draws > 0;
        else if (g_gfx_sync == 11) go = g_out_ncolour > 1 && draws > 0;
        else if (g_gfx_sync == 12) go = g_rel >= g_sync_lo && g_rel <= g_sync_hi;          // leaving the lo..hi-th pass after the R16G16F pass
        else if (g_gfx_sync == 13) go = g_rel < 0 && g_out_ncolour == 1 && g_out_fmt == 26 && draws > 0;
        else if (g_gfx_sync == 14) { if (!same_targets) gfx_scratch_copy(c); }
        else if (g_gfx_sync == 15) go = !same_targets && g_new_ncolour > 1;
        else if (g_gfx_sync == 16) { if (!same_targets && g_new_ncolour > 1) gfx_scratch_copy(c); }
        if (go) { c->Flush(); g_sync_flushes++; }
        return;
    }
    if (!g_gfx_sync || idx < g_sync_lo || idx > g_sync_hi) return;
    if (g_gfx_sync == 1) { c->Flush(); return; }
    if (g_gfx_sync == 3) { struct timespec ts = {0, 300000}; nanosleep(&ts, nullptr); return; }                                         // delay only, machine held
    if (g_gfx_sync == 4) { tm_machine_unlock(); struct timespec ts = {0, 300000}; nanosleep(&ts, nullptr); tm_machine_lock(); return; }   // delay, others may run
    static ID3D11Query* ev;
    if (!ev) { ID3D11Device* dev = nullptr; c->GetDevice(&dev); D3D11_QUERY_DESC qd{ D3D11_QUERY_EVENT, 0 }; if (dev) { dev->CreateQuery(&qd, &ev); dev->Release(); } }
    if (!ev) { c->Flush(); return; }
    c->End(ev);
    BOOL done = FALSE;
    for (int i = 0; i < 20000 && c->GetData(ev, &done, sizeof done, 0) != S_OK; i++) { struct timespec ts = {0, 50000}; nanosleep(&ts, nullptr); }
}
// F9 (input.c) steps through the TM_GFX_SYNC modes while the game runs.
extern "C" void tm_gfx_debug_cycle(void) {
    if (g_gfx_sync < 0) g_gfx_sync = 0;
    g_gfx_sync = (g_gfx_sync + 1) % 5;
    TM_INFO("TM_GFX_SYNC mode is now %d (frame %u)", g_gfx_sync, tm_gfx_present_count());
}
void cap_leave_pass(ID3D11DeviceContext* c) {
    if (!g_cap_active) return;
    if (g_cap_index) {
        UINT dsflags = 0; DXGI_FORMAT dsfmt = DXGI_FORMAT_UNKNOWN;
        if (g_cap_dsv) { D3D11_DEPTH_STENCIL_VIEW_DESC vd; g_cap_dsv->GetDesc(&vd); dsflags = vd.Flags; dsfmt = vd.Format; }
        fprintf(g_cap_index, "pass %d: draws %d, %u color target(s)", g_cap_pass, g_cap_draws, g_cap_nrt);
        if (g_cap_dsv) fprintf(g_cap_index, " + depth (view fmt %u, read-only flags %u)", (unsigned)dsfmt, dsflags);
        fprintf(g_cap_index, "\n");
        if (g_cap_draws > 0) fputs(g_cap_last.c_str(), g_cap_index);
    }
    if (g_cap_draws > 0) {
        std::lock_guard<std::mutex> l(g_reg_mu);
        void* sh[2] = { g_cap_vs, g_cap_ps }; const char* nm[2] = { "vs", "ps" };
        for (int i = 0; i < 2; i++) {
            auto it = g_shader_code.find(sh[i]);
            if (it == g_shader_code.end()) continue;
            char path[640]; snprintf(path, sizeof path, "%s/cap_%03d_%s.dxbc", g_cap_dir, g_cap_pass, nm[i]);
            if (FILE* f = fopen(path, "wb")) { fwrite(it->second.data(), 1, it->second.size(), f); fclose(f); }
        }
    }
    if (g_cap_defer && !g_cap_defer_on && g_cap_defer_left > 0 && g_cap_nrt && g_cap_rtv[0]) {
        int from_fmt = g_cap_defer_fmt;
        ID3D11Resource* r0 = nullptr; g_cap_rtv[0]->GetResource(&r0);
        D3D11_RESOURCE_DIMENSION dim0 = D3D11_RESOURCE_DIMENSION_UNKNOWN; if (r0) r0->GetType(&dim0);
        if (dim0 == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { D3D11_TEXTURE2D_DESC d0{}; static_cast<ID3D11Texture2D*>(r0)->GetDesc(&d0); if ((int)d0.Format == from_fmt || from_fmt == 0) g_cap_defer_on = 1; }
        if (r0) r0->Release();
    }
    if (g_cap_draws > 0 || (g_cap_defer && g_cap_defer_on)) {
        for (UINT i = 0; i < g_cap_nrt; i++) if (g_cap_rtv[i]) { char tag[16]; snprintf(tag, sizeof tag, "rt%u", i); cap_write(c, g_cap_rtv[i], false, tag); }
        if (g_cap_dsv) cap_write(c, g_cap_dsv, true, "ds");
    }
    if (g_cap_defer && g_cap_defer_on && --g_cap_defer_left <= 0) g_cap_defer_on = 0;
    for (auto& r : g_cap_rtv) if (r) { r->Release(); r = nullptr; }
    if (g_cap_dsv) { g_cap_dsv->Release(); g_cap_dsv = nullptr; }
    g_cap_nrt = 0; g_cap_draws = 0; g_cap_pass++;
}
void cap_enter_pass(UINT n, ID3D11RenderTargetView** rt, ID3D11DepthStencilView* ds) {
    if (!g_cap_active) return;
    g_cap_nrt = n > 8 ? 8 : n;
    for (UINT i = 0; i < g_cap_nrt; i++) { g_cap_rtv[i] = rt ? rt[i] : nullptr; if (g_cap_rtv[i]) g_cap_rtv[i]->AddRef(); }
    g_cap_dsv = ds; if (ds) ds->AddRef();
}
void cap_frame_boundary(ID3D11DeviceContext* c) {
    if (g_cap_active) {
        if (c) cap_leave_pass(c);
        if (c && g_cap_defer) {
            for (auto& q : g_cap_deferred) { cap_dump_tex(c, q.tex, 0, q.w, q.h, q.fmt, q.pass, q.tag); q.tex->Release(); }
            if (g_cap_index) fprintf(g_cap_index, "deferred capture: %u targets read back after the frame\n", (unsigned)g_cap_deferred.size());
            g_cap_deferred.clear();
        }
        if (g_cap_index) { fclose(g_cap_index); g_cap_index = nullptr; }
        TM_INFO("frame capture: %d passes written to %s", g_cap_pass, g_cap_dir);
        g_cap_active = 0;
    } else if (g_cap_req.exchange(0)) {
        const char* d = getenv("TM_CAPTURE_DIR"); const char* u = getenv("DXVK_STATE_CACHE_PATH");
        if (d) snprintf(g_cap_dir, sizeof g_cap_dir, "%s", d); else snprintf(g_cap_dir, sizeof g_cap_dir, "%s/cap", u ? u : "work");
        mkdir(g_cap_dir, 0775);
        char p[640]; snprintf(p, sizeof p, "%s/cap_index.txt", g_cap_dir);
        g_cap_index = fopen(p, "w");
        g_cap_pass = 0; g_cap_draws = 0; g_cap_active = 1;
        { const char* e = getenv("TM_CAPTURE_DEFER"); g_cap_defer = g_cap_defer_req || (e && *e == '1'); g_cap_defer_req = 0; g_cap_defer_on = 0;
          g_cap_defer_fmt = 34; g_cap_defer_left = 16; g_cap_defer_minw = 640;
#ifdef __ANDROID__
          char v[PROP_VALUE_MAX] = "";      // setprop debug.tm.capdefer "<first format, 0 = any> <passes> [min width]"
          if (__system_property_get("debug.tm.capdefer", v) > 0) sscanf(v, "%d %d %d", &g_cap_defer_fmt, &g_cap_defer_left, &g_cap_defer_minw);
#endif
        }
        TM_INFO("frame capture: recording the next frame into %s", g_cap_dir);
    }
}
ID3D11DeviceContext* g_cap_ctx = nullptr;   // the immediate context, noted when the game uses it
}
extern "C" void tm_gfx_capture_request(void) { g_cap_req = 1; }
extern "C" void tm_gfx_capture_deferred_request(void) { g_cap_defer_req = 1; g_cap_req = 1; }
// State of the draw that is about to be issued (kept for the pass's last draw):
// sampled textures, blend/depth state, viewport.
static void cap_snapshot(ID3D11DeviceContext* c, const char* what, uint32_t a0, uint32_t a1, uint32_t a2) {
    char b[400]; g_cap_last.clear();
    snprintf(b, sizeof b, "  last draw: %s(%u, %u, %u)\n", what, a0, a1, a2); g_cap_last += b;
    {   // input assembler
        D3D11_PRIMITIVE_TOPOLOGY topo; c->IAGetPrimitiveTopology(&topo);
        ID3D11InputLayout* il = nullptr; c->IAGetInputLayout(&il);
        std::string ld = "(none)";
        if (il) { std::lock_guard<std::mutex> l(g_reg_mu); auto it = g_layout_desc.find(il); if (it != g_layout_desc.end()) ld = it->second; else ld = "(unknown)"; }
        snprintf(b, sizeof b, "  ia: topology %d layout %p:", (int)topo, (void*)il); g_cap_last += b; g_cap_last += ld; g_cap_last += "\n";
        if (il) il->Release();
        ID3D11Buffer* vb[4] = {}; UINT st[4] = {}, of[4] = {};
        c->IAGetVertexBuffers(0, 4, vb, st, of);
        for (int i = 0; i < 4; i++) if (vb[i]) { D3D11_BUFFER_DESC bd; vb[i]->GetDesc(&bd);
            snprintf(b, sizeof b, "  vb%d: stride %u offset %u size %u usage %d\n", i, st[i], of[i], bd.ByteWidth, (int)bd.Usage); g_cap_last += b; vb[i]->Release(); }
    }
    ID3D11ShaderResourceView* srv[16] = {};
    c->PSGetShaderResources(0, 16, srv);
    for (int i = 0; i < 16; i++) {
        if (!srv[i]) continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd; srv[i]->GetDesc(&sd);
        ID3D11Resource* r = nullptr; srv[i]->GetResource(&r);
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN; if (r) r->GetType(&dim);
        if (r && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC td; static_cast<ID3D11Texture2D*>(r)->GetDesc(&td);
            bool is_ds = false; if (g_cap_dsv) { ID3D11Resource* dr = nullptr; g_cap_dsv->GetResource(&dr); is_ds = dr == r; if (dr) dr->Release(); }
            bool is_rt = false; for (UINT k = 0; k < g_cap_nrt; k++) if (g_cap_rtv[k]) { ID3D11Resource* rr = nullptr; g_cap_rtv[k]->GetResource(&rr); if (rr == r) is_rt = true; if (rr) rr->Release(); }
            snprintf(b, sizeof b, "  ps srv%d: %ux%u tex fmt %u view fmt %u mips %u samples %u bind %X%s%s\n", i, td.Width, td.Height, (unsigned)td.Format,
                     (unsigned)sd.Format, td.MipLevels, td.SampleDesc.Count, td.BindFlags, is_ds ? "  <== the bound depth buffer" : "", is_rt ? "  <== a bound render target" : "");
        } else snprintf(b, sizeof b, "  ps srv%d: dim %d view fmt %u\n", i, (int)dim, (unsigned)sd.Format);
        g_cap_last += b;
        if (r) r->Release();
        srv[i]->Release();
    }
    if (g_cap_draws <= 2 && !g_cap_defer) {   // constant buffers as the GPU side has them (first draws of a pass only: this is slow)
        ID3D11Buffer* cb[14] = {};
        c->PSGetConstantBuffers(0, 14, cb);
        ID3D11Device* dev = nullptr; c->GetDevice(&dev);
        for (int i = 0; i < 14; i++) {
            if (!cb[i]) continue;
            D3D11_BUFFER_DESC bd; cb[i]->GetDesc(&bd);
            snprintf(b, sizeof b, "  ps cb%d: %u bytes usage %d cpu %X :", i, bd.ByteWidth, (int)bd.Usage, bd.CPUAccessFlags); g_cap_last += b;
            D3D11_BUFFER_DESC sd = bd; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
            ID3D11Buffer* st = nullptr;
            if (dev && SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &st)) && st) {
                c->CopyResource(st, cb[i]);
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(c->Map(st, 0, D3D11_MAP_READ, 0, &m)) && m.pData) {
                    UINT n = bd.ByteWidth / 4; if (n > 32) n = 32;
                    for (UINT k = 0; k < n; k++) { snprintf(b, sizeof b, " %.5g", ((const float*)m.pData)[k]); g_cap_last += b; }
                    c->Unmap(st, 0);
                } else g_cap_last += " (map failed)";
                st->Release();
            }
            g_cap_last += "\n";
            cb[i]->Release();
        }
        if (dev) dev->Release();
    }
    ID3D11BlendState* bs = nullptr; FLOAT bf[4]; UINT mask = 0;
    c->OMGetBlendState(&bs, bf, &mask);
    if (bs) {
        D3D11_BLEND_DESC bd; bs->GetDesc(&bd);
        snprintf(b, sizeof b, "  blend: independent %d a2c %d", bd.IndependentBlendEnable, bd.AlphaToCoverageEnable); g_cap_last += b;
        for (UINT i = 0; i < (bd.IndependentBlendEnable ? 4u : 1u); i++) {
            auto& t = bd.RenderTarget[i];
            snprintf(b, sizeof b, " | rt%u en %d src %d dst %d op %d srcA %d dstA %d opA %d mask %X", i, t.BlendEnable, t.SrcBlend, t.DestBlend, t.BlendOp,
                     t.SrcBlendAlpha, t.DestBlendAlpha, t.BlendOpAlpha, t.RenderTargetWriteMask); g_cap_last += b;
        }
        g_cap_last += "\n"; bs->Release();
    } else g_cap_last += "  blend: default (off)\n";
    ID3D11DepthStencilState* dss = nullptr; UINT sref = 0;
    c->OMGetDepthStencilState(&dss, &sref);
    if (dss) { D3D11_DEPTH_STENCIL_DESC dd; dss->GetDesc(&dd);
        snprintf(b, sizeof b, "  depth: test %d write %d func %d stencil %d ref %u\n", dd.DepthEnable, dd.DepthWriteMask, dd.DepthFunc, dd.StencilEnable, sref); g_cap_last += b; dss->Release(); }
    D3D11_VIEWPORT vp[4]; UINT nvp = 4; c->RSGetViewports(&nvp, vp);
    if (nvp) { snprintf(b, sizeof b, "  viewport: %g,%g %gx%g depth %g..%g\n", vp[0].TopLeftX, vp[0].TopLeftY, vp[0].Width, vp[0].Height, vp[0].MinDepth, vp[0].MaxDepth); g_cap_last += b; }
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; ID3D11GeometryShader* gs = nullptr;
    c->VSGetShader(&vs, nullptr, nullptr); c->PSGetShader(&ps, nullptr, nullptr); c->GSGetShader(&gs, nullptr, nullptr);
    snprintf(b, sizeof b, "  shaders: vs %p ps %p gs %p\n", (void*)vs, (void*)ps, (void*)gs); g_cap_last += b;
    g_cap_vs = vs; g_cap_ps = ps;
    if (vs) vs->Release(); if (ps) ps->Release(); if (gs) gs->Release();
}
// TM_CAPTURE_MARK=1: before the first draw of a pass that is a single non-blended
// full-screen triangle, fill its target with a marker colour. If the marker is
// still there afterwards, the draw produced no fragments at all.
static void cap_mark(ID3D11DeviceContext* c, const char* what, uint32_t count) {
    static int on = -1; if (on < 0) { const char* e = getenv("TM_CAPTURE_MARK"); on = e && *e == '1'; }
    if (!on || g_cap_defer || g_cap_draws != 1 || strcmp(what, "Draw") || count != 3 || !g_cap_rtv[0]) return;
    ID3D11BlendState* bs = nullptr; FLOAT bf[4]; UINT mask = 0; c->OMGetBlendState(&bs, bf, &mask);
    bool blend = false; if (bs) { D3D11_BLEND_DESC bd; bs->GetDesc(&bd); blend = bd.RenderTarget[0].BlendEnable; bs->Release(); }
    if (blend) return;
    const FLOAT marker[4] = { 7.0f, 0.0f, 7.0f, 7.0f };
    c->ClearRenderTargetView(g_cap_rtv[0], marker);
    g_cap_last += "  (target pre-filled with marker 7,0,7,7)\n";
}
// Guest constant buffers bound to the vertex and pixel stage (for the sprite probe below).
static uint32_t g_cb_vs[14], g_cb_ps[14];
// TM_SPRITE_LOG=1 (diagnostics): for additive draws with the engine's self-illuminated
// sprite shader (PS cb0 = 16 bytes "SelfIllumScale", VS cb0 = 48 bytes "GbxVisualToWorld")
// log once per frame how many there were and the constants of the brightest one.
static void sprite_probe(ID3D11DeviceContext* c, uint32_t index_count);
static int g_sprite_log = -1;
#ifndef TM_TIMESTAMP_MODE_DEFAULT
#define TM_TIMESTAMP_MODE_DEFAULT 0
#endif
static uint32_t g_stat_draws;   // draw calls since the last statistics line (see stats_frame)

// ---- input-assembler state mirror, for the lazy flush of big dynamic buffers (see "Map staging") ----
struct IASlot { uint32_t buf = 0, stride = 0, offset = 0; };
static IASlot g_ia_vb[32];
static uint32_t g_ia_ib, g_ia_ib_fmt, g_ia_ib_off, g_ia_layout;
struct LayoutInfo { uint32_t inst_mask = 0; uint32_t step[32] = {}; };       // per-instance slots and their step rates
static std::unordered_map<uint32_t, LayoutInfo> g_layout_info;                // guest input layout -> slot classes
static int g_lazy_count;                                                      // dynamic buffers flushed at draw time
// Static index buffers cannot be read back, and a draw from a lazily flushed vertex
// buffer needs the index values to know which vertices it uses (the engine draws its
// dynamic vertices with fixed quad/fan index patterns). So index buffers created with
// initial data keep a host copy of it, up to IB_SHADOW_MAX bytes each.
static std::unordered_map<uint32_t, std::vector<uint8_t>> g_ib_shadow;        // guest index buffer -> its data
static uint64_t g_ib_shadow_bytes;
static const uint32_t IB_SHADOW_MAX = 2u << 20;
static inline void ib_shadow_drop(uint32_t res) {
    if (g_ib_shadow.empty()) return;
    auto it = g_ib_shadow.find(res);
    if (it != g_ib_shadow.end()) { g_ib_shadow_bytes -= it->second.size(); g_ib_shadow.erase(it); }
}
enum { LAZY_DRAW, LAZY_INDEXED, LAZY_UNKNOWN };
static void lazy_pre_draw_slow(int kind, uint32_t count, uint32_t start, int32_t base_vertex, uint32_t inst_count, uint32_t start_inst);
static inline void lazy_pre_draw(int kind, uint32_t count, uint32_t start, int32_t base_vertex, uint32_t inst_count, uint32_t start_inst) {
    if (g_lazy_count) lazy_pre_draw_slow(kind, count, start, base_vertex, inst_count, start_inst);
}
static void lazy_flush_all(uint32_t res);                                     // a buffer is about to be read as a whole
// Trace only: does this draw sample a texture that is one of its own render targets?
// (Legal D3D11 never does; DXVK unbinds such views. On a tile-based GPU a remaining
// overlap reads whatever the tile memory holds.)
static void trace_feedback(ID3D11DeviceContext* c, const char* what) {
    ID3D11RenderTargetView* rt[8] = {}; ID3D11DepthStencilView* ds = nullptr;
    c->OMGetRenderTargets(8, rt, &ds);
    ID3D11Resource* target[9] = {}; int nt = 0; bool ds_ro = false;
    for (int i = 0; i < 8; i++) if (rt[i]) { rt[i]->GetResource(&target[nt++]); rt[i]->Release(); }
    int ds_index = -1;
    if (ds) { D3D11_DEPTH_STENCIL_VIEW_DESC dd; ds->GetDesc(&dd); ds_ro = dd.Flags != 0; ds_index = nt; ds->GetResource(&target[nt++]); ds->Release(); }
    for (int stage = 0; stage < 2; stage++) {
        ID3D11ShaderResourceView* sv[16] = {};
        if (stage == 0) c->PSGetShaderResources(0, 16, sv); else c->VSGetShaderResources(0, 16, sv);
        for (int i = 0; i < 16; i++) {
            if (!sv[i]) continue;
            ID3D11Resource* r = nullptr; sv[i]->GetResource(&r);
            for (int t = 0; t < nt; t++) if (r && r == target[t]) {
                char d[160]; trace_res(d, sizeof d, r);
                TRACE("FEEDBACK %s draw %u: %s slot %d samples %s %d%s: %s", g_pass_in_frame, what, g_trace_draws, stage ? "VS" : "PS", i,
                      t == ds_index ? "the depth target" : "colour target", t, t == ds_index && ds_ro ? " (read-only view)" : "", d);
            }
            if (r) r->Release();
            sv[i]->Release();
        }
    }
    for (int t = 0; t < nt; t++) if (target[t]) target[t]->Release();
}
static void fwatch_draw(ID3D11DeviceContext* c, const char* what, uint32_t a0, uint32_t a1, uint32_t a2);
static uint32_t g_frame_draws;     // draws since the last Present (TM_FRAME_LOG)
#define CAP_DRAW(what) do { g_stat_draws++; g_trace_draws++; g_draws_this_pass++; g_rp_work++; g_frame_draws++; if (g_fwatch) fwatch_draw(N<ID3D11DeviceContext>(ARG(0)), what, ARG(1), ARG(2), ARG(3)); if (g_trace == 2) trace_feedback(N<ID3D11DeviceContext>(ARG(0)), what); if (g_cap_active) { g_cap_draws++; cap_snapshot(N<ID3D11DeviceContext>(ARG(0)), what, ARG(1), ARG(2), ARG(3)); cap_mark(N<ID3D11DeviceContext>(ARG(0)), what, ARG(1)); } } while (0)

// ================================================================= Device
// A failed creation is rare and usually fatal for the game a moment later: say why.
static uint32_t g_frame_created;   // objects created since the last Present (TM_FRAME_LOG)
static HRESULT created(const char* what, HRESULT hr, uint32_t descVa, int words) {
    g_frame_created++;
    static int budget = 40;
    if (FAILED(hr) && budget > 0) {
        budget--;
        char line[256]; int n = 0;
        for (int i = 0; i < words && descVa; i++) n += snprintf(line + n, sizeof line - n, " %X", rd32(descVa + 4 * i));
        TM_WARN("D3D11 %s failed hr=%08X desc:%s (from guest %08X)", what, (uint32_t)hr, line, g_cur_func);
    }
    return hr;
}
static void dev_CreateBuffer(void) {
    auto* d = N<ID3D11Device>(ARG(0));
    D3D11_BUFFER_DESC* desc = ARG(1) ? (D3D11_BUFFER_DESC*)G2H(ARG(1)) : nullptr;
    D3D11_SUBRESOURCE_DATA sd{}; D3D11_SUBRESOURCE_DATA* psd = nullptr;
    if (ARG(2)) { uint32_t g = ARG(2); sd.pSysMem = G2H(rd32(g)); sd.SysMemPitch = rd32(g + 4); sd.SysMemSlicePitch = rd32(g + 8); psd = &sd; }
    ID3D11Buffer* out = nullptr;
    HRESULT hr = created("CreateBuffer", d->CreateBuffer(desc, psd, ARG(3) ? &out : nullptr), ARG(1), 6);
    uint32_t gbufobj = SUCCEEDED(hr) && out ? wrap(out, C_BUFFER) : 0;
    if (gbufobj) {
        ib_shadow_drop(gbufobj);                   // the id may have belonged to an earlier buffer
        if (desc && psd && sd.pSysMem && (desc->BindFlags & D3D11_BIND_INDEX_BUFFER) && desc->ByteWidth && desc->ByteWidth <= IB_SHADOW_MAX) {
            const uint8_t* p0 = (const uint8_t*)sd.pSysMem;
            g_ib_shadow[gbufobj].assign(p0, p0 + desc->ByteWidth);
            g_ib_shadow_bytes += desc->ByteWidth;
        }
    }
    if (ARG(3)) wr32(ARG(3), gbufobj);
    RET((uint32_t)hr, 4);
}
static D3D11_SUBRESOURCE_DATA* read_subres(uint32_t g, uint32_t n, std::vector<D3D11_SUBRESOURCE_DATA>& v) {
    if (!g || !n) return nullptr;
    v.resize(n);
    for (uint32_t i = 0; i < n; i++) { v[i].pSysMem = G2H(rd32(g + 12 * i)); v[i].SysMemPitch = rd32(g + 12 * i + 4); v[i].SysMemSlicePitch = rd32(g + 12 * i + 8); }
    return v.data();
}
// Number of D3D11_SUBRESOURCE_DATA entries behind pInitialData = mips * array size.
//   D3D11_TEXTURE1D_DESC { Width, MipLevels, ArraySize, ... }
//   D3D11_TEXTURE2D_DESC { Width, Height, MipLevels, ArraySize, ... }
//   D3D11_TEXTURE3D_DESC { Width, Height, Depth, MipLevels, ... }
// MipLevels == 0 asks for the full chain.
static uint32_t tex_subres_count(uint32_t descVa, int dim) {
    uint32_t w = rd32(descVa), h = dim >= 2 ? rd32(descVa + 4) : 1, d = dim == 3 ? rd32(descVa + 8) : 1;
    uint32_t mips = rd32(descVa + (dim == 1 ? 4 : dim == 2 ? 8 : 12));
    uint32_t arr = dim == 1 ? rd32(descVa + 8) : dim == 2 ? rd32(descVa + 12) : 1;
    if (!mips) { uint32_t m = w > h ? w : h; if (d > m) m = d; mips = 1; while (m > 1) { m >>= 1; mips++; } }
    if (!arr) arr = 1;
    return mips * arr;
}
static void dev_CreateTexture1D(void) {
    auto* d = N<ID3D11Device>(ARG(0)); std::vector<D3D11_SUBRESOURCE_DATA> v;
    ID3D11Texture1D* out = nullptr;
    HRESULT hr = created("CreateTexture1D", d->CreateTexture1D((D3D11_TEXTURE1D_DESC*)G2H(ARG(1)), read_subres(ARG(2), ARG(2) ? tex_subres_count(ARG(1), 1) : 0, v), ARG(3) ? &out : nullptr), ARG(1), 9);
    if (ARG(3)) wr32(ARG(3), SUCCEEDED(hr) ? wrap(out, C_TEX1D) : 0);
    RET((uint32_t)hr, 4);
}
static void dev_CreateTexture2D(void) {
    auto* d = N<ID3D11Device>(ARG(0)); std::vector<D3D11_SUBRESOURCE_DATA> v;
    ID3D11Texture2D* out = nullptr;
    HRESULT hr = created("CreateTexture2D", d->CreateTexture2D((D3D11_TEXTURE2D_DESC*)G2H(ARG(1)), read_subres(ARG(2), ARG(2) ? tex_subres_count(ARG(1), 2) : 0, v), ARG(3) ? &out : nullptr), ARG(1), 11);
    if (ARG(3)) wr32(ARG(3), SUCCEEDED(hr) ? wrap(out, C_TEX2D) : 0);
    RET((uint32_t)hr, 4);
}
static void dev_CreateTexture3D(void) {
    auto* d = N<ID3D11Device>(ARG(0)); std::vector<D3D11_SUBRESOURCE_DATA> v;
    ID3D11Texture3D* out = nullptr;
    HRESULT hr = created("CreateTexture3D", d->CreateTexture3D((D3D11_TEXTURE3D_DESC*)G2H(ARG(1)), read_subres(ARG(2), ARG(2) ? tex_subres_count(ARG(1), 3) : 0, v), ARG(3) ? &out : nullptr), ARG(1), 9);
    if (ARG(3)) wr32(ARG(3), SUCCEEDED(hr) ? wrap(out, C_TEX3D) : 0);
    RET((uint32_t)hr, 4);
}
#define CREATE_VIEW(fn, METH, DESCT, OUTT, CLS) static void fn(void) { \
    auto* d = N<ID3D11Device>(ARG(0)); ID3D11Resource* res = N<ID3D11Resource>(ARG(1)); \
    OUTT* out = nullptr; \
    HRESULT hr = created(#METH, d->METH(res, ARG(2) ? (DESCT*)G2H(ARG(2)) : nullptr, ARG(3) ? &out : nullptr), ARG(2), 6); \
    if (ARG(3)) wr32(ARG(3), SUCCEEDED(hr) ? wrap(out, CLS) : 0); RET((uint32_t)hr, 4); }
CREATE_VIEW(dev_CreateShaderResourceView, CreateShaderResourceView, D3D11_SHADER_RESOURCE_VIEW_DESC, ID3D11ShaderResourceView, C_SRV)
CREATE_VIEW(dev_CreateRenderTargetView, CreateRenderTargetView, D3D11_RENDER_TARGET_VIEW_DESC, ID3D11RenderTargetView, C_RTV)
CREATE_VIEW(dev_CreateDepthStencilView, CreateDepthStencilView, D3D11_DEPTH_STENCIL_VIEW_DESC, ID3D11DepthStencilView, C_DSV)
CREATE_VIEW(dev_CreateUnorderedAccessView, CreateUnorderedAccessView, D3D11_UNORDERED_ACCESS_VIEW_DESC, ID3D11UnorderedAccessView, C_UAV)

static void dev_CreateInputLayout(void) {
    auto* d = N<ID3D11Device>(ARG(0));
    uint32_t gelem = ARG(1), n = ARG(2);
    std::vector<D3D11_INPUT_ELEMENT_DESC> el(n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = gelem + 28 * i;  // guest D3D11_INPUT_ELEMENT_DESC = 28 bytes (7x u32)
        el[i].SemanticName = (const char*)G2H(rd32(e + 0));
        el[i].SemanticIndex = rd32(e + 4);
        el[i].Format = (DXGI_FORMAT)rd32(e + 8);
        el[i].InputSlot = rd32(e + 12);
        el[i].AlignedByteOffset = rd32(e + 16);
        el[i].InputSlotClass = (D3D11_INPUT_CLASSIFICATION)rd32(e + 20);
        el[i].InstanceDataStepRate = rd32(e + 24);
    }
    ID3D11InputLayout* out = nullptr;
    HRESULT hr = d->CreateInputLayout(el.data(), n, G2H(ARG(3)), ARG(4), ARG(5) ? &out : nullptr);
    if (SUCCEEDED(hr) && out) {
        std::string ld; char b[160];
        for (uint32_t i = 0; i < n; i++) { snprintf(b, sizeof b, " %s%u fmt %d slot %u off %u cls %d step %u;", el[i].SemanticName, el[i].SemanticIndex,
            (int)el[i].Format, el[i].InputSlot, el[i].AlignedByteOffset, (int)el[i].InputSlotClass, el[i].InstanceDataStepRate); ld += b; }
        std::lock_guard<std::mutex> l(g_reg_mu); g_layout_desc[(void*)out] = ld;
    }
    uint32_t glayout = SUCCEEDED(hr) && out ? wrap(out, C_INPUTLAYOUT) : 0;
    if (glayout) {
        LayoutInfo li;
        for (uint32_t i = 0; i < n; i++) if (el[i].InputSlot < 32 && el[i].InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA) {
            li.inst_mask |= 1u << el[i].InputSlot; li.step[el[i].InputSlot] = el[i].InstanceDataStepRate;
        }
        g_layout_info[glayout] = li;
    }
    if (ARG(5)) wr32(ARG(5), glayout);
    RET((uint32_t)hr, 6);
}
#define CREATE_SHADER(fn, METH, OUTT, CLS) static void fn(void) { \
    auto* d = N<ID3D11Device>(ARG(0)); OUTT* out = nullptr; \
    HRESULT hr = created(#METH, d->METH(G2H(ARG(1)), ARG(2), N<ID3D11ClassLinkage>(ARG(3)), ARG(4) ? &out : nullptr), 0, 0); \
    if (SUCCEEDED(hr) && out) { std::lock_guard<std::mutex> l(g_reg_mu); const uint8_t* bc = (const uint8_t*)G2H(ARG(1)); g_shader_code[(void*)out].assign(bc, bc + ARG(2)); } \
    if (ARG(4)) wr32(ARG(4), SUCCEEDED(hr) ? wrap(out, CLS) : 0); RET((uint32_t)hr, 5); }
CREATE_SHADER(dev_CreateVertexShader, CreateVertexShader, ID3D11VertexShader, C_VS)
CREATE_SHADER(dev_CreatePixelShader, CreatePixelShader, ID3D11PixelShader, C_PS)
CREATE_SHADER(dev_CreateComputeShader, CreateComputeShader, ID3D11ComputeShader, C_CS)
CREATE_SHADER(dev_CreateHullShader, CreateHullShader, ID3D11HullShader, C_HS)
CREATE_SHADER(dev_CreateDomainShader, CreateDomainShader, ID3D11DomainShader, C_DS)
static void dev_CreateGeometryShader(void) {
    auto* d = N<ID3D11Device>(ARG(0)); ID3D11GeometryShader* out = nullptr;
    HRESULT hr = d->CreateGeometryShader(G2H(ARG(1)), ARG(2), N<ID3D11ClassLinkage>(ARG(3)), ARG(4) ? &out : nullptr);
    if (ARG(4)) wr32(ARG(4), SUCCEEDED(hr) ? wrap(out, C_GS) : 0);
    RET((uint32_t)hr, 5);
}
static void dev_CreateGeometryShaderWithStreamOutput(void) { if (ARG(9)) wr32(ARG(9), 0); RET(HR_FAIL, 10); }
#define CREATE_STATE(fn, METH, DESCT, OUTT, CLS) static void fn(void) { \
    auto* d = N<ID3D11Device>(ARG(0)); OUTT* out = nullptr; \
    HRESULT hr = created(#METH, d->METH((DESCT*)G2H(ARG(1)), ARG(2) ? &out : nullptr), ARG(1), 8); \
    if (ARG(2)) wr32(ARG(2), SUCCEEDED(hr) ? wrap(out, CLS) : 0); RET((uint32_t)hr, 3); }
CREATE_STATE(dev_CreateBlendState, CreateBlendState, D3D11_BLEND_DESC, ID3D11BlendState, C_BLEND)
CREATE_STATE(dev_CreateDepthStencilState, CreateDepthStencilState, D3D11_DEPTH_STENCIL_DESC, ID3D11DepthStencilState, C_DEPTHSTENCIL)
CREATE_STATE(dev_CreateRasterizerState, CreateRasterizerState, D3D11_RASTERIZER_DESC, ID3D11RasterizerState, C_RASTERIZER)
CREATE_STATE(dev_CreateSamplerState, CreateSamplerState, D3D11_SAMPLER_DESC, ID3D11SamplerState, C_SAMPLER)
static void query_note(uint32_t gq, uint32_t type);      // remember the query's type (defined with the query calls)
static void dev_CreateQuery(void) {
    auto* d = N<ID3D11Device>(ARG(0)); ID3D11Query* out = nullptr;
    TM_INFO("CreateQuery type=%u flags=%u", ARG(1)?rd32(ARG(1)):0, ARG(1)?rd32(ARG(1)+4):0);
    HRESULT hr = d->CreateQuery((D3D11_QUERY_DESC*)G2H(ARG(1)), ARG(2) ? &out : nullptr);
    uint32_t gq = SUCCEEDED(hr) && out ? wrap(out, C_QUERY) : 0;
    if (gq && ARG(1)) query_note(gq, rd32(ARG(1)));
    if (ARG(2)) wr32(ARG(2), gq);
    RET((uint32_t)hr, 3);
}
static void q_GetDataSize(void) { auto* q = N<ID3D11Query>(ARG(0)); RET(q ? q->GetDataSize() : 0, 1); }
static void q_GetDesc(void) { auto* q = N<ID3D11Query>(ARG(0)); if (q && ARG(1)) q->GetDesc((D3D11_QUERY_DESC*)G2H(ARG(1))); RET(0, 2); }
static void dev_CreatePredicate(void) {
    auto* d = N<ID3D11Device>(ARG(0)); ID3D11Predicate* out = nullptr;
    HRESULT hr = d->CreatePredicate((D3D11_QUERY_DESC*)G2H(ARG(1)), ARG(2) ? &out : nullptr);
    if (ARG(2)) wr32(ARG(2), SUCCEEDED(hr) ? wrap(out, C_QUERY) : 0);
    RET((uint32_t)hr, 3);
}
static void dev_CreateClassLinkage(void) { if (ARG(1)) wr32(ARG(1), 0); RET(HR_FAIL, 2); }
static void dev_CreateCounter(void) { if (ARG(2)) wr32(ARG(2), 0); RET(HR_FAIL, 3); }
static void dev_CreateDeferredContext(void) { if (ARG(2)) wr32(ARG(2), 0); RET(HR_FAIL, 3); }
static void dev_OpenSharedResource(void) { if (ARG(3)) wr32(ARG(3), 0); RET(HR_FAIL, 4); }
static void dev_CheckFormatSupport(void) {
    auto* d = N<ID3D11Device>(ARG(0)); UINT s = 0;
    HRESULT hr = d->CheckFormatSupport((DXGI_FORMAT)ARG(1), &s);
    if (ARG(2)) wr32(ARG(2), s);
    RET((uint32_t)hr, 3);
}
static void dev_CheckMultisampleQualityLevels(void) {
    auto* d = N<ID3D11Device>(ARG(0)); UINT q = 0;
    HRESULT hr = d->CheckMultisampleQualityLevels((DXGI_FORMAT)ARG(1), ARG(2), &q);
    if (ARG(3)) wr32(ARG(3), q);
    RET((uint32_t)hr, 4);
}
static void dev_CheckCounterInfo(void) { RET(0, 2); }
static void dev_CheckCounter(void) { RET(HR_FAIL, 9); }
static void dev_CheckFeatureSupport(void) {
    auto* d = N<ID3D11Device>(ARG(0));
    HRESULT hr = d->CheckFeatureSupport((D3D11_FEATURE)ARG(1), ARG(2) ? G2H(ARG(2)) : nullptr, ARG(3));
    RET((uint32_t)hr, 4);
}
static void dev_GetFeatureLevel(void) { auto* d = N<ID3D11Device>(ARG(0)); RET(d ? d->GetFeatureLevel() : 0xb000, 1); }
static void dev_GetCreationFlags(void) { auto* d = N<ID3D11Device>(ARG(0)); RET(d ? d->GetCreationFlags() : 0, 1); }
static void dev_GetDeviceRemovedReason(void) { auto* d = N<ID3D11Device>(ARG(0)); RET(d ? (uint32_t)d->GetDeviceRemovedReason() : 0, 1); }
static void dev_GetImmediateContext(void) {
    auto* d = N<ID3D11Device>(ARG(0)); ID3D11DeviceContext* ctx = nullptr;
    if (d) d->GetImmediateContext(&ctx);
    if (ARG(1)) wr32(ARG(1), ctx ? wrap(ctx, C_CONTEXT) : 0);
    RET(0, 2);
}
static void dev_SetExceptionMode(void) { RET(HR_OK, 2); }
static void dev_GetExceptionMode(void) { RET(0, 1); }

// ================================================================= Context
// Shaders/samplers/SRV/CB setters take (StartSlot, Num, array)
static void ctx_VSSetConstantBuffers(void) { auto* c = N<ID3D11DeviceContext>(ARG(0)); std::vector<void*> a; unwrap_array(ARG(3), ARG(2), a); for (uint32_t i = 0; i < ARG(2) && ARG(1) + i < 14; i++) g_cb_vs[ARG(1) + i] = ARG(3) ? rd32(ARG(3) + 4 * i) : 0; c->VSSetConstantBuffers(ARG(1), ARG(2), (ID3D11Buffer**)a.data()); RET(0, 4); }
static void ctx_PSSetConstantBuffers(void) { auto* c = N<ID3D11DeviceContext>(ARG(0)); std::vector<void*> a; unwrap_array(ARG(3), ARG(2), a); for (uint32_t i = 0; i < ARG(2) && ARG(1) + i < 14; i++) g_cb_ps[ARG(1) + i] = ARG(3) ? rd32(ARG(3) + 4 * i) : 0; c->PSSetConstantBuffers(ARG(1), ARG(2), (ID3D11Buffer**)a.data()); RET(0, 4); }
static void ctx_GSSetConstantBuffers(void) { auto* c = N<ID3D11DeviceContext>(ARG(0)); std::vector<void*> a; unwrap_array(ARG(3), ARG(2), a); c->GSSetConstantBuffers(ARG(1), ARG(2), (ID3D11Buffer**)a.data()); RET(0, 4); }
static void ctx_HSSetConstantBuffers(void) { auto* c = N<ID3D11DeviceContext>(ARG(0)); std::vector<void*> a; unwrap_array(ARG(3), ARG(2), a); c->HSSetConstantBuffers(ARG(1), ARG(2), (ID3D11Buffer**)a.data()); RET(0, 4); }
static void ctx_DSSetConstantBuffers(void) { auto* c = N<ID3D11DeviceContext>(ARG(0)); std::vector<void*> a; unwrap_array(ARG(3), ARG(2), a); c->DSSetConstantBuffers(ARG(1), ARG(2), (ID3D11Buffer**)a.data()); RET(0, 4); }
static void ctx_CSSetConstantBuffers(void) { auto* c = N<ID3D11DeviceContext>(ARG(0)); std::vector<void*> a; unwrap_array(ARG(3), ARG(2), a); c->CSSetConstantBuffers(ARG(1), ARG(2), (ID3D11Buffer**)a.data()); RET(0, 4); }
static void ctx_VSSetShaderResources(void) { auto* c = N<ID3D11DeviceContext>(ARG(0)); std::vector<void*> a; unwrap_array(ARG(3), ARG(2), a); c->VSSetShaderResources(ARG(1), ARG(2), (ID3D11ShaderResourceView**)a.data()); RET(0, 4); }
static void ctx_PSSetShaderResources(void) { auto* c = N<ID3D11DeviceContext>(ARG(0)); std::vector<void*> a; unwrap_array(ARG(3), ARG(2), a); c->PSSetShaderResources(ARG(1), ARG(2), (ID3D11ShaderResourceView**)a.data()); RET(0, 4); }
static void ctx_GSSetShaderResources(void) { auto* c = N<ID3D11DeviceContext>(ARG(0)); std::vector<void*> a; unwrap_array(ARG(3), ARG(2), a); c->GSSetShaderResources(ARG(1), ARG(2), (ID3D11ShaderResourceView**)a.data()); RET(0, 4); }
static void ctx_HSSetShaderResources(void) { auto* c = N<ID3D11DeviceContext>(ARG(0)); std::vector<void*> a; unwrap_array(ARG(3), ARG(2), a); c->HSSetShaderResources(ARG(1), ARG(2), (ID3D11ShaderResourceView**)a.data()); RET(0, 4); }
static void ctx_DSSetShaderResources(void) { auto* c = N<ID3D11DeviceContext>(ARG(0)); std::vector<void*> a; unwrap_array(ARG(3), ARG(2), a); c->DSSetShaderResources(ARG(1), ARG(2), (ID3D11ShaderResourceView**)a.data()); RET(0, 4); }
static void ctx_CSSetShaderResources(void) { auto* c = N<ID3D11DeviceContext>(ARG(0)); std::vector<void*> a; unwrap_array(ARG(3), ARG(2), a); c->CSSetShaderResources(ARG(1), ARG(2), (ID3D11ShaderResourceView**)a.data()); RET(0, 4); }
static void ctx_VSSetSamplers(void) { auto* c = N<ID3D11DeviceContext>(ARG(0)); std::vector<void*> a; unwrap_array(ARG(3), ARG(2), a); c->VSSetSamplers(ARG(1), ARG(2), (ID3D11SamplerState**)a.data()); RET(0, 4); }
static void ctx_PSSetSamplers(void) { auto* c = N<ID3D11DeviceContext>(ARG(0)); std::vector<void*> a; unwrap_array(ARG(3), ARG(2), a); c->PSSetSamplers(ARG(1), ARG(2), (ID3D11SamplerState**)a.data()); RET(0, 4); }
static void ctx_GSSetSamplers(void) { auto* c = N<ID3D11DeviceContext>(ARG(0)); std::vector<void*> a; unwrap_array(ARG(3), ARG(2), a); c->GSSetSamplers(ARG(1), ARG(2), (ID3D11SamplerState**)a.data()); RET(0, 4); }
static void ctx_HSSetSamplers(void) { auto* c = N<ID3D11DeviceContext>(ARG(0)); std::vector<void*> a; unwrap_array(ARG(3), ARG(2), a); c->HSSetSamplers(ARG(1), ARG(2), (ID3D11SamplerState**)a.data()); RET(0, 4); }
static void ctx_DSSetSamplers(void) { auto* c = N<ID3D11DeviceContext>(ARG(0)); std::vector<void*> a; unwrap_array(ARG(3), ARG(2), a); c->DSSetSamplers(ARG(1), ARG(2), (ID3D11SamplerState**)a.data()); RET(0, 4); }
static void ctx_CSSetSamplers(void) { auto* c = N<ID3D11DeviceContext>(ARG(0)); std::vector<void*> a; unwrap_array(ARG(3), ARG(2), a); c->CSSetSamplers(ARG(1), ARG(2), (ID3D11SamplerState**)a.data()); RET(0, 4); }
#define CTX_SET_SHADER(fn, METH, T) static void fn(void) { auto* c = N<ID3D11DeviceContext>(ARG(0)); \
    std::vector<void*> ci; unwrap_array(ARG(2), ARG(3), ci); \
    c->METH(N<T>(ARG(1)), ARG(2) ? (ID3D11ClassInstance**)ci.data() : nullptr, ARG(3)); RET(0, 4); }
CTX_SET_SHADER(ctx_VSSetShader, VSSetShader, ID3D11VertexShader)
CTX_SET_SHADER(ctx_PSSetShader, PSSetShader, ID3D11PixelShader)
CTX_SET_SHADER(ctx_GSSetShader, GSSetShader, ID3D11GeometryShader)
CTX_SET_SHADER(ctx_HSSetShader, HSSetShader, ID3D11HullShader)
CTX_SET_SHADER(ctx_DSSetShader, DSSetShader, ID3D11DomainShader)
CTX_SET_SHADER(ctx_CSSetShader, CSSetShader, ID3D11ComputeShader)
static void ctx_DrawIndexed(void) { if (g_sprite_log) sprite_probe(N<ID3D11DeviceContext>(ARG(0)), ARG(1)); CAP_DRAW("DrawIndexed"); lazy_pre_draw(LAZY_INDEXED, ARG(1), ARG(2), (INT)ARG(3), 1, 0); N<ID3D11DeviceContext>(ARG(0))->DrawIndexed(ARG(1), ARG(2), (INT)ARG(3)); RET(0, 4); }
static void ctx_Draw(void) { CAP_DRAW("Draw"); lazy_pre_draw(LAZY_DRAW, ARG(1), ARG(2), 0, 1, 0); N<ID3D11DeviceContext>(ARG(0))->Draw(ARG(1), ARG(2)); RET(0, 3); }
static void ctx_DrawIndexedInstanced(void) { CAP_DRAW("DrawIndexedInstanced"); lazy_pre_draw(LAZY_INDEXED, ARG(1), ARG(3), (INT)ARG(4), ARG(2), ARG(5)); N<ID3D11DeviceContext>(ARG(0))->DrawIndexedInstanced(ARG(1), ARG(2), ARG(3), (INT)ARG(4), ARG(5)); RET(0, 6); }
static void ctx_DrawInstanced(void) { CAP_DRAW("DrawInstanced"); lazy_pre_draw(LAZY_DRAW, ARG(1), ARG(3), 0, ARG(2), ARG(4)); N<ID3D11DeviceContext>(ARG(0))->DrawInstanced(ARG(1), ARG(2), ARG(3), ARG(4)); RET(0, 5); }
static void ctx_DrawAuto(void) { CAP_DRAW("DrawAuto"); lazy_pre_draw(LAZY_UNKNOWN, 0, 0, 0, 0, 0); N<ID3D11DeviceContext>(ARG(0))->DrawAuto(); RET(0, 1); }
static void ctx_DrawIndexedInstancedIndirect(void) { CAP_DRAW("DrawIndexedInstancedIndirect"); lazy_pre_draw(LAZY_UNKNOWN, 0, 0, 0, 0, 0); N<ID3D11DeviceContext>(ARG(0))->DrawIndexedInstancedIndirect(N<ID3D11Buffer>(ARG(1)), ARG(2)); RET(0, 3); }
static void ctx_DrawInstancedIndirect(void) { CAP_DRAW("DrawInstancedIndirect"); lazy_pre_draw(LAZY_UNKNOWN, 0, 0, 0, 0, 0); N<ID3D11DeviceContext>(ARG(0))->DrawInstancedIndirect(N<ID3D11Buffer>(ARG(1)), ARG(2)); RET(0, 3); }
static void ctx_Dispatch(void) { N<ID3D11DeviceContext>(ARG(0))->Dispatch(ARG(1), ARG(2), ARG(3)); RET(0, 4); }
static void ctx_DispatchIndirect(void) { N<ID3D11DeviceContext>(ARG(0))->DispatchIndirect(N<ID3D11Buffer>(ARG(1)), ARG(2)); RET(0, 3); }
static void ctx_IASetInputLayout(void) { g_ia_layout = ARG(1); N<ID3D11DeviceContext>(ARG(0))->IASetInputLayout(N<ID3D11InputLayout>(ARG(1))); RET(0, 2); }
static void ctx_IASetVertexBuffers(void) {
    auto* c = N<ID3D11DeviceContext>(ARG(0)); std::vector<void*> b; unwrap_array(ARG(3), ARG(2), b);
    for (uint32_t i = 0; i < ARG(2) && ARG(1) + i < 32; i++) {
        IASlot& sl = g_ia_vb[ARG(1) + i];
        sl.buf = ARG(3) ? rd32(ARG(3) + 4 * i) : 0;
        sl.stride = ARG(4) ? rd32(ARG(4) + 4 * i) : 0;
        sl.offset = ARG(5) ? rd32(ARG(5) + 4 * i) : 0;
    }
    c->IASetVertexBuffers(ARG(1), ARG(2), (ID3D11Buffer**)b.data(), ARG(4) ? (UINT*)G2H(ARG(4)) : nullptr, ARG(5) ? (UINT*)G2H(ARG(5)) : nullptr);
    RET(0, 6);
}
static void ctx_IASetIndexBuffer(void) { g_ia_ib = ARG(1); g_ia_ib_fmt = ARG(2); g_ia_ib_off = ARG(3); N<ID3D11DeviceContext>(ARG(0))->IASetIndexBuffer(N<ID3D11Buffer>(ARG(1)), (DXGI_FORMAT)ARG(2), ARG(3)); RET(0, 4); }
static void ctx_IASetPrimitiveTopology(void) { N<ID3D11DeviceContext>(ARG(0))->IASetPrimitiveTopology((D3D11_PRIMITIVE_TOPOLOGY)ARG(1)); RET(0, 2); }
// Remember what the pass that starts now renders to (for the sync modes above).
static void note_targets(uint32_t n, const std::vector<void*>& rt) {
    g_out_ncolour = 0; g_out_fmt = 0;
    if (g_rel >= 0) g_rel++;
    for (uint32_t i = 0; i < n && i < rt.size(); i++) if (rt[i]) {
        if (!g_out_ncolour) {
            ID3D11Resource* r = nullptr; ((ID3D11RenderTargetView*)rt[i])->GetResource(&r);
            D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
            if (r) { r->GetType(&dim); if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { D3D11_TEXTURE2D_DESC td{}; static_cast<ID3D11Texture2D*>(r)->GetDesc(&td); g_out_fmt = td.Format; } r->Release(); }
        }
        g_out_ncolour++;
    }
    if (g_out_ncolour == 1 && g_out_fmt == 34 && g_rel < 0) g_rel = 0;
}
// ---- render-pass split (TM_RP_SPLIT) ------------------------------------------------
// Adreno 830, driver 512.800: when several render passes that share attachments but
// differ in their attachment lists end up in ONE command buffer, some draws come out
// wrong. In this game the frame does
//     {velocity} -> {HDR, velocity} -> {HDR} -> {HDR, velocity} -> ...
// and the lamps drawn in the fourth pass turn into a screen-filling glare in about two
// frames out of three (found by bisecting with TM_GFX_SYNC; every wait or readback
// between the passes hides it, and so did the slower CPU path before the performance
// work, because DXVK then flushed by its timer at most target changes). One submission
// boundary anywhere between the first and the fourth pass is enough.
// A second case showed up over water (the lake under the long jump of track #01):
//     {HDR}+depth: water fog -> {refraction copy of HDR} -> {HDR}+depth: water surface
// in one command buffer lets the water surface ignore the depth buffer in a lattice of
// 32x32-pixel tiles (every second or third tile, the GPU's three slices): the lake's big
// quads show through the terrain as screen-filling flat or textured triangles that
// flicker from frame to frame. A boundary before the copy, before the water pass or right
// after it removes that; one before the water-fog pass does not.
// The exact condition inside the driver is not known, and a boundary is not harmless
// either. Track #05 has the sun in view at the start; the engine dims its lens flare by
// the cloud cover, which it reads from the cloud mask: the cloud sprites render to
// {HDR, mask}, later a 2x2 box of the mask at the sun's position is copied into a tiny
// texture that the flare shader samples. When a submission ends right after that
// {HDR, mask} pass (instead of going on with the {HDR} fog pass that follows it), the
// flare comes out at full strength in about a third of the frames: a white blob, with
// the tile lattice inside. Found by bisecting with "debug.tm.gfxsync 12 6 6" on top of
// rule 1 (37 % of the frames; any other single extra boundary: 0 to 0.4 %). The modes
// that flush at every target change (3, 4, 6) do exactly that. So the default is the
// narrowest set of rules that removes all the known cases:
//   1 (Android default): Flush (a) when a texture is bound as render target in another
//      slot than the one it last had in this submission -- the velocity target moving
//      from slot 0 to slot 1 above -- (b) before every pass with several colour targets
//      whose target list differs from the previous one -- the fourth pass above, and the
//      particle pass {HDR, mask} later in the frame; (a) alone left 1-2 % of the frames
//      wrong -- and (c) when a target list (colour targets and depth view) comes back:
//      it was used earlier in this submission and something was drawn, cleared or copied
//      under another list since -- the water surface above. About 35 flushes per frame.
//   5: rules (a) and (b) only (0.5.0; 22 per frame; the water lattice shows).
//   2: only rule (b).
//   4: Flush at every target change that has draws, clears or copies recorded before it
//      (0.5.1's default, about 56 per frame; the track #05 blob shows).
//   3: Flush at every real target change (about 90 per frame; the blob shows).
//   6: mode 4 plus rules (a) and (b) even when nothing was recorded (the blob shows).
//   0: off (the start-lamp glare of track #01 shows).
// "setprop debug.tm.rpsplit <n>" changes the mode while the game runs.
extern "C" void tm_dxvk_barrier(ID3D11DeviceContext* c);   // gfx/dxvk/prepare.py patch
static void rp_split(ID3D11DeviceContext* c, uint32_t n, const std::vector<void*>& rt, bool same, uint32_t depth_view) {
    if (g_rp_split < 0) {
        const char* e = getenv("TM_RP_SPLIT");
#ifdef __ANDROID__
        g_rp_split = e && *e ? atoi(e) : 1;
        // With the bridge's rules in charge, DXVK's own timer flush at render-target changes
        // only adds boundaries at places that depend on timing (TM_DXVK_IMPLICIT_FLUSH=1 or
        // "setprop debug.tm.implflush 1" brings it back).
        if (g_rp_split) { const char* f = getenv("TM_DXVK_IMPLICIT_FLUSH"); g_tm_dxvk_implicit_flush = f && *f ? atoi(f) : 0; }
#else
        g_rp_split = e && *e ? atoi(e) : 0;
#endif
    }
    if (!g_rp_split || same) return;
    static struct { ID3D11Resource* res; uint32_t slot; } seen[32];
    static uint64_t sets[32];                       // target lists used since the last split (hashes)
    static uint32_t left_work[32];                  // g_rp_work when the list was last replaced by another one
    static uint32_t nseen, nsets, cur = 0xFFFFFFFFu, frame = 0xFFFFFFFFu;
    uint32_t now = g_present_count_now();
    if (now != frame) { frame = now; nseen = 0; nsets = 0; cur = 0xFFFFFFFFu; g_rp_work = 0; }   // a Present ends the submission anyway
    if (cur < nsets) left_work[cur] = g_rp_work;
    ID3D11Resource* res[8] = {}; uint32_t ncol = 0;
    for (uint32_t i = 0; i < n && i < 8 && i < rt.size(); i++) if (rt[i]) { ((ID3D11RenderTargetView*)rt[i])->GetResource(&res[i]); if (res[i]) { res[i]->Release(); ncol++; } }
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < 8; i++) h = (h ^ (uint64_t)(uintptr_t)res[i]) * 1099511628211ull;
    h = (h ^ depth_view) * 1099511628211ull;        // the same colour targets with another depth view are another list
    bool split = false;
    if (g_rp_split == 3) split = true;
    else if (g_rp_split == 4) split = g_rp_work > 0;
    else if (g_rp_split == 6) {
        // everything mode 4 and mode 1 would do: work recorded, or a multi-target list
        // (rule b), or a target that changes its slot (rule a)
        split = g_rp_work > 0 || ncol > 1;
        for (uint32_t i = 0; i < 8 && !split; i++) if (res[i])
            for (uint32_t k = 0; k < nseen; k++) if (seen[k].res == res[i] && seen[k].slot != i) { split = true; break; }
    }
    else if (g_rp_split == 2) split = ncol > 1;
    else {
        // a) a target changes its slot
        for (uint32_t i = 0; i < 8 && !split; i++) if (res[i])
            for (uint32_t k = 0; k < nseen; k++) if (seen[k].res == res[i] && seen[k].slot != i) { split = true; break; }
        // b) a list of several targets follows a different list (this includes the list
        //    coming back, {HDR, velocity} -> {HDR} -> {HDR, velocity}, and a second list
        //    that shares a target, {HDR, velocity} -> {HDR} -> {HDR, mask}: the particle
        //    pass, whose mask the water pass reads)
        if (!split && ncol > 1) split = true;
        // c) a list comes back: it was used earlier in this submission and something was
        //    rendered elsewhere since ({HDR} -> {refraction copy} -> {HDR}: the water
        //    surface, see above)
        if (!split && g_rp_split != 5)
            for (uint32_t k = 0; k < nsets; k++) if (sets[k] == h) { split = g_rp_work > left_work[k]; break; }
    }
    if (split) {
        // A split ends the render pass with a full memory barrier (tm_dxvk_barrier) instead of
        // a submission: the water lattice on track #01's jump stays away the same (0 of 1680
        // frames; 23 without splits), and the ~40 submissions per frame are gone.
        // TM_RP_BARRIER=0: submit (Flush) as before.
        static int barrier = -1;
        if (barrier < 0) { const char* b = getenv("TM_RP_BARRIER"); barrier = !(b && *b == '0'); TM_INFO("render-pass splits: %s", barrier ? "barrier, no submission" : "submission (Flush)"); }
        if (barrier) tm_dxvk_barrier(c); else c->Flush();
        g_rp_splits++; nseen = 0; nsets = 0; g_rp_work = 0;
    }
    { uint32_t k = 0; while (k < nsets && sets[k] != h) k++; if (k == nsets && nsets < 32) sets[nsets++] = h; cur = k; }
    for (uint32_t i = 0; i < 8; i++) if (res[i]) {
        uint32_t k = 0;
        while (k < nseen && seen[k].res != res[i]) k++;
        if (k == nseen) { if (nseen == 32) continue; nseen++; }
        seen[k].res = res[i]; seen[k].slot = i;     // pointers are compared only, never used
    }
}
// Are these the render targets that are bound already? (Remembers the new set.)
static bool same_targets(uint32_t n, uint32_t arr, uint32_t ds) {
    static uint32_t last[9]; uint32_t now[9] = {};
    for (uint32_t i = 0; i < 8; i++) now[i] = i < n && arr ? rd32(arr + 4 * i) : 0;
    now[8] = ds;
    bool same = !memcmp(last, now, sizeof now);
    memcpy(last, now, sizeof now);
    return same;
}
static void ctx_OMSetRenderTargets(void) {
    auto* c = N<ID3D11DeviceContext>(ARG(0)); std::vector<void*> rt; unwrap_array(ARG(2), ARG(1), rt);
    g_cap_ctx = c;
    g_new_ncolour = 0; for (uint32_t i = 0; i < ARG(1) && i < rt.size(); i++) if (rt[i]) g_new_ncolour++;
    { bool same = same_targets(ARG(1), ARG(2), ARG(3)); gfx_sync_point(c, same); rp_split(c, ARG(1), rt, same, ARG(3)); }
    note_targets(ARG(1), rt);
    if (g_trace == 2) {
        char line[400]; int k = 0;
        for (uint32_t i = 0; i < ARG(1) && i < 8; i++) {
            if (!ARG(2) || !rt[i]) { k += snprintf(line + k, sizeof line - k, " -"); continue; }
            ID3D11Resource* r = nullptr; ((ID3D11RenderTargetView*)rt[i])->GetResource(&r);
            D3D11_TEXTURE2D_DESC td{}; D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
            if (r) { r->GetType(&dim); if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) static_cast<ID3D11Texture2D*>(r)->GetDesc(&td); r->Release(); }
            k += snprintf(line + k, sizeof line - k, " [%08X %ux%u fmt %u]", rd32(ARG(2) + 4 * i), td.Width, td.Height, (unsigned)td.Format);
        }
        TRACE("targets:%s depth %08X", g_pass_in_frame, line, ARG(3));
    }
    if (g_cap_active) { cap_leave_pass(c); cap_enter_pass(ARG(1), ARG(2) ? (ID3D11RenderTargetView**)rt.data() : nullptr, N<ID3D11DepthStencilView>(ARG(3))); }
    c->OMSetRenderTargets(ARG(1), ARG(2) ? (ID3D11RenderTargetView**)rt.data() : nullptr, N<ID3D11DepthStencilView>(ARG(3)));
    RET(0, 4);
}
static void ctx_OMSetRenderTargetsAndUnorderedAccessViews(void) {
    auto* c = N<ID3D11DeviceContext>(ARG(0)); std::vector<void*> rt, uav;
    unwrap_array(ARG(2), ARG(1), rt); unwrap_array(ARG(6), ARG(5), uav);
    g_new_ncolour = 0; if (ARG(1) != 0xFFFFFFFFu) for (uint32_t i = 0; i < ARG(1) && i < rt.size(); i++) if (rt[i]) g_new_ncolour++;
    { bool same = ARG(1) != 0xFFFFFFFFu && same_targets(ARG(1), ARG(2), ARG(3)); gfx_sync_point(c, same); if (ARG(1) != 0xFFFFFFFFu) rp_split(c, ARG(1), rt, same, ARG(3)); }
    if (ARG(1) != 0xFFFFFFFFu) note_targets(ARG(1), rt);
    if (g_cap_active && ARG(1) != 0xFFFFFFFFu) { cap_leave_pass(c); cap_enter_pass(ARG(1), ARG(2) ? (ID3D11RenderTargetView**)rt.data() : nullptr, N<ID3D11DepthStencilView>(ARG(3))); }
    c->OMSetRenderTargetsAndUnorderedAccessViews(ARG(1), ARG(2) ? (ID3D11RenderTargetView**)rt.data() : nullptr,
        N<ID3D11DepthStencilView>(ARG(3)), ARG(4), ARG(5), ARG(6) ? (ID3D11UnorderedAccessView**)uav.data() : nullptr,
        ARG(7) ? (UINT*)G2H(ARG(7)) : nullptr);
    RET(0, 8);
}
static void ctx_OMSetBlendState(void) { N<ID3D11DeviceContext>(ARG(0))->OMSetBlendState(N<ID3D11BlendState>(ARG(1)), ARG(2) ? (const FLOAT*)G2H(ARG(2)) : nullptr, ARG(3)); RET(0, 4); }
static void ctx_OMSetDepthStencilState(void) { N<ID3D11DeviceContext>(ARG(0))->OMSetDepthStencilState(N<ID3D11DepthStencilState>(ARG(1)), ARG(2)); RET(0, 3); }
static void ctx_RSSetState(void) { N<ID3D11DeviceContext>(ARG(0))->RSSetState(N<ID3D11RasterizerState>(ARG(1))); RET(0, 2); }
static void ctx_RSSetViewports(void) { N<ID3D11DeviceContext>(ARG(0))->RSSetViewports(ARG(1), ARG(2) ? (D3D11_VIEWPORT*)G2H(ARG(2)) : nullptr); RET(0, 3); }
static void ctx_RSSetScissorRects(void) { N<ID3D11DeviceContext>(ARG(0))->RSSetScissorRects(ARG(1), ARG(2) ? (D3D11_RECT*)G2H(ARG(2)) : nullptr); RET(0, 3); }
static void ctx_SOSetTargets(void) { auto* c = N<ID3D11DeviceContext>(ARG(0)); std::vector<void*> b; unwrap_array(ARG(2), ARG(1), b); c->SOSetTargets(ARG(1), ARG(2) ? (ID3D11Buffer**)b.data() : nullptr, ARG(3) ? (UINT*)G2H(ARG(3)) : nullptr); RET(0, 4); }
static void ctx_SetPredication(void) { N<ID3D11DeviceContext>(ARG(0))->SetPredication(N<ID3D11Predicate>(ARG(1)), ARG(2)); RET(0, 3); }

static void ctx_ClearRenderTargetView(void) { g_clears_this_pass++; g_rp_work++; if (g_trace == 2) { const float* cc = (const float*)G2H(ARG(2)); TRACE("ClearRenderTargetView %08X (%g %g %g %g)", g_pass_in_frame, ARG(1), cc[0], cc[1], cc[2], cc[3]); } N<ID3D11DeviceContext>(ARG(0))->ClearRenderTargetView(N<ID3D11RenderTargetView>(ARG(1)), (const FLOAT*)G2H(ARG(2))); RET(0, 3); }
static void ctx_ClearDepthStencilView(void) { g_rp_work++; TRACE("ClearDepthStencilView %08X flags %u depth %g", g_pass_in_frame, ARG(1), ARG(2), (double)*(float*)G2H(g_esp + 4 + 12)); N<ID3D11DeviceContext>(ARG(0))->ClearDepthStencilView(N<ID3D11DepthStencilView>(ARG(1)), ARG(2), *(float*)G2H(g_esp + 4 + 12), (UINT8)ARG(4)); RET(0, 5); }
static void ctx_ClearUnorderedAccessViewUint(void) { N<ID3D11DeviceContext>(ARG(0))->ClearUnorderedAccessViewUint(N<ID3D11UnorderedAccessView>(ARG(1)), (const UINT*)G2H(ARG(2))); RET(0, 3); }
static void ctx_ClearUnorderedAccessViewFloat(void) { N<ID3D11DeviceContext>(ARG(0))->ClearUnorderedAccessViewFloat(N<ID3D11UnorderedAccessView>(ARG(1)), (const FLOAT*)G2H(ARG(2))); RET(0, 3); }
static void ctx_GenerateMips(void) { g_rp_work++; TRACE("GenerateMips view %08X", g_pass_in_frame, ARG(1)); N<ID3D11DeviceContext>(ARG(0))->GenerateMips(N<ID3D11ShaderResourceView>(ARG(1))); RET(0, 2); }

// Map hands the game a guest-addressable staging buffer (DXVK's mapped pointer
// is host memory outside the 32-bit guest space); Unmap copies it to the real
// mapping. The staging buffer must hold what the game expects to still be there:
//   READ / WRITE / READ_WRITE : current contents are copied in
//   WRITE_DISCARD             : contents undefined, nothing copied in
//   WRITE_NO_OVERWRITE        : the game appends to data written by earlier maps
//                               and Unmap copies the whole buffer back, so the
//                               staging buffer has to carry those earlier bytes
// Buffers mapped DISCARD/NO_OVERWRITE (dynamic vertex/index/constant buffers)
// therefore keep ONE persistent staging buffer per subresource; everything else
// gets a transient one that is freed at Unmap.
extern "C" uint32_t tm_map_begin(uint32_t res, uint32_t sub, void* host, uint32_t total, uint32_t type, int is_buffer);
extern "C" int tm_map_shadow_valid(uint32_t res, uint32_t sub);
static uint64_t g_map_block_ns;  // time inside native Map for READ/WRITE/READ_WRITE since the last statistics line
static uint32_t g_stg_discards;  // staging-texture maps turned into DISCARD since then
static uint32_t g_map_failed;   // Map calls that returned an error since the last statistics line
static void ctx_Map(void) {
    auto* c = N<ID3D11DeviceContext>(ARG(0));
    ID3D11Resource* r = N<ID3D11Resource>(ARG(1));
    D3D11_MAPPED_SUBRESOURCE m{};
    if (g_gfx_sync == 7 && ARG(3) == D3D11_MAP_WRITE_DISCARD) {          // diagnostics, see gfx_sync_point
        D3D11_RESOURCE_DIMENSION dim0 = D3D11_RESOURCE_DIMENSION_UNKNOWN; r->GetType(&dim0);
        if (dim0 == D3D11_RESOURCE_DIMENSION_BUFFER) { D3D11_BUFFER_DESC b0{}; static_cast<ID3D11Buffer*>(r)->GetDesc(&b0); if (b0.BindFlags == D3D11_BIND_INDEX_BUFFER && b0.ByteWidth >= 65536) c->Flush(); }
    }
    // Staging textures written through Map(WRITE): the engine uploads every video frame and
    // two parameter tables this way, a dozen maps per frame. DXVK has to make such a map
    // wait until its command thread has caught up and the GPU is done with the previous
    // copy out of the texture, which stalls the game's main thread several times per
    // frame. The bridge keeps the texture's contents itself (the guest only ever sees
    // the bridge's staging copy), so once it has them the native map can be a DISCARD:
    // a fresh slice at once, no wait. Unmap writes the whole subresource back as before.
    // TM_STAGING_DISCARD=0 turns this off.
    D3D11_MAP native_type = (D3D11_MAP)ARG(3);
    int tex_flags = 0;                      // 4: staging texture with a kept copy, 8: this map was turned into a DISCARD
    if (ARG(3) == D3D11_MAP_READ || ARG(3) == D3D11_MAP_WRITE || ARG(3) == D3D11_MAP_READ_WRITE) {
        static int stg = -1; if (stg < 0) { const char* e = getenv("TM_STAGING_DISCARD"); stg = !(e && *e == '0'); }
        D3D11_RESOURCE_DIMENSION dimt = D3D11_RESOURCE_DIMENSION_UNKNOWN; r->GetType(&dimt);
        if (stg && dimt == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC td{}; static_cast<ID3D11Texture2D*>(r)->GetDesc(&td);
            if (td.Usage == D3D11_USAGE_STAGING && (td.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE)) {
                tex_flags = 4;
                if (ARG(3) == D3D11_MAP_WRITE && ARG(5) && tm_map_shadow_valid(ARG(1), ARG(2))) { native_type = D3D11_MAP_WRITE_DISCARD; tex_flags |= 8; g_stg_discards++; }
            }
        }
    }
    uint64_t t_map0 = native_type == D3D11_MAP_WRITE_DISCARD || native_type == D3D11_MAP_WRITE_NO_OVERWRITE ? 0 : tm_now_ns();
    HRESULT hr = c->Map(r, ARG(2), native_type, ARG(4), ARG(5) ? &m : nullptr);
    if (t_map0) g_map_block_ns += tm_now_ns() - t_map0;
    {   // diagnostics (TM_MAP_LOG=1): maps that are not the usual DISCARD / NO_OVERWRITE, and every failure
        static int map_log = -1; static uint32_t logged;
        if (map_log < 0) { const char* e = getenv("TM_MAP_LOG"); map_log = e && *e ? atoi(e) : 0; }
        if (FAILED(hr)) g_map_failed++;
        if (map_log && (FAILED(hr) || (ARG(3) != D3D11_MAP_WRITE_DISCARD && ARG(3) != D3D11_MAP_WRITE_NO_OVERWRITE)) && (logged++ < 80 || (logged & 1023) == 0)) {
            D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN; r->GetType(&dim);
            uint32_t w = 0, h = 0, fmt = 0, usage = 0, bind = 0;
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { D3D11_TEXTURE2D_DESC td{}; static_cast<ID3D11Texture2D*>(r)->GetDesc(&td); w = td.Width; h = td.Height; fmt = td.Format; usage = td.Usage; bind = td.BindFlags; }
            else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) { D3D11_BUFFER_DESC bd{}; static_cast<ID3D11Buffer*>(r)->GetDesc(&bd); w = bd.ByteWidth; usage = bd.Usage; bind = bd.BindFlags; }
            TM_INFO("Map res %08X sub %u type %u flags %u -> hr %08X | dim %d %ux%u fmt %u usage %u bind %X rowpitch %u from %08X",
                    ARG(1), ARG(2), ARG(3), ARG(4), (unsigned)hr, (int)dim, w, h, fmt, usage, bind, m.RowPitch, g_cur_func);
        }
    }
    if (g_trace == 2) {
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN; r->GetType(&dim);
        D3D11_BUFFER_DESC bd{}; if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) static_cast<ID3D11Buffer*>(r)->GetDesc(&bd);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER && ARG(3) == D3D11_MAP_WRITE_DISCARD && bd.ByteWidth <= 4096) { g_trace_cbmaps++; g_trace_cbbytes += bd.ByteWidth; }
        else { char d[160]; trace_res(d, sizeof d, r); TRACE("Map %08X sub %u type %u flags %u hr %08X: %s", g_pass_in_frame, ARG(1), ARG(2), ARG(3), ARG(4), (unsigned)hr, d); }
    }
    if (ARG(5)) {
        uint32_t gbuf = 0;
        if (SUCCEEDED(hr) && m.pData) {
            D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
            r->GetType(&dim);
            uint32_t total = m.DepthPitch ? m.DepthPitch : m.RowPitch;
            int buf_flags = 0;                 // 1: a buffer, 2: only ever read by the input assembler
            if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
                D3D11_BUFFER_DESC bd{}; static_cast<ID3D11Buffer*>(r)->GetDesc(&bd);
                total = bd.ByteWidth;
                buf_flags = 1 | (bd.Usage == D3D11_USAGE_DYNAMIC && bd.BindFlags
                                 && !(bd.BindFlags & ~(UINT)(D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER)) ? 2 : 0);
            }
            if (!total) total = 0x10000;
            gbuf = tm_map_begin(ARG(1), ARG(2), m.pData, total, ARG(3), buf_flags | tex_flags);
        }
        wr32(ARG(5) + 0, gbuf);
        wr32(ARG(5) + 4, m.RowPitch);
        wr32(ARG(5) + 8, m.DepthPitch);
    }
    RET((uint32_t)hr, 6);
}
static void ctx_Unmap(void) {
    auto* c = N<ID3D11DeviceContext>(ARG(0));
    tm_map_flush(ARG(1), ARG(2));
    c->Unmap(N<ID3D11Resource>(ARG(1)), ARG(2));
    RET(0, 3);
}
static void ctx_UpdateSubresource(void) {
    auto* c = N<ID3D11DeviceContext>(ARG(0));
    D3D11_BOX* box = ARG(3) ? (D3D11_BOX*)G2H(ARG(3)) : nullptr;
    g_rp_work++;
    tm_map_invalidate(ARG(1));
    if (g_trace == 2) {
        char d[160]; trace_res(d, sizeof d, N<ID3D11Resource>(ARG(1)));
        if (box) TRACE("UpdateSubresource %08X sub %u box %u..%u x %u..%u: %s", g_pass_in_frame, ARG(1), ARG(2), box->left, box->right, box->top, box->bottom, d);
        else TRACE("UpdateSubresource %08X sub %u whole: %s", g_pass_in_frame, ARG(1), ARG(2), d);
    }
    if (g_upd_mode == 1) c->Flush();                 // diagnostics: everything recorded so far is submitted before the update
    c->UpdateSubresource(N<ID3D11Resource>(ARG(1)), ARG(2), box, G2H(ARG(4)), ARG(5), ARG(6));
    RET(0, 7);
}
static void ctx_CopyResource(void) { g_rp_work++; tm_map_invalidate(ARG(1)); lazy_flush_all(ARG(2));
    if (g_trace == 2) { char a[160], b[160]; trace_res(a, sizeof a, N<ID3D11Resource>(ARG(1))); trace_res(b, sizeof b, N<ID3D11Resource>(ARG(2))); TRACE("CopyResource %08X <- %08X: %s <- %s", g_pass_in_frame, ARG(1), ARG(2), a, b); }
    if (g_upd_mode == 2) N<ID3D11DeviceContext>(ARG(0))->Flush();
    N<ID3D11DeviceContext>(ARG(0))->CopyResource(N<ID3D11Resource>(ARG(1)), N<ID3D11Resource>(ARG(2))); RET(0, 3); }
static void ctx_CopySubresourceRegion(void) {
    g_rp_work++;
    tm_map_invalidate(ARG(1));
    lazy_flush_all(ARG(6));
    if (g_trace == 2) { char a[160], b[160]; trace_res(a, sizeof a, N<ID3D11Resource>(ARG(1))); trace_res(b, sizeof b, N<ID3D11Resource>(ARG(6))); TRACE("CopySubresourceRegion %08X sub %u <- %08X sub %u: %s <- %s", g_pass_in_frame, ARG(1), ARG(2), ARG(6), ARG(7), a, b); }
    if (g_upd_mode == 2) N<ID3D11DeviceContext>(ARG(0))->Flush();
    N<ID3D11DeviceContext>(ARG(0))->CopySubresourceRegion(N<ID3D11Resource>(ARG(1)), ARG(2), ARG(3), ARG(4), ARG(5),
        N<ID3D11Resource>(ARG(6)), ARG(7), ARG(8) ? (D3D11_BOX*)G2H(ARG(8)) : nullptr);
    RET(0, 9);
}
static void ctx_CopyStructureCount(void) { N<ID3D11DeviceContext>(ARG(0))->CopyStructureCount(N<ID3D11Buffer>(ARG(1)), ARG(2), N<ID3D11UnorderedAccessView>(ARG(3))); RET(0, 4); }
static void ctx_ResolveSubresource(void) { g_rp_work++; N<ID3D11DeviceContext>(ARG(0))->ResolveSubresource(N<ID3D11Resource>(ARG(1)), ARG(2), N<ID3D11Resource>(ARG(3)), ARG(4), (DXGI_FORMAT)ARG(5)); RET(0, 6); }
// Query traffic for TM_GFX_STATS / TM_QUERY_LOG (diagnostics).
static struct { uint32_t begin, end, ok, pending, flushes, occ_ok, occ_nonzero; uint64_t occ_max, occ_sum; } g_qstat;
static int g_query_log = -1;
static uint32_t g_query_logged;
// Guest query object -> its D3D11_QUERY type, and for timestamp queries the CPU time of
// their last End() (see TM_TIMESTAMP_MODE below).
struct QueryInfo { uint32_t type = 0xFFFFFFFFu; uint64_t end_ns = 0; };
static std::unordered_map<uint32_t, QueryInfo> g_query_info;
// TM_TIMESTAMP_MODE: 0 = the GPU timestamps DXVK reads back; 1 = the CPU clock at End()
// with a 1 GHz frequency (always available, monotonic).
static int g_ts_mode = -1;
static int ts_mode(void) {
    if (g_ts_mode < 0) { const char* e = getenv("TM_TIMESTAMP_MODE"); g_ts_mode = e && *e ? atoi(e) : TM_TIMESTAMP_MODE_DEFAULT; }
    return g_ts_mode;
}
static void query_note(uint32_t gq, uint32_t type) { QueryInfo qi; qi.type = type; g_query_info[gq] = qi; }
static inline QueryInfo* query_info(uint32_t q) { auto it = g_query_info.find(q); return it == g_query_info.end() ? nullptr : &it->second; }
static void ctx_Begin(void) { g_qstat.begin++; if (g_trace == 2) { QueryInfo* qi = query_info(ARG(1)); TRACE("query Begin %08X type %u (draw %u of the pass, from guest %08X)", g_pass_in_frame, ARG(1), qi ? qi->type : 99u, g_draws_this_pass, g_cur_func); } N<ID3D11DeviceContext>(ARG(0))->Begin(N<ID3D11Asynchronous>(ARG(1))); RET(0, 2); }
static void ctx_End(void) {
    g_qstat.end++;
    if (g_trace == 2) { QueryInfo* qi = query_info(ARG(1)); TRACE("query End %08X type %u (draw %u of the pass)", g_pass_in_frame, ARG(1), qi ? qi->type : 99u, g_draws_this_pass); }
    if (QueryInfo* qi = query_info(ARG(1))) if (qi->type == D3D11_QUERY_TIMESTAMP || qi->type == D3D11_QUERY_TIMESTAMP_DISJOINT) qi->end_ns = tm_now_ns();
    N<ID3D11DeviceContext>(ARG(0))->End(N<ID3D11Asynchronous>(ARG(1)));
    RET(0, 2);
}
static void ctx_GetData(void) {
    auto* c = N<ID3D11DeviceContext>(ARG(0));
    QueryInfo* qi = query_info(ARG(1));
    uint32_t qtype = qi ? qi->type : 0xFFFFFFFFu;
    if (g_query_log < 0) { const char* e = getenv("TM_QUERY_LOG"); g_query_log = e && *e ? atoi(e) : 0; }
    if (ts_mode() == 1 && qi && qi->end_ns && (qtype == D3D11_QUERY_TIMESTAMP || qtype == D3D11_QUERY_TIMESTAMP_DISJOINT)) {
        // Answer from the CPU clock: no wait for the GPU, and never a stale or zero tick.
        if (ARG(2)) {
            if (qtype == D3D11_QUERY_TIMESTAMP && ARG(3) >= 8) { wr32(ARG(2), (uint32_t)qi->end_ns); wr32(ARG(2) + 4, (uint32_t)(qi->end_ns >> 32)); }
            if (qtype == D3D11_QUERY_TIMESTAMP_DISJOINT && ARG(3) >= 12) { wr32(ARG(2), 1000000000u); wr32(ARG(2) + 4, 0); wr32(ARG(2) + 8, 0); if (ARG(3) >= 16) wr32(ARG(2) + 12, 0); }
        }
        g_qstat.ok++;
        RET(0, 5);
    }
    HRESULT hr = c->GetData(N<ID3D11Asynchronous>(ARG(1)), ARG(2) ? G2H(ARG(2)) : nullptr, ARG(3), ARG(4));
    if (hr == 0) g_qstat.ok++; else g_qstat.pending++;
    if (g_trace == 2) {
        uint64_t tv = hr == 0 && ARG(2) && ARG(3) >= 8 ? (rd32(ARG(2)) | ((uint64_t)rd32(ARG(2) + 4) << 32)) : 0;
        TRACE("query GetData %08X type %u hr %08X value %llu (size %u flags %u)", g_pass_in_frame, ARG(1), qtype, (unsigned)hr, (unsigned long long)tv, ARG(3), ARG(4));
    }
    // TM_QUERY_LOG=4: every occlusion result with its frame (which frames get which value)
    if (g_query_log >= 4 && hr == 0 && qtype == D3D11_QUERY_OCCLUSION && ARG(2) && ARG(3) >= 8)
        TM_INFO("occ %08X frame %u pass %d = %u (from guest %08X)", ARG(1), tm_gfx_present_count(), g_pass_in_frame, rd32(ARG(2)), g_cur_func);
    if (hr == 0 && ARG(2) && ARG(3) >= 8) {
        uint64_t v = rd32(ARG(2)) | ((uint64_t)rd32(ARG(2) + 4) << 32);
        if (qtype == D3D11_QUERY_OCCLUSION) {
            g_qstat.occ_ok++; g_qstat.occ_sum += v; if (v) g_qstat.occ_nonzero++; if (v > g_qstat.occ_max) g_qstat.occ_max = v;
        }
        if (g_query_log > 1 && (g_query_logged++ < 60 || (g_query_logged & 255) == 0))
            TM_INFO("query %08X type %u = %llu%s (size %u flags %u)", ARG(1), qtype, (unsigned long long)v,
                    qtype == D3D11_QUERY_TIMESTAMP_DISJOINT ? (rd32(ARG(2) + 8) ? " DISJOINT" : " not disjoint") : "", ARG(3), ARG(4));
    } else if (g_query_log > 2) TM_INFO("GetData %08X type %u hr=%08X size %u flags %u", ARG(1), qtype, (unsigned)hr, ARG(3), ARG(4));
    if (hr == 1 /*S_FALSE*/) {
        // The guest polls this on the thread holding the machine lock. Hand the
        // machine to the other guest threads (the render thread does the work the
        // query waits for), so the single-threaded serialisation doesn't starve them.
        // The Flush is only for a game that spins on a result: it is a submission
        // boundary wherever in the frame the poll happens to sit, and whether a result
        // is pending depends on timing. TM_QUERY_FLUSH: 1 (default) = flush when the same
        // query is polled again within a frame, 2 = at every pending poll (as before),
        // 0 = never.
        if (g_query_flush < 0) { const char* e = getenv("TM_QUERY_FLUSH"); g_query_flush = e && *e ? atoi(e) : 1; }
        static uint32_t last_q, last_frame = 0xFFFFFFFFu, polls;
        uint32_t fr = tm_gfx_present_count();
        if (ARG(1) == last_q && fr == last_frame) polls++; else { last_q = ARG(1); last_frame = fr; polls = 0; }
        if (g_query_flush >= 2 || (g_query_flush == 1 && polls >= 1)) { c->Flush(); g_qstat.flushes++; }
        tm_machine_unlock();
        struct timespec ts = {0, 200000}; nanosleep(&ts, NULL);
        tm_machine_lock();
    }
    RET((uint32_t)hr, 5);
}
static void ctx_Flush(void) { N<ID3D11DeviceContext>(ARG(0))->Flush(); RET(0, 1); }
static void ctx_ClearState(void) { for (auto& sl : g_ia_vb) sl = IASlot(); g_ia_ib = g_ia_ib_fmt = g_ia_ib_off = g_ia_layout = 0; N<ID3D11DeviceContext>(ARG(0))->ClearState(); RET(0, 1); }
static void ctx_GetType(void) { RET(N<ID3D11DeviceContext>(ARG(0))->GetType(), 1); }
static void ctx_GetContextFlags(void) { RET(N<ID3D11DeviceContext>(ARG(0))->GetContextFlags(), 1); }
static void ctx_SetResourceMinLOD(void) { N<ID3D11DeviceContext>(ARG(0))->SetResourceMinLOD(N<ID3D11Resource>(ARG(1)), ARGF(2)); RET(0, 3); }
static void ctx_GetResourceMinLOD(void) { float f = N<ID3D11DeviceContext>(ARG(0))->GetResourceMinLOD(N<ID3D11Resource>(ARG(1))); fp_push_impl(g_st, &g_fp_top, (double)f); RET(0, 2); }
static void ctx_ExecuteCommandList(void) { RET(0, 3); }
static void ctx_FinishCommandList(void) { if (ARG(2)) wr32(ARG(2), 0); RET(HR_FAIL, 3); }
static void ctx_CSSetUnorderedAccessViews(void) { auto* c = N<ID3D11DeviceContext>(ARG(0)); std::vector<void*> a; unwrap_array(ARG(3), ARG(2), a); c->CSSetUnorderedAccessViews(ARG(1), ARG(2), (ID3D11UnorderedAccessView**)a.data(), ARG(4) ? (UINT*)G2H(ARG(4)) : nullptr); RET(0, 5); }

// Context getters the engine may read back: fill with zeros/none (safe no-ops).
static void ctx_noop1(void) { RET(0, 1); }
static void ctx_noop2(void) { RET(0, 2); }
static void ctx_noop3(void) { RET(0, 3); }
static void ctx_noop4(void) { RET(0, 4); }
static void ctx_noop5(void) { RET(0, 5); }

#include "d3d11_bridge_tables.inc"

// ================================================================= DXGI
static void fac_EnumAdapters_impl(bool one) {
    IDXGIFactory1* f = N<IDXGIFactory1>(ARG(0));
    IDXGIAdapter* a = nullptr;
    HRESULT hr;
    if (one) hr = f->EnumAdapters1(ARG(1), (IDXGIAdapter1**)&a);
    else hr = f->EnumAdapters(ARG(1), &a);
    if (ARG(2)) wr32(ARG(2), SUCCEEDED(hr) ? wrap(a, C_DXGIADAPTER) : 0);
    RET((uint32_t)hr, 3);
}
static void fac_EnumAdapters(void) { fac_EnumAdapters_impl(false); }
static void fac_EnumAdapters1(void) { fac_EnumAdapters_impl(true); }
static void fac_MakeWindowAssociation(void) { RET(HR_OK, 3); }
static void fac_GetWindowAssociation(void) { if (ARG(1)) wr32(ARG(1), (uint32_t)(uintptr_t)tm_wsi_window_handle()); RET(HR_OK, 2); }
static void fac_GetParent(void) { if (ARG(2)) wr32(ARG(2), 0); RET(HR_NOINTERFACE, 3); }
static void fac_CreateSoftwareAdapter(void) { if (ARG(2)) wr32(ARG(2), 0); RET(HR_FAIL, 3); }
static void fac_IsCurrent(void) { RET(1, 1); }
static void fac_CreateSwapChain(void) {
    IDXGIFactory* f = N<IDXGIFactory>(ARG(0));
    IUnknown* dev = as_unknown(ARG(1));
    uint32_t gd = ARG(2);
    DXGI_SWAP_CHAIN_DESC desc{};
    // guest DXGI_SWAP_CHAIN_DESC: BufferDesc(28){W,H,RR{n,d},Fmt,SOrder,Scaling} SampleDesc(8) BufferUsage(4) BufferCount(4) OutputWindow(4) Windowed(4) SwapEffect(4) Flags(4)
    desc.BufferDesc.Width = rd32(gd + 0);
    desc.BufferDesc.Height = rd32(gd + 4);
    desc.BufferDesc.RefreshRate.Numerator = rd32(gd + 8);
    desc.BufferDesc.RefreshRate.Denominator = rd32(gd + 12);
    desc.BufferDesc.Format = (DXGI_FORMAT)rd32(gd + 16);
    desc.BufferDesc.ScanlineOrdering = (DXGI_MODE_SCANLINE_ORDER)rd32(gd + 20);
    desc.BufferDesc.Scaling = (DXGI_MODE_SCALING)rd32(gd + 24);
    desc.SampleDesc.Count = rd32(gd + 28);
    desc.SampleDesc.Quality = rd32(gd + 32);
    desc.BufferUsage = rd32(gd + 36);
    desc.BufferCount = rd32(gd + 40);
    desc.OutputWindow = (HWND)tm_wsi_window_handle();   /* the real presentation target */
    desc.Windowed = rd32(gd + 48);
    desc.SwapEffect = (DXGI_SWAP_EFFECT)rd32(gd + 52);
    desc.Flags = rd32(gd + 56);
    IDXGISwapChain* sc = nullptr;
    HRESULT hr = f->CreateSwapChain(dev, &desc, ARG(3) ? &sc : nullptr);
    TM_INFO("CreateSwapChain %ux%u fmt %u -> hr=%08X", desc.BufferDesc.Width, desc.BufferDesc.Height, desc.BufferDesc.Format, (unsigned)hr);
    if (ARG(3)) wr32(ARG(3), SUCCEEDED(hr) ? wrap(sc, C_DXGISWAPCHAIN) : 0);
    RET((uint32_t)hr, 4);
}
static void adp_EnumOutputs(void) {
    IDXGIAdapter* a = N<IDXGIAdapter>(ARG(0)); IDXGIOutput* o = nullptr;
    HRESULT hr = a->EnumOutputs(ARG(1), &o);
    if (ARG(2)) wr32(ARG(2), SUCCEEDED(hr) ? wrap(o, C_DXGIOUTPUT) : 0);
    RET((uint32_t)hr, 3);
}
static void adp_GetDesc(void) {
    auto* a = N<IDXGIAdapter>(ARG(0)); uint32_t g = ARG(1);
    if (a && g) { DXGI_ADAPTER_DESC d{}; a->GetDesc(&d);
        memcpy(G2H(g), d.Description, 256);
        uint32_t ven = 0x1002, dev = 0x6718;  /* AMD Radeon HD 6900: in GfxDevicePerfs.txt, so no benchmark */
        if (getenv("TM_GPU_VENDOR")) ven = strtoul(getenv("TM_GPU_VENDOR"), 0, 16);
        if (getenv("TM_GPU_DEVICE")) dev = strtoul(getenv("TM_GPU_DEVICE"), 0, 16);
        wr32(g + 256, ven); wr32(g + 260, dev); wr32(g + 264, d.SubSysId); wr32(g + 268, d.Revision);
        wr32(g + 272, (uint32_t)d.DedicatedVideoMemory ? (uint32_t)d.DedicatedVideoMemory : (1u<<30)); wr32(g + 276, (uint32_t)d.DedicatedSystemMemory);
        wr32(g + 280, (uint32_t)d.SharedSystemMemory);
        wr32(g + 284, d.AdapterLuid.LowPart); wr32(g + 288, (uint32_t)d.AdapterLuid.HighPart); }
    RET(0, 2); }
static void adp_GetDesc1(void) {
    auto* a = N<IDXGIAdapter1>(ARG(0)); uint32_t g = ARG(1);
    if (a && g) { DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
        memcpy(G2H(g), d.Description, 256);
        uint32_t ven = 0x1002, dev = 0x6718;
        if (getenv("TM_GPU_VENDOR")) ven = strtoul(getenv("TM_GPU_VENDOR"), 0, 16);
        if (getenv("TM_GPU_DEVICE")) dev = strtoul(getenv("TM_GPU_DEVICE"), 0, 16);
        wr32(g + 256, ven); wr32(g + 260, dev); wr32(g + 264, d.SubSysId); wr32(g + 268, d.Revision);
        wr32(g + 272, (uint32_t)d.DedicatedVideoMemory ? (uint32_t)d.DedicatedVideoMemory : (1u<<30)); wr32(g + 276, (uint32_t)d.DedicatedSystemMemory);
        wr32(g + 280, (uint32_t)d.SharedSystemMemory);
        wr32(g + 284, d.AdapterLuid.LowPart); wr32(g + 288, (uint32_t)d.AdapterLuid.HighPart); wr32(g + 292, d.Flags); }
    RET(0, 2); }
static void adp_CheckInterfaceSupport(void) { auto* a = N<IDXGIAdapter>(ARG(0)); HRESULT hr = a->CheckInterfaceSupport(*(GUID*)G2H(ARG(1)), ARG(2) ? (LARGE_INTEGER*)G2H(ARG(2)) : nullptr); RET((uint32_t)hr, 3); }
static void adp_GetParent(void) { auto* a = N<IDXGIObject>(ARG(0)); IUnknown* p = nullptr; if (a) a->GetParent(*(GUID*)G2H(ARG(1)), (void**)&p); if (ARG(2)) wr32(ARG(2), p ? wrap(p, C_DXGIFACTORY) : 0); RET(p ? HR_OK : HR_NOINTERFACE, 3); }
static void out_GetDesc(void) {
    auto* o = N<IDXGIOutput>(ARG(0)); uint32_t g = ARG(1);
    if (o && g) { DXGI_OUTPUT_DESC d{}; o->GetDesc(&d);
        memcpy(G2H(g), d.DeviceName, 64);
        wr32(g + 64, d.DesktopCoordinates.left); wr32(g + 68, d.DesktopCoordinates.top);
        wr32(g + 72, d.DesktopCoordinates.right); wr32(g + 76, d.DesktopCoordinates.bottom);
        wr32(g + 80, d.AttachedToDesktop); wr32(g + 84, d.Rotation);
        wr32(g + 88, (uint32_t)(uintptr_t)d.Monitor); }
    RET(0, 2); }
static void out_GetDisplayModeList(void) { auto* o = N<IDXGIOutput>(ARG(0)); UINT n = ARG(3) ? rd32(ARG(3)) : 0; HRESULT hr = o->GetDisplayModeList((DXGI_FORMAT)ARG(1), ARG(2), &n, ARG(4) ? (DXGI_MODE_DESC*)G2H(ARG(4)) : nullptr); if (ARG(3)) wr32(ARG(3), n); RET((uint32_t)hr, 5); }
static void out_FindClosestMatchingMode(void) { auto* o = N<IDXGIOutput>(ARG(0)); HRESULT hr = o->FindClosestMatchingMode((DXGI_MODE_DESC*)G2H(ARG(1)), (DXGI_MODE_DESC*)G2H(ARG(2)), as_unknown(ARG(3))); RET((uint32_t)hr, 4); }
static void out_ok1(void) { RET(HR_OK, 1); }
static void out_ok2(void) { RET(HR_OK, 2); }
static void out_fail2(void) { RET(HR_FAIL, 2); }
static void dxgidev_GetParent(void) { if (ARG(2)) wr32(ARG(2), 0); RET(HR_NOINTERFACE, 3); }
static void dxgidev_GetAdapter(void) { auto* d = N<IDXGIDevice>(ARG(0)); IDXGIAdapter* a = nullptr; HRESULT hr = d->GetAdapter(&a); if (ARG(1)) wr32(ARG(1), SUCCEEDED(hr) ? wrap(a, C_DXGIADAPTER) : 0); RET((uint32_t)hr, 2); }
static std::atomic<uint32_t> g_present_count{0};
// TM_GFX_STATS=<frames>: every N presented frames log the frame time and what the
// D3D11 traffic looked like (draws, maps by type, bytes copied through Map staging,
// the buffers that cost the most). Counters are always kept; they are a few adds.
struct GfxStats {
    uint32_t maps[6] = {};
    uint64_t bytes_in = 0, bytes_out = 0;
    uint32_t lazy_whole[4] = {};                   // whole-buffer flushes: 0 unknown draw kind, 1 no index data, 2 no stride/layout, 3 other use
    uint64_t lazy_range_bytes = 0, lazy_whole_bytes = 0;
    struct Top { uint32_t res, total, n; uint64_t bytes; } top[32] = {};
    void note(uint32_t res, uint32_t total, uint64_t bytes) {
        int slot = -1, min_i = 0;
        for (int i = 0; i < 32; i++) {
            if (top[i].n && top[i].res == res) { slot = i; break; }
            if (!top[i].n) { if (slot < 0) slot = i; }
            else if (!top[min_i].n || top[i].bytes < top[min_i].bytes) min_i = i;
        }
        if (slot < 0) { slot = min_i; top[slot] = Top(); }           // table full: the cheapest entry makes room
        if (!top[slot].n) top[slot].res = res;
        top[slot].total = total; top[slot].n++; top[slot].bytes += bytes;
    }
};
static GfxStats g_stats;
static int g_stats_every = -1;
static uint64_t g_stats_t0;
/* Frame time without the waits: from the end of one Present to the start of the next
   (game logic + command recording + our translation), and the time inside Present. */
static uint64_t g_last_work_ns;
// The platform can lower/restore the frame cap at run time (Android: thermal status, see
// MainActivity.onThermal). 0 = no override (TM_MAX_FPS applies).
static std::atomic<int> g_fps_override{0};
extern "C" void tm_gfx_set_max_fps(int fps) { g_fps_override.store(fps); }

/* ---- CPU placement of the render thread (Android) --------------------------------------
 * The game's frame is one thread (game logic + the draw calls through this bridge). Android's
 * scheduler leaves a thread that is busy ~50 % of the time (a 60 fps cap) on a mid core: on the
 * Odin3 (Snapdragon 8 Elite) cpu0-5 run at 3.53 GHz, cpu6-7 at 4.32 GHz -- 22 % more for a frame
 * that needs 8.5 ms against a 120 fps budget of 8.3 ms.
 *   TM_PRIME (default 1): above 60 fps the thread binds itself to the CPUs with the highest
 *     maximum clock (only when those are not all CPUs anyway);
 *   TM_ADPF (default 1): an Android performance hint session (Android 13+, libandroid,
 *     loaded at runtime) gets the frame budget as its target and every frame's work time,
 *     so the scheduler raises the clock before frames run late. */
#ifdef __ANDROID__
#include <dlfcn.h>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>
namespace {
typedef void* (*adpf_get_manager_t)();
typedef void* (*adpf_create_t)(void*, const int32_t*, size_t, int64_t);
typedef int (*adpf_update_t)(void*, int64_t);
typedef int (*adpf_report_t)(void*, int64_t);
adpf_update_t g_adpf_update; adpf_report_t g_adpf_report; void* g_adpf;
int64_t g_adpf_target;
void cpu_prime_bind() {
    const char* e = getenv("TM_PRIME");
    if (e && *e == '0') return;
    long best = 0, f[64]; int n = 0;
    for (; n < 64; n++) {
        char path[96]; snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", n);
        FILE* fp = fopen(path, "r");
        if (!fp) break;
        if (fscanf(fp, "%ld", &f[n]) != 1) f[n] = 0;
        fclose(fp);
        if (f[n] > best) best = f[n];
    }
    cpu_set_t set; CPU_ZERO(&set); int k = 0;
    for (int i = 0; i < n; i++) if (f[i] == best) { CPU_SET(i, &set); k++; }
    if (best <= 0 || k == 0 || k == n) { TM_INFO("cpu: %d CPUs, no faster cluster to bind the render thread to", n); return; }
    int r = sched_setaffinity(0, sizeof set, &set);
    TM_INFO("cpu: render thread bound to the %d fastest of %d CPUs (%ld kHz)%s", k, n, best, r ? " -- failed" : "");
}
void adpf_open(int64_t target_ns) {
    const char* e = getenv("TM_ADPF");
    if (e && *e == '0') return;
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return;
    auto get = (adpf_get_manager_t)dlsym(lib, "APerformanceHint_getManager");
    auto create = (adpf_create_t)dlsym(lib, "APerformanceHint_createSession");
    g_adpf_update = (adpf_update_t)dlsym(lib, "APerformanceHint_updateTargetWorkDuration");
    g_adpf_report = (adpf_report_t)dlsym(lib, "APerformanceHint_reportActualWorkDuration");
    if (!get || !create || !g_adpf_update || !g_adpf_report) { TM_INFO("adpf: not available"); return; }
    void* mgr = get();
    int32_t tid = (int32_t)syscall(SYS_gettid);
    g_adpf = mgr ? create(mgr, &tid, 1, target_ns) : nullptr;
    g_adpf_target = target_ns;
    TM_INFO("adpf: performance hint session %s (target %.2f ms)", g_adpf ? "open" : "refused", target_ns / 1e6);
}
}  // namespace
static void cpu_frame(uint64_t work_ns, int fps) {
    static int init;
    int64_t target = fps > 0 ? 1000000000ll / fps : 16666667ll;
    // The prime cores only for more than 60 fps: a 60 fps frame (~5 ms of work) fits the mid
    // cores, which run cooler (the panel drops to 60 Hz anyway once the device is warm).
    static int bound;
    if (!bound && fps > 60) { bound = 1; cpu_prime_bind(); }
    if (!init) { init = 1; adpf_open(target); }
    if (!g_adpf) return;
    if (target != g_adpf_target) { g_adpf_update(g_adpf, target); g_adpf_target = target; }
    if (work_ns > 0) g_adpf_report(g_adpf, (int64_t)work_ns);
}
#else
static void cpu_frame(uint64_t, int) {}
#endif
static uint64_t g_ft_sum_ns, g_ft_present_ns; static uint32_t g_ft_n; static uint32_t g_ft_hist[64];   /* 0.5 ms buckets */
static void ft_add(uint64_t work_ns, uint64_t present_ns) {
    g_ft_sum_ns += work_ns; g_ft_present_ns += present_ns; g_ft_n++;
    uint32_t b = (uint32_t)(work_ns / 500000ull); if (b > 63) b = 63; g_ft_hist[b]++;
}
static void stats_frame(void) {
    if (g_stats_every < 0) { const char* e = getenv("TM_GFX_STATS"); g_stats_every = e && *e ? (atoi(e) > 0 ? atoi(e) : 300) : 0; g_stats_t0 = tm_now_ns(); }
    if (!g_stats_every) return;
    uint32_t n = g_present_count.load(std::memory_order_relaxed);
    if (!n || n % (uint32_t)g_stats_every) return;
    uint64_t now = tm_now_ns(); double f = g_stats_every;
    TM_INFO("gfx stats: %.1f fps | per frame: %.0f draws, maps R %.1f W %.1f RW %.1f DISCARD %.1f NO_OVERWRITE %.1f, staging copy in %.0f KB out %.0f KB",
            f * 1e9 / (double)(now - g_stats_t0), g_stat_draws / f, g_stats.maps[1] / f, g_stats.maps[2] / f, g_stats.maps[3] / f,
            g_stats.maps[4] / f, g_stats.maps[5] / f, g_stats.bytes_in / f / 1024.0, g_stats.bytes_out / f / 1024.0);
    TM_INFO("gfx stats:   native Map of READ/WRITE types blocked %.2f ms per frame; %.1f staging-texture maps per frame without waiting",
            g_map_block_ns / f / 1e6, g_stg_discards / f);
    g_map_block_ns = 0; g_stg_discards = 0;
    TM_INFO("gfx stats:   lazy flush: ranges %.0f KB/frame, whole buffers %.0f KB/frame (per frame: %.1f unknown draw, %.1f no index data, %.1f no stride/layout, %.1f other); index shadows %u buffers, %.1f MB",
            g_stats.lazy_range_bytes / f / 1024.0, g_stats.lazy_whole_bytes / f / 1024.0, g_stats.lazy_whole[0] / f, g_stats.lazy_whole[1] / f,
            g_stats.lazy_whole[2] / f, g_stats.lazy_whole[3] / f, (unsigned)g_ib_shadow.size(), g_ib_shadow_bytes / 1048576.0);
    TM_INFO("gfx stats:   queries per frame: %.1f begin, %.1f end, GetData %.1f ready / %.1f pending (%.2f flushes); occlusion results %.1f (%.1f non-zero, max %llu, mean %.0f)",
            g_qstat.begin / f, g_qstat.end / f, g_qstat.ok / f, g_qstat.pending / f, g_qstat.flushes / f, g_qstat.occ_ok / f, g_qstat.occ_nonzero / f,
            (unsigned long long)g_qstat.occ_max, g_qstat.occ_ok ? (double)g_qstat.occ_sum / g_qstat.occ_ok : 0.0);
    if (g_sync_flushes) TM_INFO("gfx stats:   extra flushes: %.1f per frame", g_sync_flushes / f);
    g_sync_flushes = 0;
    if (g_rp_split > 0) TM_INFO("gfx stats:   render-pass splits (TM_RP_SPLIT=%d): %.1f per frame", g_rp_split, g_rp_splits / f);
    g_rp_splits = 0;
    if (g_map_failed) TM_INFO("gfx stats:   Map failures: %.2f per frame", g_map_failed / f);
    g_map_failed = 0;
    memset(&g_qstat, 0, sizeof g_qstat);
    for (int k = 0; k < 6; k++) {                                     // the six most expensive buffers
        int best = -1;
        for (int i = 0; i < 32; i++) if (g_stats.top[i].n && (best < 0 || g_stats.top[i].bytes > g_stats.top[best].bytes)) best = i;
        if (best < 0) break;
        TM_INFO("gfx stats:   res %08X size %u: %.1f maps/frame, %.0f KB/frame", g_stats.top[best].res, g_stats.top[best].total,
                g_stats.top[best].n / f, g_stats.top[best].bytes / f / 1024.0);
        g_stats.top[best].n = 0;
    }
    if (g_ft_n) {
        uint32_t want = (uint32_t)(g_ft_n * 0.95), acc = 0, p95 = 63;
        for (uint32_t b = 0; b < 64; b++) { acc += g_ft_hist[b]; if (acc >= want) { p95 = b; break; } }
        TM_INFO("gfx stats:   frame work %.2f ms avg, p95 %.1f ms; in Present %.2f ms avg",
                g_ft_sum_ns / (double)g_ft_n / 1e6, (p95 + 1) * 0.5, g_ft_present_ns / (double)g_ft_n / 1e6);
    }
    g_ft_sum_ns = g_ft_present_ns = 0; g_ft_n = 0; memset(g_ft_hist, 0, sizeof g_ft_hist);
    g_stats = GfxStats(); g_stat_draws = 0; g_stats_t0 = now;
}
extern "C" uint32_t tm_gfx_present_count(void) { return g_present_count.load(std::memory_order_relaxed); }
static void sc_Present(void) {
    IDXGISwapChain* s = N<IDXGISwapChain>(ARG(0));
    uint32_t sync = ARG(1), flags = ARG(2);
    // TM_SYNC_INTERVAL overrides the game's choice (it presents with 0 = "as fast as
    // possible" by default). On Android the default is 1: the queue paces the game to
    // the display instead of rendering 90 frames for a 60 Hz panel and heating the
    // device; -1 passes the game's value through.
    static int force_sync = -2;
    if (force_sync == -2) {
        const char* e = getenv("TM_SYNC_INTERVAL");
#ifdef __ANDROID__
        force_sync = e && *e ? atoi(e) : 1;
#else
        force_sync = e && *e ? atoi(e) : -1;
#endif
    }
    if (force_sync >= 0) sync = (uint32_t)force_sync;
    // TM_FRAME_LOG=<ms> (diagnostics): report frames whose work between two Presents took
    // longer, with where the main thread lost the time: waiting for the machine (another
    // guest thread had it), reading files, creating D3D objects.
    static uint64_t frame_t0;
    if (tm_diag_on()) {
        uint64_t t_in = tm_now_ns();
        uint64_t wait = tm_diag_take(0), io = tm_diag_take(1);
        if (frame_t0 && t_in - frame_t0 > (uint64_t)tm_diag_on() * 1000000ull)
            TM_INFO("long frame %u: %.1f ms of work (waiting for the machine %.1f ms, file reads %.1f ms, %u objects created, %u draws)",
                    tm_gfx_present_count(), (t_in - frame_t0) / 1e6, wait / 1e6, io / 1e6, g_frame_created, g_frame_draws);
    }
    g_frame_created = 0; g_frame_draws = 0;
    cap_frame_boundary(g_cap_ctx);
    /* Present can wait for the display: let other guest threads run meanwhile.
     * The gate holds the frame while the app is paused or has no surface. */
    tm_machine_unlock();
    uint64_t t_present = tm_now_ns();
    tm_wsi_present_begin();
    HRESULT hr = s->Present(sync, flags);
    tm_wsi_present_end();
    if (frame_t0) { g_last_work_ns = t_present - frame_t0; ft_add(t_present - frame_t0, tm_now_ns() - t_present); }
    {
        // Frame pacing. TM_MAX_FPS caps the frame rate (0 = no cap); the Android default
        // is 60 (the start screen passes its "Bildrate" setting: 120, 60 or 30). Vsync alone does not give even frames: the panel also runs at 120 Hz,
        // where the 60-95 frames the game can render alternate between one and two
        // refreshes. With the cap every frame gets the same time.
        static int max_fps = -1;
        static uint64_t next_ns;
        if (max_fps < 0) {
            const char* e = getenv("TM_MAX_FPS");
#ifdef __ANDROID__
            max_fps = e && *e ? atoi(e) : 60;
#else
            max_fps = e && *e ? atoi(e) : 0;
#endif
        }
        // Above 60 fps the cap adapts (TM_FPS_ADAPT, default on): a frame rate the game cannot
        // hold on a 120 Hz panel means frames alternately shown for one and two refreshes, which
        // the engine's motion blur turns into flicker. So: the full rate while 90 % of the frames
        // (in windows of 120) fit it, else half of it (every frame exactly two refreshes) for at
        // least 300 frames, back once 120 frames in a row fit with 10 % to spare.
        static int adapt = -1, cur_fps = 0; static uint32_t win_n, win_slow, calm_frames, dwell;
        static int base_fps = -1;
        if (adapt < 0) { const char* a = getenv("TM_FPS_ADAPT"); adapt = !(a && *a == '0'); cur_fps = max_fps; base_fps = max_fps; }
        {   // run-time cap from the platform (0 = back to the configured one)
            int ov = g_fps_override.load(), want = ov > 0 ? (base_fps > 0 ? std::min(ov, base_fps) : ov) : base_fps;
            if (want != max_fps) { TM_INFO("frame pacing: cap %d fps (%s)", want, ov > 0 ? "platform" : "restored"); max_fps = want; cur_fps = want; win_n = win_slow = calm_frames = dwell = 0; }
        }
        if (adapt && max_fps > 60) {
            uint64_t budget = 1000000000ull / (uint64_t)max_fps;
            uint64_t work = g_last_work_ns;
            if (cur_fps == max_fps) {
                win_n++; if (work > budget) win_slow++;
                if (win_n >= 120) {
                    if (win_slow * 10 > win_n) { cur_fps = max_fps / 2; calm_frames = 0; dwell = 0; TM_INFO("frame pacing: %d fps (%u of %u frames over %.1f ms)", cur_fps, win_slow, win_n, budget / 1e6); }
                    win_n = win_slow = 0;
                }
            } else {
                dwell++;
                if (work < budget - budget / 10) calm_frames++; else calm_frames = 0;
                if (dwell >= 300 && calm_frames >= 120) { cur_fps = max_fps; win_n = win_slow = 0; TM_INFO("frame pacing: back to %d fps", cur_fps); }
            }
        } else cur_fps = max_fps;
        cpu_frame(g_last_work_ns, max_fps > 0 ? max_fps : 120);   // the full target, also while paced down
        if (cur_fps > 0) {
            uint64_t iv = 1000000000ull / (uint64_t)cur_fps, now = tm_now_ns();
            if (next_ns > now && next_ns - now <= iv) {
                struct timespec ts = { 0, (long)(next_ns - now) };
                nanosleep(&ts, nullptr);
                now = next_ns;
            }
            next_ns = (next_ns && now - next_ns < iv ? next_ns : now) + iv;   // a late frame does not build up debt
        }
    }
    frame_t0 = tm_now_ns();
    tm_machine_lock();
    g_present_count.fetch_add(1, std::memory_order_relaxed);
    gfx_sync_frame();
    stats_frame();
    RET((uint32_t)hr, 3);
}
static void sc_GetBuffer(void) {
    IDXGISwapChain* s = N<IDXGISwapChain>(ARG(0)); void* out = nullptr;
    HRESULT hr = s->GetBuffer(ARG(1), *(GUID*)G2H(ARG(2)), &out);
    if (ARG(3)) wr32(ARG(3), SUCCEEDED(hr) ? wrap(out, C_TEX2D) : 0);
    RET((uint32_t)hr, 4);
}
static void sc_GetDesc(void) {
    IDXGISwapChain* s = N<IDXGISwapChain>(ARG(0));
    DXGI_SWAP_CHAIN_DESC d{}; s->GetDesc(&d);
    uint32_t g = ARG(1);
    if (g) {
        wr32(g + 0, d.BufferDesc.Width); wr32(g + 4, d.BufferDesc.Height);
        wr32(g + 8, d.BufferDesc.RefreshRate.Numerator); wr32(g + 12, d.BufferDesc.RefreshRate.Denominator);
        wr32(g + 16, d.BufferDesc.Format); wr32(g + 20, d.BufferDesc.ScanlineOrdering); wr32(g + 24, d.BufferDesc.Scaling);
        wr32(g + 28, d.SampleDesc.Count); wr32(g + 32, d.SampleDesc.Quality);
        wr32(g + 36, d.BufferUsage); wr32(g + 40, d.BufferCount);
        wr32(g + 44, (uint32_t)(uintptr_t)tm_wsi_window_handle()); wr32(g + 48, d.Windowed);
        wr32(g + 52, d.SwapEffect); wr32(g + 56, d.Flags);
    }
    RET(0, 2);
}
static void sc_ResizeBuffers(void) { IDXGISwapChain* s = N<IDXGISwapChain>(ARG(0)); HRESULT hr = s->ResizeBuffers(ARG(1), ARG(2), ARG(3), (DXGI_FORMAT)ARG(4), ARG(5)); RET((uint32_t)hr, 6); }
static void sc_ResizeTarget(void) { IDXGISwapChain* s = N<IDXGISwapChain>(ARG(0)); HRESULT hr = s->ResizeTarget((DXGI_MODE_DESC*)G2H(ARG(1))); RET((uint32_t)hr, 2); }
static void sc_GetContainingOutput(void) { IDXGISwapChain* s = N<IDXGISwapChain>(ARG(0)); IDXGIOutput* o = nullptr; HRESULT hr = s->GetContainingOutput(&o); if (ARG(1)) wr32(ARG(1), SUCCEEDED(hr) ? wrap(o, C_DXGIOUTPUT) : 0); RET((uint32_t)hr, 2); }
static void sc_SetFullscreenState(void) { RET(HR_OK, 3); }
static void sc_GetFullscreenState(void) { if (ARG(1)) wr32(ARG(1), 0); if (ARG(2)) wr32(ARG(2), 0); RET(HR_OK, 3); }
static void sc_GetFrameStatistics(void) { RET(HR_FAIL, 2); }
static void sc_GetLastPresentCount(void) { if (ARG(1)) wr32(ARG(1), 0); RET(HR_OK, 2); }
static void sc_GetDevice(void) { IDXGISwapChain* s = N<IDXGISwapChain>(ARG(0)); void* d = nullptr; HRESULT hr = s->GetDevice(*(GUID*)G2H(ARG(1)), &d); if (ARG(2)) wr32(ARG(2), SUCCEEDED(hr) ? wrap(d, C_DEVICE) : 0); RET((uint32_t)hr, 3); }
static void obj_SetPrivateData(void) { RET(HR_OK, 4); }
static void obj_SetPrivateDataInterface(void) { RET(HR_OK, 3); }
static void obj_GetPrivateData(void) { RET(HR_FAIL, 4); }

#include "d3d11_bridge_dxgi_tables.inc"

// ---------------------------------------------------------------- Map staging
// Lazy flush. The engine appends to two big dynamic buffers ("DynaVB"/"DynaIB",
// 1 MB each) with WRITE_NO_OVERWRITE dozens of times per frame, and D3D11's Map
// does not say which bytes were written. Copying the whole staging buffer back at
// every Unmap moved ~100 MB per frame (16 % of the main thread). For dynamic
// buffers that only the input assembler reads, Unmap copies nothing: each draw
// call copies exactly the vertex and index ranges it is about to use (DXVK keeps
// such buffers persistently mapped, so the host pointer stays valid after Unmap).
// Whenever a range cannot be determined, the whole buffer is copied as before.
// TM_MAP_LAZY=0 turns this off.
namespace {
struct MapRec {
    uint32_t gbuf = 0, cap = 0, total = 0; void* host = nullptr;
    bool write = false, persistent = false, valid = false;
    bool lazy = false, dirty = false;                 // lazy: flushed by draws; dirty: staging holds unflushed data
    uint32_t nfl = 0; struct { uint32_t lo, hi; } fl[6] = {};   // ranges already flushed since the last Unmap
};
std::unordered_map<uint64_t, MapRec> g_maps;
std::mutex g_map_mu;
inline uint64_t map_key(uint32_t res, uint32_t sub) { return ((uint64_t)res << 32) | sub; }
struct LazyBuf { uint32_t res; MapRec* rec; };
LazyBuf g_lazy[16];
int g_lazy_enabled = -1;
const uint32_t LAZY_MIN_BYTES = 64 * 1024;

inline MapRec* lazy_find(uint32_t res) {
    for (int i = 0; i < g_lazy_count; i++) if (g_lazy[i].res == res) return g_lazy[i].rec;
    return nullptr;
}
void lazy_copy(MapRec& m, uint64_t lo, uint64_t hi, int whole_reason = -1) {
    if (!m.dirty || !m.host || !m.gbuf) return;
    if (hi > m.total) hi = m.total;
    if (lo >= hi) return;
    for (uint32_t i = 0; i < m.nfl; i++) if (m.fl[i].lo <= lo && hi <= m.fl[i].hi) return;      // already there
    memcpy((uint8_t*)m.host + lo, (const uint8_t*)G2H(m.gbuf) + lo, (size_t)(hi - lo));
    g_stats.bytes_out += hi - lo;
    if (whole_reason >= 0) { g_stats.lazy_whole[whole_reason & 3]++; g_stats.lazy_whole_bytes += hi - lo; }
    else g_stats.lazy_range_bytes += hi - lo;
    for (uint32_t i = 0; i < m.nfl; i++) if (lo <= m.fl[i].hi && m.fl[i].lo <= hi) {             // touches a known range: grow it
        if (lo < m.fl[i].lo) m.fl[i].lo = (uint32_t)lo;
        if (hi > m.fl[i].hi) m.fl[i].hi = (uint32_t)hi;
        return;
    }
    if (m.nfl < 6) { m.fl[m.nfl].lo = (uint32_t)lo; m.fl[m.nfl].hi = (uint32_t)hi; m.nfl++; }    // else: just not remembered
}
inline void lazy_whole(MapRec& m, int reason = 3) { lazy_copy(m, 0, m.total, reason); }
}
static void lazy_flush_all(uint32_t res) {
    if (!g_lazy_count) return;
    if (MapRec* m = lazy_find(res)) lazy_whole(*m);
}
static void lazy_pre_draw_slow(int kind, uint32_t count, uint32_t start, int32_t base_vertex, uint32_t inst_count, uint32_t start_inst) {
    // Which of the bound buffers are lazy at all?
    MapRec* vb[32]; uint32_t used = 0;
    for (int sl = 0; sl < 32; sl++) { vb[sl] = g_ia_vb[sl].buf ? lazy_find(g_ia_vb[sl].buf) : nullptr; if (vb[sl] && vb[sl]->dirty) used |= 1u << sl; }
    MapRec* ib = kind != LAZY_DRAW && g_ia_ib ? lazy_find(g_ia_ib) : nullptr;
    if (!used && !(ib && ib->dirty)) return;
    if (kind == LAZY_UNKNOWN) {                       // DrawAuto / indirect: no ranges on this side
        for (int sl = 0; sl < 32; sl++) if (used & (1u << sl)) lazy_whole(*vb[sl], 0);
        if (ib) lazy_whole(*ib, 0);
        return;
    }
    int64_t vmin = start, vmax = (int64_t)start + count - 1;
    bool known = count != 0;
    if (kind == LAZY_INDEXED) {
        uint32_t isz = g_ia_ib_fmt == DXGI_FORMAT_R16_UINT ? 2 : g_ia_ib_fmt == DXGI_FORMAT_R32_UINT ? 4 : 0;
        uint64_t ilo = (uint64_t)g_ia_ib_off + (uint64_t)start * isz, ihi = ilo + (uint64_t)count * isz;
        if (ib) { if (isz) lazy_copy(*ib, ilo, ihi); else lazy_whole(*ib); }
        known = false;
        if (used && isz && count) {
            // The index values bound the vertices. They are readable when the index
            // buffer has a staging mirror of its own (it is written through Map too).
            const uint8_t* idx = nullptr; uint32_t ibtotal = 0;
            if (ib && ib->gbuf && ib->valid) { idx = (const uint8_t*)G2H(ib->gbuf); ibtotal = ib->total; }
            else {
                std::lock_guard<std::mutex> l(g_map_mu);
                auto it = g_maps.find(map_key(g_ia_ib, 0));
                if (it != g_maps.end() && it->second.gbuf && it->second.valid && it->second.persistent) { idx = (const uint8_t*)G2H(it->second.gbuf); ibtotal = it->second.total; }
                else if (!g_ib_shadow.empty()) {
                    auto sh = g_ib_shadow.find(g_ia_ib);
                    if (sh != g_ib_shadow.end()) { idx = sh->second.data(); ibtotal = (uint32_t)sh->second.size(); }
                }
            }
            if (idx && ihi <= ibtotal) {
                uint32_t mn = 0xFFFFFFFFu, mx = 0;
                if (isz == 2) { const uint16_t* p = (const uint16_t*)(idx + ilo); for (uint32_t i = 0; i < count; i++) { uint32_t v = p[i]; if (v == 0xFFFFu) continue; if (v < mn) mn = v; if (v > mx) mx = v; } }
                else          { const uint32_t* p = (const uint32_t*)(idx + ilo); for (uint32_t i = 0; i < count; i++) { uint32_t v = p[i]; if (v == 0xFFFFFFFFu) continue; if (v < mn) mn = v; if (v > mx) mx = v; } }
                if (mn <= mx) { vmin = (int64_t)base_vertex + mn; vmax = (int64_t)base_vertex + mx; known = true; }
            }
        }
    }
    if (!used) return;
    const LayoutInfo* li = nullptr;
    if (g_ia_layout) { auto it = g_layout_info.find(g_ia_layout); if (it != g_layout_info.end()) li = &it->second; }
    for (int sl = 0; sl < 32; sl++) {
        if (!(used & (1u << sl))) continue;
        MapRec& m = *vb[sl];
        uint32_t stride = g_ia_vb[sl].stride; uint64_t off = g_ia_vb[sl].offset;
        bool per_inst = li && (li->inst_mask & (1u << sl));
        if (!stride || !li) { lazy_whole(m, 2); continue; }
        if (per_inst) {
            uint32_t step = li->step[sl] ? li->step[sl] : 1;
            uint64_t n = inst_count ? (inst_count + step - 1) / step : 1;
            lazy_copy(m, off + (uint64_t)start_inst * stride, off + ((uint64_t)start_inst + n) * stride + 64);
        } else if (known) {
            if (vmin < 0) vmin = 0;
            if (vmax < vmin) continue;
            lazy_copy(m, off + (uint64_t)vmin * stride, off + ((uint64_t)vmax + 1) * stride + 64);   // +64: an element may reach past the stride
        } else lazy_whole(m, 1);
    }
}
extern "C" uint32_t tm_map_begin(uint32_t res, uint32_t sub, void* host, uint32_t total, uint32_t type, int is_buffer) {
    std::lock_guard<std::mutex> l(g_map_mu);
    MapRec& m = g_maps[map_key(res, sub)];
    bool append = type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE_NO_OVERWRITE;
    if (type != D3D11_MAP_READ) ib_shadow_drop(res);   // written through Map from now on: the staging copy is the mirror
    // A lazily flushed buffer that is suddenly used another way gets its pending data first.
    if (m.lazy && m.dirty && (!append || m.cap < total)) lazy_whole(m);
    if (m.gbuf && m.cap < total) { tm_gfree(m.gbuf); m.gbuf = 0; m.valid = false; }
    if (!m.gbuf) { m.gbuf = tm_heap_alloc(tm_heap_process(), 0, total); m.cap = total; m.valid = false; }
    if (!m.gbuf) return 0;
    bool copy_in = type == D3D11_MAP_READ || type == D3D11_MAP_WRITE || type == D3D11_MAP_READ_WRITE
                || (type == D3D11_MAP_WRITE_NO_OVERWRITE && !m.valid);
    if ((is_buffer & 8) && m.valid) copy_in = false;  // staging texture mapped as DISCARD: the kept copy IS the contents
    if (copy_in) { memcpy(G2H(m.gbuf), host, total); g_stats.bytes_in += total; }
    if (type < 6) g_stats.maps[type]++;
    if (type == D3D11_MAP_WRITE_DISCARD) { m.dirty = false; m.nfl = 0; }      // old contents are gone, nothing is owed to the new slice
    m.host = host; m.total = total; m.write = type != D3D11_MAP_READ;
    m.persistent = ((is_buffer & 1) && append) || (is_buffer & 4);
    m.valid = true;
    if (g_lazy_enabled < 0) { const char* e = getenv("TM_MAP_LAZY"); g_lazy_enabled = !(e && *e == '0'); }
    bool want_lazy = g_lazy_enabled && (is_buffer & 2) && append && sub == 0 && total >= LAZY_MIN_BYTES;
    if (want_lazy && !m.lazy && g_lazy_count < 16) { g_lazy[g_lazy_count].res = res; g_lazy[g_lazy_count].rec = &m; g_lazy_count++; m.lazy = true; }
    if (!want_lazy && m.lazy) {                       // falls back to copy-at-Unmap
        for (int i = 0; i < g_lazy_count; i++) if (g_lazy[i].res == res) { g_lazy[i] = g_lazy[--g_lazy_count]; break; }
        m.lazy = false; m.dirty = false;
    }
    return m.gbuf;
}
// Does the bridge hold the current contents of this (staging texture) subresource?
extern "C" int tm_map_shadow_valid(uint32_t res, uint32_t sub) {
    std::lock_guard<std::mutex> l(g_map_mu);
    auto it = g_maps.find(map_key(res, sub));
    return it != g_maps.end() && it->second.gbuf && it->second.valid && it->second.persistent && !it->second.lazy;
}
extern "C" void tm_map_flush(uint32_t res, uint32_t sub) {
    std::lock_guard<std::mutex> l(g_map_mu);
    auto it = g_maps.find(map_key(res, sub));
    if (it == g_maps.end()) return;
    MapRec& m = it->second;
    if (m.lazy && m.write && m.host) { m.dirty = true; m.nfl = 0; g_stats.note(res, m.total, 0); return; }   // draws copy what they need
    if (m.write && m.host && m.gbuf) { memcpy(m.host, G2H(m.gbuf), m.total); g_stats.bytes_out += m.total; g_stats.note(res, m.total, m.total); }
    m.host = nullptr;
    if (!m.persistent) { tm_gfree(m.gbuf); g_maps.erase(it); }
}
// The resource was written on the GPU side (UpdateSubresource / Copy*): a kept
// staging copy no longer mirrors it, so the next NO_OVERWRITE map re-reads it.
extern "C" void tm_map_invalidate(uint32_t res) {
    std::lock_guard<std::mutex> l(g_map_mu);
    ib_shadow_drop(res);
    if (g_maps.empty()) return;
    for (auto it = g_maps.begin(); it != g_maps.end(); ++it) if ((uint32_t)(it->first >> 32) == res) {
        if (it->second.lazy) lazy_whole(it->second);  // what the game wrote before goes in first
        it->second.valid = false; it->second.dirty = false;
    }
}
// Last reference to the resource is gone: drop its staging buffers.
extern "C" void tm_map_forget(uint32_t res) {
    std::lock_guard<std::mutex> l(g_map_mu);
    for (int i = 0; i < g_lazy_count; i++) if (g_lazy[i].res == res) { g_lazy[i] = g_lazy[--g_lazy_count]; break; }
    g_layout_info.erase(res);
    g_query_info.erase(res);
    ib_shadow_drop(res);
    for (auto it = g_maps.begin(); it != g_maps.end();) {
        if ((uint32_t)(it->first >> 32) == res) { if (it->second.gbuf) tm_gfree(it->second.gbuf); it = g_maps.erase(it); }
        else ++it;
    }
}

// ---------------------------------------------------------------- float watch
// TM_FLOAT_WATCH=<mode> / "setprop debug.tm.fwatch <mode>" (diagnostics): before every draw
// look at the constant buffers the game has bound to the vertex stage (mode 2: pixel stage
// too) and report floats that are NaN, infinite or absurdly large (|x| >= 2^27). Geometry
// that explodes into screen-filling triangles has such a number in a transform; the report
// says which shader (FNV-1a of its bytecode, tools/shader_hash.py names it), which buffer
// and which words, so the bad value can be followed back into the game's own data.
// Skeletons get a tighter check (see below), reported as "fwatch BONE"; mode 4 also dumps
// guest memory (as F11 does) the first time one fails it.
extern "C" void tm_memdump_request(void);
static void fwatch_draw(ID3D11DeviceContext* c, const char* what, uint32_t a0, uint32_t a1, uint32_t a2) {
    if (g_fwatch < 0) { const char* e = getenv("TM_FLOAT_WATCH"); g_fwatch = e && *e ? atoi(e) : 0; if (g_fwatch) TM_INFO("float watch mode %d", g_fwatch); }
    if (g_fwatch <= 0) return;
    static std::unordered_map<uint64_t, uint32_t> seen;       // shader hash, slot -> reports so far
    static uint32_t vs_hash_for; static void* vs_ptr; static uint32_t vs_size;
    for (int stage = 0; stage < (g_fwatch >= 2 ? 2 : 1); stage++) {
        const uint32_t* slots = stage ? g_cb_ps : g_cb_vs;
        for (int sl = 0; sl < 14; sl++) {
            if (!slots[sl]) continue;
            uint32_t data[1024]; uint32_t n = 0, total = 0;
            {
                std::lock_guard<std::mutex> l(g_map_mu);
                auto it = g_maps.find(map_key(slots[sl], 0));
                if (it == g_maps.end() || !it->second.gbuf || !it->second.valid) continue;
                total = it->second.total; n = total / 4; if (n > 1024) n = 1024;
                memcpy(data, G2H(it->second.gbuf), n * 4);
            }
            uint32_t nbad = 0; int first = -1;
            // Skinned objects ("ObjectV" with GbxBonesToVisuals, 3x4 floats per bone): the car
            // (3808 / 3824 bytes, 40 bones from register 14 / 15) and animated bodies (2416
            // bytes, 45 bones from register 12). A sane bone has a rotation part within
            // [-4, 4] and sits within 200 m of its object.
            uint32_t bone0 = 0, nbones = 0;
            if (!stage && sl == 0) {
                if (total == 3808) { bone0 = 56; nbones = 40; } else if (total == 3824) { bone0 = 60; nbones = 40; } else if (total == 2416) { bone0 = 48; nbones = 45; }
            }
            if (nbones) {
                for (uint32_t b = 0; b < nbones && first < 0; b++) for (uint32_t k = 0; k < 12; k++) {
                    float f; memcpy(&f, &data[bone0 + b * 12 + k], 4);
                    float lim = (k & 3) == 3 ? 200.0f : 4.0f;
                    if (!(f > -lim && f < lim)) { first = (int)(bone0 + b * 12); nbad = b; break; }
                }
                if (first >= 0) {
                    static uint32_t car_reports, dumped;
                    car_reports++;
                    if (car_reports <= 60 || (car_reports & 31) == 0) {
                        float m[12]; memcpy(m, &data[first], 48);
                        TM_INFO("fwatch BONE frame %u pass %d draw %u %s(%u,%u,%u): %u B object, bone %u = [%g %g %g %g | %g %g %g %g | %g %g %g %g] cb res %08X (report %u, guest %08X)",
                                tm_gfx_present_count(), g_pass_in_frame, g_draws_this_pass, what, a0, a1, a2, total, nbad,
                                m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], slots[sl], car_reports, g_cur_func);
                    }
                    if (g_fwatch >= 4 && !dumped) { dumped = 1; TM_INFO("fwatch: first bad bone, dumping guest memory"); tm_memdump_request(); }
                }
                continue;
            }
            for (uint32_t k = 0; k < n; k++) {
                uint32_t e = (data[k] >> 23) & 0xFF;
                if (e >= 154 && data[k] != 0xFFFFFFFFu && data[k] != 0x501502F9u) { if (first < 0) first = (int)k; nbad++; }
            }
            if (first < 0) continue;
            void* sh = nullptr;
            if (stage) { ID3D11PixelShader* ps = nullptr; c->PSGetShader(&ps, nullptr, nullptr); sh = ps; if (ps) ps->Release(); }
            else { ID3D11VertexShader* vs = nullptr; c->VSGetShader(&vs, nullptr, nullptr); sh = vs; if (vs) vs->Release(); }
            if (sh != vs_ptr) {
                vs_ptr = sh; vs_hash_for = 0; vs_size = 0;
                std::lock_guard<std::mutex> l(g_reg_mu);
                auto it = g_shader_code.find(sh);
                if (it != g_shader_code.end()) { uint32_t h = 2166136261u; for (uint8_t b : it->second) h = (h ^ b) * 16777619u; vs_hash_for = h; vs_size = (uint32_t)it->second.size(); }
            }
            uint32_t& cnt = seen[((uint64_t)vs_hash_for << 8) | (uint32_t)(stage * 16 + sl)];
            cnt++;
            if (cnt > 4 && (cnt & 127)) continue;
            char line[700]; int o = 0;
            int from = first & ~3; if (from >= 4) from -= 4;
            for (int k = from; k < from + 20 && k < (int)n && o < 640; k++) { float f; memcpy(&f, &data[k], 4); o += snprintf(line + o, sizeof line - o, k == first ? " [%g]" : " %g", f); }
            TM_INFO("fwatch frame %u pass %d draw %u %s(%u,%u,%u): %s %08X (%u B) cb%d res %08X %u B: %u bad of %u words, first word %d (%08X), words %d.. =%s  (report %u, guest %08X)",
                    tm_gfx_present_count(), g_pass_in_frame, g_draws_this_pass, what, a0, a1, a2, stage ? "PS" : "VS", vs_hash_for, vs_size, sl, slots[sl], total,
                    nbad, n, first, data[first], from, line, cnt, g_cur_func);
        }
    }
}

// ---------------------------------------------------------------- sprite probe
static void sprite_probe(ID3D11DeviceContext* c, uint32_t index_count) {
    if (g_sprite_log < 0) { const char* e = getenv("TM_SPRITE_LOG"); g_sprite_log = e && *e ? atoi(e) : 0; if (!g_sprite_log) return; }
    static uint32_t frame = 0xFFFFFFFFu, n; static float best; static float bp[4], bv[12]; static uint32_t bcount;
    uint32_t now = g_present_count.load(std::memory_order_relaxed);
    if (now != frame) {
        if (frame != 0xFFFFFFFFu && n)
            TM_INFO("sprites frame %u: %u draws, brightest SelfIllumScale %.4g (%.3g %.3g %.3g) idx %u, VisualToWorld [%.3g %.3g %.3g %.4g | %.3g %.3g %.3g %.4g | %.3g %.3g %.3g %.4g]",
                    frame, n, bp[0], bp[1], bp[2], bp[3], bcount, bv[0], bv[1], bv[2], bv[3], bv[4], bv[5], bv[6], bv[7], bv[8], bv[9], bv[10], bv[11]);
        frame = now; n = 0; best = -1e30f;
    }
    float p[4] = {}, v[12] = {};
    {
        std::lock_guard<std::mutex> l(g_map_mu);
        auto ip = g_maps.find(map_key(g_cb_ps[0], 0)), iv = g_maps.find(map_key(g_cb_vs[0], 0));
        if (ip == g_maps.end() || iv == g_maps.end() || ip->second.total != 16 || iv->second.total != 48 || !ip->second.gbuf || !iv->second.gbuf) return;
        memcpy(p, G2H(ip->second.gbuf), 16); memcpy(v, G2H(iv->second.gbuf), 48);
    }
    ID3D11BlendState* bs = nullptr; FLOAT bf[4]; UINT sm = 0;
    c->OMGetBlendState(&bs, bf, &sm);
    bool additive = false;
    if (bs) { D3D11_BLEND_DESC bd; bs->GetDesc(&bd); additive = bd.RenderTarget[0].BlendEnable && bd.RenderTarget[0].SrcBlend == D3D11_BLEND_ONE && bd.RenderTarget[0].DestBlend == D3D11_BLEND_ONE; bs->Release(); }
    if (!additive) return;
    n++;
    if (p[0] > best || p[0] != p[0]) { best = p[0]; memcpy(bp, p, 16); memcpy(bv, v, 48); bcount = index_count; }
}

// ---------------------------------------------------------------- wrap impl
static int class_for_iid(uint32_t riid) {
    if (!riid) return -1;
    // match by the well-known D3D11/DXGI IIDs we care about (first dword)
    uint32_t d1 = rd32(riid);
    switch (d1) {
    case 0xdb6f6ddb: return C_DEVICE;        // ID3D11Device
    case 0xc0bfa96c: return C_CONTEXT;       // ID3D11DeviceContext
    case 0x6f15aaf2: return C_TEX2D;         // ID3D11Texture2D
    case 0x54ec77fa: return C_DXGIDEVICE;    // IDXGIDevice
    case 0x770aae78: return C_DXGIFACTORY;   // IDXGIFactory1
    case 0x7b7166ec: return C_DXGIFACTORY;   // IDXGIFactory
    case 0x2411e7e1: return C_DXGIADAPTER;   // IDXGIAdapter
    case 0x29038f61: return C_DXGIADAPTER;   // IDXGIAdapter1
    case 0x310d36a0: return C_DXGISWAPCHAIN; // IDXGISwapChain
    default: return -1;
    }
}
static uint32_t wrap(void* native, int cls) {
    if (!native) return 0;
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_wrap.find(native);
    // Already wrapped: the incoming pointer carries the reference the native
    // getter/creator just added, and that one reference pairs with the guest
    // reference taken here (an extra AddRef would leak one native reference per
    // call and keep e.g. swap-chain buffers alive across ResizeBuffers).
    if (it != g_wrap.end()) { tm_com_addref(it->second); return it->second; }
    uint32_t obj = tm_com_new(&g_cls[cls], native);
    g_wrap[native] = obj;
    return obj;
}


// ---------------------------------------------------------------- GPU probe (diagnostics)
// TM_GFX_PROBE=1: right after device creation, draw the engine's full-screen
// triangle with pixel shaders that each output ONE input (FragCoord, a constant
// buffer element, the derived texture coordinate) and log what comes back. The
// shader blobs are derived from the game's own shaders, so they live in work/
// (generated by the capture tooling) and the probe compiles in only if present.
#if __has_include("../work/probe_shaders.inc")
#include "../work/probe_shaders.inc"
static void gfx_probe(ID3D11Device* dev) {
    ID3D11DeviceContext* c = nullptr; dev->GetImmediateContext(&c);
    D3D11_TEXTURE2D_DESC td{}; td.Width = 64; td.Height = 64; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* rt = nullptr, *st = nullptr, *src = nullptr; ID3D11RenderTargetView* rtv = nullptr; ID3D11ShaderResourceView* srv = nullptr;
    dev->CreateTexture2D(&td, nullptr, &rt); dev->CreateRenderTargetView(rt, nullptr, &rtv);
    dev->CreateTexture2D(&td, nullptr, &src); dev->CreateShaderResourceView(src, nullptr, &srv);
    D3D11_TEXTURE2D_DESC sd = td; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    dev->CreateTexture2D(&sd, nullptr, &st);
    const float tri[9] = { -1, -1, 0,  -1, 3, 0,  3, -1, 0 };
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof tri; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init{}; init.pSysMem = tri;
    ID3D11Buffer* vb = nullptr; dev->CreateBuffer(&bd, &init, &vb);
    ID3D11Buffer* cb0 = nullptr, *cb13 = nullptr;
    D3D11_BUFFER_DESC cd{}; cd.Usage = D3D11_USAGE_DYNAMIC; cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cd.ByteWidth = 32; dev->CreateBuffer(&cd, nullptr, &cb0);
    cd.ByteWidth = 528; dev->CreateBuffer(&cd, nullptr, &cb13);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(c->Map(cb0, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { const float v[8] = { 1, 2, 3, 4, 1, 1, 0, 0 }; memcpy(m.pData, v, sizeof v); c->Unmap(cb0, 0); }
    if (SUCCEEDED(c->Map(cb13, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { float v[132] = {}; v[1] = 1.0f; v[24] = 0.5f; v[25] = 0.25f; v[26] = 9; v[27] = 10; memcpy(m.pData, v, sizeof v); c->Unmap(cb13, 0); }
    const D3D11_INPUT_ELEMENT_DESC el[1] = { { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ID3D11RasterizerState* rs = nullptr; dev->CreateRasterizerState(&rd, &rs);
    D3D11_SAMPLER_DESC smd{}; smd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; smd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* smp = nullptr; dev->CreateSamplerState(&smd, &smp);
    struct { const char* name; const unsigned char* code; size_t size; } vss[] = {
        { "vs z=1", probe_vs_z1, sizeof probe_vs_z1 }, { "vs z=0", probe_vs_z0, sizeof probe_vs_z0 } };
    struct { const char* name; const unsigned char* code; size_t size; const char* expect; } pss[] = {
        { "fragcoord.xy", probe_ps_fragcoord, sizeof probe_ps_fragcoord, "(10.5,20.5,0,1) (50.5,40.5,0,1)" },
        { "cb0[0]", probe_ps_cb0, sizeof probe_ps_cb0, "(1,2,3,4)" },
        { "cb13[6]", probe_ps_cb13, sizeof probe_ps_cb13, "(0.5,0.25,9,10)" },
        { "fragcoord*cb13[6].xy+cb13[6].wz", probe_ps_uv, sizeof probe_ps_uv, "(15.25,14.125,0,1) (35.25,19.125,0,1)" } };
    for (auto& v : vss) for (auto& p : pss) {
        ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; ID3D11InputLayout* il = nullptr;
        HRESULT h1 = dev->CreateVertexShader(v.code, v.size, nullptr, &vs), h2 = dev->CreatePixelShader(p.code, p.size, nullptr, &ps);
        HRESULT h3 = dev->CreateInputLayout(el, 1, v.code, v.size, &il);
        if (FAILED(h1) || FAILED(h2) || FAILED(h3)) { TM_WARN("gfx probe %s / %s: create failed %08X %08X %08X", v.name, p.name, (unsigned)h1, (unsigned)h2, (unsigned)h3); continue; }
        const FLOAT clr[4] = { -1, -1, -1, -1 }; c->ClearRenderTargetView(rtv, clr);
        c->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{ 0, 0, 64, 64, 0, 1 }; c->RSSetViewports(1, &vp); c->RSSetState(rs);
        UINT stride = 12, off = 0; c->IASetVertexBuffers(0, 1, &vb, &stride, &off); c->IASetInputLayout(il); c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(vs, nullptr, 0); c->PSSetShader(ps, nullptr, 0);
        ID3D11Buffer* cbs[14] = {}; cbs[0] = cb0; cbs[13] = cb13; c->PSSetConstantBuffers(0, 14, cbs); c->VSSetConstantBuffers(0, 14, cbs);
        c->PSSetShaderResources(0, 1, &srv); c->PSSetSamplers(0, 1, &smp);
        c->Draw(3, 0);
        c->CopyResource(st, rt);
        if (SUCCEEDED(c->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            const float* a = (const float*)((const uint8_t*)m.pData + 20 * m.RowPitch) + 10 * 4;
            const float* b = (const float*)((const uint8_t*)m.pData + 40 * m.RowPitch) + 50 * 4;
            TM_INFO("gfx probe [%s] %-32s px(10,20)=(%g,%g,%g,%g) px(50,40)=(%g,%g,%g,%g)   expect %s", v.name, p.name, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3], p.expect);
            c->Unmap(st, 0);
        }
        vs->Release(); ps->Release(); il->Release();
    }
    c->ClearState();
    smp->Release(); rs->Release(); cb0->Release(); cb13->Release(); vb->Release(); st->Release(); srv->Release(); src->Release(); rtv->Release(); rt->Release(); c->Release();
}
#else
static void gfx_probe(ID3D11Device*) { TM_WARN("gfx probe: work/probe_shaders.inc was not present at build time"); }
#endif

// ---------------------------------------------------------------- entry points
static void shim_D3D11CreateDevice(void) {
    IDXGIAdapter* adapter = N<IDXGIAdapter>(ARG(0));
    const D3D_FEATURE_LEVEL* levels = ARG(4) ? (D3D_FEATURE_LEVEL*)G2H(ARG(4)) : nullptr;
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(adapter, (D3D_DRIVER_TYPE)ARG(1), (HMODULE)(uintptr_t)ARG(2), ARG(3),
                                   levels, ARG(5), ARG(6), ARG(7) ? &dev : nullptr, &fl, ARG(9) ? &ctx : nullptr);
    TM_INFO("D3D11CreateDevice(DXVK) -> hr=%08X feature level %04X dev=%p", (unsigned)hr, fl, (void*)dev);
    if (SUCCEEDED(hr) && dev && getenv("TM_GFX_PROBE")) gfx_probe(dev);
    if (ARG(7)) wr32(ARG(7), SUCCEEDED(hr) ? wrap(dev, C_DEVICE) : 0);
    if (ARG(8)) wr32(ARG(8), fl);
    if (ARG(9)) wr32(ARG(9), SUCCEEDED(hr) ? wrap(ctx, C_CONTEXT) : 0);
    RET((uint32_t)hr, 10);
}
static void shim_CreateDXGIFactory1(void) {
    void* f = nullptr;
    GUID iid; memcpy(&iid, G2H(ARG(0)), 16);
    HRESULT hr = CreateDXGIFactory1(iid, &f);
    if (ARG(1)) wr32(ARG(1), SUCCEEDED(hr) ? wrap(f, C_DXGIFACTORY) : 0);
    RET((uint32_t)hr, 2);
}
static void shim_CreateDXGIFactory(void) {
    void* f = nullptr;
    GUID iid; memcpy(&iid, G2H(ARG(0)), 16);
    HRESULT hr = CreateDXGIFactory(iid, &f);
    if (ARG(1)) wr32(ARG(1), SUCCEEDED(hr) ? wrap(f, C_DXGIFACTORY) : 0);
    RET((uint32_t)hr, 2);
}
static void shim_D3D11CreateDeviceAndSwapChain(void) { TM_WARN("D3D11CreateDeviceAndSwapChain unexpected"); RET(HR_FAIL, 12); }

extern "C" void tm_d3d11bridge_register(void) {
    build_classes();
    static const tm_shim_def shims[] = {
        { "d3d11.dll", "D3D11CreateDevice", shim_D3D11CreateDevice },
        { "d3d11.dll", "D3D11CreateDeviceAndSwapChain", shim_D3D11CreateDeviceAndSwapChain },
        { "dxgi.dll", "CreateDXGIFactory1", shim_CreateDXGIFactory1 },
        { "dxgi.dll", "CreateDXGIFactory", shim_CreateDXGIFactory },
    };
    tm_register_shims(shims, sizeof shims / sizeof shims[0]);
}
