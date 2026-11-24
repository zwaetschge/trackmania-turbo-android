// gfx_selftest.cpp - renderer viability check for the Trackmania Turbo port.
//
// Runs the same steps on the Linux host (lavapipe, headless) and on the
// Android device (real GPU, activity surface):
//   1. Vulkan feature probe for the features DXVK's D3D11 feature levels need
//   2. D3D11CreateDevice through DXVK: feature level, adapter, format support
//   3. every precompiled game shader (DXBC from GpuCache) -> Create*Shader
//   4. a sample of VS/PS pairs drawn once, so the driver compiles pipelines
//   5. (with a window) a swap chain and N presented frames
// It reports facts; it never substitutes a result.
#include <d3d11_1.h>
#include <dxgi.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

extern "C" {
typedef void (*tm_gfx_emit)(const char* line);
int tm_gfx_selftest(const char* shader_pak, void* hwnd, int frames, int max_pipelines, tm_gfx_emit emit);
void tm_dxvk_log(int level, const char* line);
}

namespace {

tm_gfx_emit g_emit;
std::atomic<int> g_dxvk_errors{0}, g_dxvk_warnings{0};
std::atomic<bool> g_echo_dxvk{true};

void say(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_emit) g_emit(buf); else { fputs(buf, stdout); fputc('\n', stdout); fflush(stdout); }
}

struct Blob { std::string name; std::vector<uint8_t> data; int type = -1; /* 0 ps 1 vs 5 cs */ };

uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

// pak: repeated [u32 name_len][name][u32 size][bytes]
bool load_pak(const char* path, std::vector<Blob>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    for (;;) {
        uint32_t nl, sz;
        if (fread(&nl, 4, 1, f) != 1) break;
        Blob b; b.name.resize(nl);
        if (fread(b.name.data(), 1, nl, f) != nl || fread(&sz, 4, 1, f) != 1) break;
        b.data.resize(sz);
        if (fread(b.data.data(), 1, sz, f) != sz) break;
        out.push_back(std::move(b));
    }
    fclose(f);
    return true;
}

struct SigElem { std::string name; uint32_t index, sysval, comptype, reg; uint8_t mask; };

const uint8_t* find_chunk(const Blob& b, const char* tag, uint32_t* size) {
    const uint8_t* d = b.data.data();
    if (b.data.size() < 32 || memcmp(d, "DXBC", 4)) return nullptr;
    uint32_t n = rd32(d + 28);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t off = rd32(d + 32 + 4 * i);
        if (off + 8 > b.data.size()) return nullptr;
        if (!memcmp(d + off, tag, 4)) { *size = rd32(d + off + 4); return d + off + 8; }
    }
    return nullptr;
}

std::vector<SigElem> signature(const Blob& b, bool output) {
    static const char* in_tags[] = {"ISGN", "ISG1"};
    static const char* out_tags[] = {"OSGN", "OSG5", "OSG1"};
    std::vector<SigElem> v;
    for (int t = 0; t < (output ? 3 : 2); t++) {
        const char* tag = output ? out_tags[t] : in_tags[t];
        uint32_t size; const uint8_t* c = find_chunk(b, tag, &size);
        if (!c) continue;
        uint32_t n = rd32(c);
        uint32_t stride = tag[3] == '1' ? 32 : tag[3] == '5' ? 28 : 24;
        uint32_t lead = tag[3] == 'N' ? 0 : 4;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t* e = c + 8 + stride * i + lead;
            SigElem s;
            s.name = (const char*)(c + rd32(e));
            s.index = rd32(e + 4); s.sysval = rd32(e + 8); s.comptype = rd32(e + 12); s.reg = rd32(e + 16);
            s.mask = e[20];
            v.push_back(s);
        }
        break;
    }
    return v;
}

int program_type(const Blob& b) {
    for (const char* tag : {"SHEX", "SHDR"}) {
        uint32_t size; const uint8_t* c = find_chunk(b, tag, &size);
        if (c) return int(rd32(c) >> 16);
    }
    return -1;
}

DXGI_FORMAT layout_format(const SigElem& s) {
    int n = __builtin_popcount(s.mask & 15);
    static const DXGI_FORMAT f[3][4] = {
        {DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32B32_UINT, DXGI_FORMAT_R32G32B32A32_UINT},
        {DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R32G32_SINT, DXGI_FORMAT_R32G32B32_SINT, DXGI_FORMAT_R32G32B32A32_SINT},
        {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT}};
    int row = s.comptype == 1 ? 0 : s.comptype == 2 ? 1 : 2;
    return f[row][std::max(1, n) - 1];
}

bool is_sv(const SigElem& s) { return s.sysval != 0 || !strncasecmp(s.name.c_str(), "SV_", 3); }

void vulkan_probe() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance inst;
    if (vkCreateInstance(&ci, nullptr, &inst) != VK_SUCCESS) { say("vulkan: vkCreateInstance failed"); return; }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst, &n, devs.data());
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(devs[i], &p);
        VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(devs[i], &f);
        say("vulkan[%u]: %s, API %u.%u.%u, driver %08X, vendor %04X device %04X", i, p.deviceName,
            VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion),
            p.driverVersion, p.vendorID, p.deviceID);
        struct { const char* name; VkBool32 v; } feats[] = {
            {"geometryShader", f.geometryShader}, {"tessellationShader", f.tessellationShader},
            {"robustBufferAccess", f.robustBufferAccess},
            {"shaderStorageImageWriteWithoutFormat", f.shaderStorageImageWriteWithoutFormat},
            {"depthClamp", f.depthClamp}, {"depthBiasClamp", f.depthBiasClamp}, {"fillModeNonSolid", f.fillModeNonSolid},
            {"sampleRateShading", f.sampleRateShading}, {"shaderClipDistance", f.shaderClipDistance},
            {"shaderCullDistance", f.shaderCullDistance}, {"textureCompressionBC", f.textureCompressionBC},
            {"occlusionQueryPrecise", f.occlusionQueryPrecise}, {"independentBlend", f.independentBlend},
            {"multiViewport", f.multiViewport}, {"fullDrawIndexUint32", f.fullDrawIndexUint32},
            {"shaderImageGatherExtended", f.shaderImageGatherExtended}, {"dualSrcBlend", f.dualSrcBlend},
            {"imageCubeArray", f.imageCubeArray}, {"drawIndirectFirstInstance", f.drawIndirectFirstInstance},
            {"fragmentStoresAndAtomics", f.fragmentStoresAndAtomics}, {"multiDrawIndirect", f.multiDrawIndirect},
            {"logicOp", f.logicOp}, {"variableMultisampleRate", f.variableMultisampleRate},
            {"vertexPipelineStoresAndAtomics", f.vertexPipelineStoresAndAtomics},
            {"samplerAnisotropy", f.samplerAnisotropy}, {"textureCompressionETC2", f.textureCompressionETC2},
            {"textureCompressionASTC_LDR", f.textureCompressionASTC_LDR}};
        std::string missing, present;
        for (auto& x : feats) (x.v ? present : missing) += std::string(" ") + x.name;
        say("vulkan[%u] features present:%s", i, present.c_str());
        say("vulkan[%u] features MISSING:%s", i, missing.empty() ? " (none)" : missing.c_str());
        uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(devs[i], nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> ext(ne);
        vkEnumerateDeviceExtensionProperties(devs[i], nullptr, &ne, ext.data());
        std::string have;
        for (const char* want : {"VK_KHR_image_format_list", "VK_KHR_swapchain", "VK_EXT_transform_feedback",
                                 "VK_EXT_depth_clip_enable", "VK_EXT_robustness2", "VK_EXT_custom_border_color",
                                 "VK_EXT_vertex_attribute_divisor", "VK_EXT_host_query_reset",
                                 "VK_EXT_shader_demote_to_helper_invocation", "VK_KHR_driver_properties"}) {
            bool ok = false;
            for (auto& e : ext) if (!strcmp(e.extensionName, want)) ok = true;
            have += std::string(" ") + (ok ? "+" : "-") + (want + 3);
        }
        say("vulkan[%u] extensions (+have -missing):%s", i, have.c_str());
    }
    vkDestroyInstance(inst, nullptr);
}

template <class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() { return p; }
    operator T*() { return p; }
};

double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Block until the GPU has executed everything submitted so far.
void sync_gpu(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    Com<ID3D11Query> q;
    if (FAILED(dev->CreateQuery(&qd, &q))) { ctx->Flush(); return; }
    ctx->End(q);
    BOOL done = FALSE;
    double t0 = now_ms();
    while (ctx->GetData(q, &done, sizeof done, 0) != S_OK && now_ms() - t0 < 20000) {}
}

}  // namespace

extern "C" void tm_dxvk_log(int level, const char* line) {
    // dxvk LogLevel: Trace 0, Debug 1, Info 2, Warn 3, Error 4
    if (level >= 4) g_dxvk_errors++;
    else if (level == 3) g_dxvk_warnings++;
    if (level >= 3 && g_echo_dxvk) say("dxvk %s: %s", level >= 4 ? "ERR" : "warn", line);
}

extern "C" int tm_gfx_selftest(const char* shader_pak, void* hwnd, int frames, int max_pipelines, tm_gfx_emit emit) {
    g_emit = emit;
    g_dxvk_errors = 0; g_dxvk_warnings = 0;
    int failures = 0;
    say("== 1. Vulkan probe");
    vulkan_probe();

    say("== 2. D3D11 device through DXVK");
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                               D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3};
    Com<ID3D11Device> dev; Com<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 5, D3D11_SDK_VERSION, &dev, &got, &ctx);
    if (FAILED(hr)) {
        // 11_1 in the list makes old runtimes fail with E_INVALIDARG; retry as the game would.
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels + 1, 4, D3D11_SDK_VERSION, &dev, &got, &ctx);
    }
    if (FAILED(hr)) { say("RESULT device: D3D11CreateDevice FAILED hr=%08X", unsigned(hr)); return 1; }
    Com<IDXGIDevice> dxgidev; Com<IDXGIAdapter> adapter;
    dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgidev);
    DXGI_ADAPTER_DESC ad{};
    if (dxgidev.p && SUCCEEDED(dxgidev->GetAdapter(&adapter))) adapter->GetDesc(&ad);
    char gpu[128]; size_t gi = 0;
    for (; gi < 127 && ad.Description[gi]; gi++) gpu[gi] = char(ad.Description[gi]);
    gpu[gi] = 0;
    say("RESULT device: feature level %X.%X, adapter '%s', vendor %04X device %04X, VRAM %u MB",
        unsigned(got) >> 12, (unsigned(got) >> 8) & 15, gpu, ad.VendorId, ad.DeviceId, unsigned(ad.DedicatedVideoMemory >> 20));
    struct { const char* n; DXGI_FORMAT f; } fmts[] = {
        {"BC1", DXGI_FORMAT_BC1_UNORM}, {"BC2", DXGI_FORMAT_BC2_UNORM}, {"BC3", DXGI_FORMAT_BC3_UNORM},
        {"BC4", DXGI_FORMAT_BC4_UNORM}, {"BC5", DXGI_FORMAT_BC5_UNORM}, {"BC6H", DXGI_FORMAT_BC6H_UF16},
        {"BC7", DXGI_FORMAT_BC7_UNORM}, {"RGBA8", DXGI_FORMAT_R8G8B8A8_UNORM}, {"RGBA16F", DXGI_FORMAT_R16G16B16A16_FLOAT},
        {"R11G11B10F", DXGI_FORMAT_R11G11B10_FLOAT}, {"D24S8", DXGI_FORMAT_D24_UNORM_S8_UINT}, {"D32F", DXGI_FORMAT_D32_FLOAT}};
    std::string fs;
    for (auto& f : fmts) {
        UINT sup = 0; dev->CheckFormatSupport(f.f, &sup);
        fs += std::string(" ") + f.n + ((sup & D3D11_FORMAT_SUPPORT_TEXTURE2D) ? "=yes" : "=NO");
    }
    say("RESULT formats (Texture2D):%s", fs.c_str());

    std::vector<Blob> blobs;
    if (shader_pak && *shader_pak) {
        say("== 3. game shaders (%s)", shader_pak);
        if (!load_pak(shader_pak, blobs)) say("shader pak not readable: step skipped");
    }
    std::vector<int> vs_idx, ps_idx;
    if (!blobs.empty()) {
        int ok[6] = {0}, bad[6] = {0}, shown = 0;
        double t0 = now_ms();
        g_echo_dxvk = false;
        for (size_t i = 0; i < blobs.size(); i++) {
            Blob& b = blobs[i];
            b.type = program_type(b);
            HRESULT r = E_FAIL;
            int errs_before = g_dxvk_errors;
            if (b.type == 1) { Com<ID3D11VertexShader> s; r = dev->CreateVertexShader(b.data.data(), b.data.size(), nullptr, &s); }
            else if (b.type == 0) { Com<ID3D11PixelShader> s; r = dev->CreatePixelShader(b.data.data(), b.data.size(), nullptr, &s); }
            else if (b.type == 5) { Com<ID3D11ComputeShader> s; r = dev->CreateComputeShader(b.data.data(), b.data.size(), nullptr, &s); }
            int t = b.type >= 0 && b.type < 6 ? b.type : 2;
            if (SUCCEEDED(r) && g_dxvk_errors == errs_before) {
                ok[t]++;
                if (b.type == 1) vs_idx.push_back(int(i));
                if (b.type == 0) ps_idx.push_back(int(i));
            } else {
                bad[t]++;
                if (shown++ < 8) say("shader FAILED hr=%08X type %d: %s", unsigned(r), b.type, b.name.c_str());
            }
            if ((i + 1) % 2000 == 0) say("  ... %zu/%zu shaders", i + 1, blobs.size());
        }
        g_echo_dxvk = true;
        double dt = now_ms() - t0;
        int nbad = bad[0] + bad[1] + bad[2] + bad[5];
        failures += nbad;
        say("RESULT shaders: %d translated (vs %d, ps %d, cs %d), %d FAILED, %.1f s (%.2f ms each)",
            ok[0] + ok[1] + ok[5], ok[1], ok[0], ok[5], nbad, dt / 1000.0, dt / double(blobs.size()));
    }

    if (max_pipelines > 0 && !vs_idx.empty() && !ps_idx.empty()) {
        say("== 4. pipeline compilation on this GPU (sample of %d VS/PS pairs, one draw each)", max_pipelines);
        // offscreen targets
        D3D11_TEXTURE2D_DESC td{256, 256, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
        Com<ID3D11Texture2D> rt[4], ds, dummy; Com<ID3D11RenderTargetView> rtv[4]; Com<ID3D11DepthStencilView> dsv;
        for (int i = 0; i < 4; i++) { dev->CreateTexture2D(&td, nullptr, &rt[i]); dev->CreateRenderTargetView(rt[i], nullptr, &rtv[i]); }
        D3D11_TEXTURE2D_DESC dd = td; dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        dev->CreateTexture2D(&dd, nullptr, &ds); dev->CreateDepthStencilView(ds, nullptr, &dsv);
        D3D11_TEXTURE2D_DESC sd = td; sd.Width = sd.Height = 4; sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<uint8_t> px(4 * 4 * 4, 0x80);
        D3D11_SUBRESOURCE_DATA init{px.data(), 16, 0};
        dev->CreateTexture2D(&sd, &init, &dummy);
        Com<ID3D11ShaderResourceView> srv; dev->CreateShaderResourceView(dummy, nullptr, &srv);
        D3D11_SAMPLER_DESC smp{}; smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP; smp.MaxLOD = D3D11_FLOAT32_MAX;
        smp.ComparisonFunc = D3D11_COMPARISON_NEVER;
        Com<ID3D11SamplerState> sampler; dev->CreateSamplerState(&smp, &sampler);
        std::vector<uint8_t> zeros(65536, 0);
        D3D11_BUFFER_DESC cbd{65536, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA zi{zeros.data(), 0, 0};
        Com<ID3D11Buffer> cb, vb; dev->CreateBuffer(&cbd, &zi, &cb);
        D3D11_BUFFER_DESC vbd{65536, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
        dev->CreateBuffer(&vbd, &zi, &vb);
        ID3D11Buffer* cbs[14]; ID3D11ShaderResourceView* srvs[16]; ID3D11SamplerState* smps[16];
        for (auto& c : cbs) c = cb; for (auto& s : srvs) s = srv; for (auto& s : smps) s = sampler;
        D3D11_VIEWPORT vp{0, 0, 256, 256, 0, 1};

        // index PS by the set of non-system inputs they consume
        struct PsInfo { int idx; std::vector<std::pair<std::string, uint32_t>> in; int outputs; };
        std::vector<PsInfo> ps;
        size_t ps_step = std::max<size_t>(1, ps_idx.size() / 1500);
        for (size_t k = 0; k < ps_idx.size(); k += ps_step) {
            PsInfo p; p.idx = ps_idx[k]; p.outputs = 0;
            for (auto& e : signature(blobs[p.idx], false)) if (!is_sv(e)) p.in.push_back({e.name, e.index});
            for (auto& e : signature(blobs[p.idx], true)) if (!strcasecmp(e.name.c_str(), "SV_Target")) p.outputs = std::max(p.outputs, int(e.index) + 1);
            ps.push_back(p);
        }
        int drawn = 0, no_match = 0, layout_fail = 0;
        int errs0 = g_dxvk_errors;
        double t0 = now_ms();
        size_t vs_step = std::max<size_t>(1, vs_idx.size() / size_t(max_pipelines));
        size_t rot = 0;
        for (size_t k = 0; k < vs_idx.size() && drawn < max_pipelines; k += vs_step) {
            const Blob& v = blobs[vs_idx[k]];
            std::set<std::pair<std::string, uint32_t>> outs;
            for (auto& e : signature(v, true)) outs.insert({e.name, e.index});
            const PsInfo* pick = nullptr;
            for (size_t j = 0; j < ps.size(); j++) {
                const PsInfo& p = ps[(j + rot) % ps.size()];
                bool ok = true;
                for (auto& in : p.in) if (!outs.count(in)) { ok = false; break; }
                if (ok) { pick = &p; rot = (j + rot + 1) % ps.size(); break; }
            }
            if (!pick) { no_match++; continue; }
            std::vector<D3D11_INPUT_ELEMENT_DESC> il; std::vector<SigElem> ins = signature(v, false);
            UINT offset = 0;
            for (auto& e : ins) {
                if (is_sv(e)) continue;
                il.push_back({e.name.c_str(), e.index, layout_format(e), 0, offset, D3D11_INPUT_PER_VERTEX_DATA, 0});
                offset += 4 * UINT(std::max(1, __builtin_popcount(e.mask & 15)));
            }
            Com<ID3D11InputLayout> layout;
            if (!il.empty() && FAILED(dev->CreateInputLayout(il.data(), UINT(il.size()), v.data.data(), v.data.size(), &layout))) { layout_fail++; continue; }
            Com<ID3D11VertexShader> vsh; Com<ID3D11PixelShader> psh;
            const Blob& pb = blobs[pick->idx];
            if (FAILED(dev->CreateVertexShader(v.data.data(), v.data.size(), nullptr, &vsh)) ||
                FAILED(dev->CreatePixelShader(pb.data.data(), pb.data.size(), nullptr, &psh))) continue;
            ID3D11RenderTargetView* targets[4] = {rtv[0], rtv[1], rtv[2], rtv[3]};
            ctx->OMSetRenderTargets(UINT(std::max(1, std::min(4, pick->outputs))), targets, dsv);
            ctx->RSSetViewports(1, &vp);
            ctx->IASetInputLayout(layout);
            UINT stride = std::max<UINT>(offset, 4), zero = 0; ID3D11Buffer* vbp = vb;
            ctx->IASetVertexBuffers(0, 1, &vbp, &stride, &zero);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vsh, nullptr, 0); ctx->PSSetShader(psh, nullptr, 0);
            ctx->VSSetConstantBuffers(0, 14, cbs); ctx->PSSetConstantBuffers(0, 14, cbs);
            ctx->VSSetShaderResources(0, 16, srvs); ctx->PSSetShaderResources(0, 16, srvs);
            ctx->VSSetSamplers(0, 16, smps); ctx->PSSetSamplers(0, 16, smps);
            ctx->Draw(3, 0);
            drawn++;
            if (drawn % 16 == 0) sync_gpu(dev, ctx);
        }
        sync_gpu(dev, ctx);
        ctx->ClearState();
        int perr = g_dxvk_errors - errs0;
        failures += perr;
        say("RESULT pipelines: %d VS/PS pairs drawn, %d DXVK errors, %d VS without matching PS in sample, %d layout failures, %.1f s",
            drawn, perr, no_match, layout_fail, (now_ms() - t0) / 1000.0);
    }

    if (hwnd && frames > 0) {
        say("== 5. swap chain on the device surface, %d frames", frames);
        Com<IDXGIFactory> factory;
        if (!adapter.p || FAILED(adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory))) {
            say("RESULT present: no IDXGIFactory"); return failures + 1;
        }
        DXGI_SWAP_CHAIN_DESC sc{};
        sc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sc.BufferDesc.RefreshRate = {60, 1};
        sc.SampleDesc = {1, 0};
        sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc.BufferCount = 2;
        sc.OutputWindow = HWND(hwnd);
        sc.Windowed = TRUE;
        sc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        Com<IDXGISwapChain> chain;
        hr = factory->CreateSwapChain(dev, &sc, &chain);
        if (FAILED(hr)) { say("RESULT present: CreateSwapChain FAILED hr=%08X", unsigned(hr)); return failures + 1; }
        DXGI_SWAP_CHAIN_DESC real{}; chain->GetDesc(&real);
        Com<ID3D11Texture2D> back; chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
        Com<ID3D11RenderTargetView> brtv; dev->CreateRenderTargetView(back, nullptr, &brtv);
        int errs0 = g_dxvk_errors, presented = 0;
        double t0 = now_ms(), worst = 0, last = t0;
        for (int i = 0; i < frames; i++) {
            float t = float(i) / float(frames);
            // yellow -> blue sweep with a moving white bar, so a screenshot proves live frames
            float col[4] = {1.0f - t, 0.75f - 0.5f * t, t, 1.0f};
            ctx->ClearRenderTargetView(brtv, col);
            D3D11_RECT bar{LONG(float(real.BufferDesc.Width) * t), 0, LONG(float(real.BufferDesc.Width) * t) + 40, LONG(real.BufferDesc.Height)};
            float white[4] = {1, 1, 1, 1};
            Com<ID3D11DeviceContext1> ctx1;
            if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&ctx1)))
                ctx1->ClearView(brtv, white, &bar, 1);
            hr = chain->Present(1, 0);
            if (hr == S_OK) presented++;
            double n = now_ms(); worst = std::max(worst, n - last); last = n;
        }
        sync_gpu(dev, ctx);
        double dt = now_ms() - t0;
        int perr = g_dxvk_errors - errs0;
        failures += perr + (presented == frames ? 0 : 1);
        say("RESULT present: %dx%d, %d/%d frames presented, %.1f fps average, worst frame %.1f ms, %d DXVK errors",
            real.BufferDesc.Width, real.BufferDesc.Height, presented, frames, 1000.0 * frames / dt, worst, perr);
        // Read the rendered back buffer back to the CPU and save it, so the
        // actual pixels can be inspected (X capture of a Vulkan window is unreliable).
        if (const char* out = getenv("TM_GFX_SAVE")) {
            D3D11_TEXTURE2D_DESC bd{}; back->GetDesc(&bd);
            bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = 0; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; bd.MiscFlags = 0;
            Com<ID3D11Texture2D> stg;
            if (SUCCEEDED(dev->CreateTexture2D(&bd, nullptr, &stg))) {
                ctx->CopyResource(stg, back);
                sync_gpu(dev, ctx);
                D3D11_MAPPED_SUBRESOURCE ms{};
                if (SUCCEEDED(ctx->Map(stg, 0, D3D11_MAP_READ, 0, &ms))) {
                    FILE* f = fopen(out, "wb");
                    if (f) {
                        fprintf(f, "P6\n%u %u\n255\n", bd.Width, bd.Height);
                        for (uint32_t y = 0; y < bd.Height; y++) {
                            const uint8_t* row = (const uint8_t*)ms.pData + (size_t)y * ms.RowPitch;
                            for (uint32_t x = 0; x < bd.Width; x++) { fputc(row[x*4+0], f); fputc(row[x*4+1], f); fputc(row[x*4+2], f); }
                        }
                        fclose(f);
                        say("RESULT saved frame to %s (%ux%u)", out, bd.Width, bd.Height);
                    }
                    ctx->Unmap(stg, 0);
                }
            }
        }
    } else {
        say("== 5. swap chain: skipped (no window on this host)");
    }
    say("SELFTEST %s: %d failures, %d DXVK errors, %d DXVK warnings in total", failures ? "FAILED" : "PASSED", failures,
        int(g_dxvk_errors), int(g_dxvk_warnings));
    return failures;
}

#ifndef __ANDROID__
extern "C" void* tm_wsi_host_open(unsigned int w, unsigned int h);
int main(int argc, char** argv) {
    const char* pak = argc > 1 ? argv[1] : "";
    int maxp = argc > 2 ? atoi(argv[2]) : 64;
    int frames = argc > 3 ? atoi(argv[3]) : 120;
    void* hwnd = tm_wsi_host_open(1280, 720);   // real X window (Xvfb on CI)
    return tm_gfx_selftest(pak, hwnd, frames, maxp, nullptr) ? 1 : 0;
}
#endif
