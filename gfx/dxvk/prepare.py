"""Prepare the pinned zlib-licensed DXVK native 1.9.2b (d3d11 + dxgi) for
ARM64 Android and a headless Linux host.

  prepare.py <dxvk-native checkout> <output dir>

Generic ARM64/Android adaptations follow nfsu2-recompiled's
ports/dxvk_android/prepare.py (MIT); the Direct3D 11 parts are this project's.
Every patch asserts its anchor text, so a different DXVK revision fails loudly.
"""
import argparse, hashlib, pathlib, shutil, subprocess

PIN = "c8dc91fabd00cac11d697ccf07426e798393cd40"
ap = argparse.ArgumentParser()
ap.add_argument("source", type=pathlib.Path)
ap.add_argument("output", type=pathlib.Path)
args = ap.parse_args()
rev = subprocess.check_output(["git", "-C", str(args.source), "rev-parse", "HEAD"], text=True).strip()
if rev != PIN:
    raise SystemExit("Unexpected DXVK revision: " + rev)
if args.output.exists():
    shutil.rmtree(args.output)
args.output.mkdir(parents=True)
for d in ("src", "include"):
    shutil.copytree(args.source / d, args.output / d)
shutil.copy2(args.source / "LICENSE", args.output / "LICENSE")
here = pathlib.Path(__file__).parent
nfs = here / ".." / ".." / "third_party" / "nfsu2-recompiled" / "ports" / "dxvk_android" / "include"
for h in ("vulkan_android.h", "vulkan_xlib.h"):
    shutil.copy2(nfs / h, args.output / "include/vulkan" / h)


def replace(path, old, new, count=1):
    f = args.output / path
    t = f.read_text()
    if t.count(old) < 1:
        raise SystemExit(f"Patch mismatch: {path}: {old[:60]!r}")
    f.write_text(t.replace(old, new, count) if count else t.replace(old, new))


# ---- ARM64: no x86 intrinsics -------------------------------------------
replace("src/util/util_bit.h", "#pragma once", "#pragma once\n#include <cstdint>")
replace("src/util/util_bit.h", "#ifndef _MSC_VER", "#if defined(__i386__) || defined(__x86_64__)\n#ifndef _MSC_VER")
replace("src/util/util_bit.h", '#include "util_likely.h"', '#endif // x86 intrinsics\n#include "util_likely.h"')
replace("src/util/util_bit.h", "#elif defined(__GNUC__) || defined(__clang__)\n    uint32_t res;",
        "#elif defined(__aarch64__) || defined(__arm__)\n    return n ? __builtin_ctz(n) : 32;\n    #elif defined(__GNUC__) || defined(__clang__)\n    uint32_t res;")
replace("src/util/util_bit.h", "#if defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER)", "#if defined(__i386__) || defined(__x86_64__)")
replace("src/util/sync/sync_spinlock.h", "_mm_pause();",
        '#if defined(__aarch64__) || defined(__arm__)\n        asm volatile("yield");\n#else\n        _mm_pause();\n#endif')
replace("src/util/util_vector.h", "    __m128 value = _mm_load_ps(a.data);",
        "#if defined(__aarch64__) || defined(__arm__)\n    for (unsigned i = 0; i < 4; ++i) result.data[i] = a.data[i] == a.data[i] ? a.data[i] : 0.0f;\n#else\n    __m128 value = _mm_load_ps(a.data);")
replace("src/util/util_vector.h", "    _mm_store_ps(result.data, value);", "    _mm_store_ps(result.data, value);\n#endif")

# ---- logging: everything through one host hook (logcat / runtime log) ----
replace("src/util/log/log.cpp", '#include "log.h"\n',
        '#include "log.h"\nextern "C" __attribute__((weak)) void tm_dxvk_log(int level, const char* line);\n')
replace("src/util/log/log.cpp", "        std::cerr << prefix << line << std::endl;\n",
        "        std::cerr << prefix << line << std::endl;\n        if (tm_dxvk_log) tm_dxvk_log(int(level), line.c_str());\n")

# ---- Android presents landscape images as SUBOPTIMAL every frame ----------
replace("src/d3d11/d3d11_swapchain.cpp",
        "    if (status != VK_SUCCESS)\n      RecreateSwapChain(m_vsync);",
        "    if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR)\n      RecreateSwapChain(m_vsync);")

# ---- d3d11 and dxgi live in one library here: one logger instance ---------
replace("src/dxgi/dxgi_main.cpp", '  Logger Logger::s_instance("dxgi.log");\n', '  // logger instance: see d3d11_main.cpp (single library)\n')

# DXVK caps allocations at 80% of the reported heap on unified-memory devices.
# That presumes a real heap size; lavapipe (host tests) reports a fixed 2 GB,
# less than the game uses for one track. TM_DXVK_NO_HEAP_BUDGET=1 lifts the cap.
replace("src/dxvk/dxvk_memory.cpp",
        "        m_memHeaps[i].budget = (8 * m_memProps.memoryHeaps[i].size) / 10;",
        "        m_memHeaps[i].budget = env::getEnvVar(\"TM_DXVK_NO_HEAP_BUDGET\").empty() ? (8 * m_memProps.memoryHeaps[i].size) / 10 : 0;")

# Qualcomm's proprietary driver (seen on Adreno 830, driver 512.800.72): a fragment
# shader that declares the SignedZeroInfNanPreserve execution mode
# (VK_KHR_shader_float_controls) reads gl_FragCoord as (0,0). Every full-screen
# pass of the engine derives its texture coordinates from SV_Position, so the
# whole post-processing chain collapsed to one texel. d3d11.floatControls
# therefore defaults to off on that driver (a dxvk.conf entry still overrides).
replace("src/d3d11/d3d11_options.cpp",
        "    this->floatControls         = config.getOption<bool>(\"d3d11.floatControls\", true);",
        "    this->floatControls         = config.getOption<bool>(\"d3d11.floatControls\",\n"
        "      devInfo.khrDeviceDriverProperties.driverID != VK_DRIVER_ID_QUALCOMM_PROPRIETARY_KHR);")
# ---- implicit flush at render-target changes under the bridge's control ----------
replace("src/d3d11/d3d11_context_imm.cpp", '          ID3D11DepthStencilView*           pDepthStencilView) {\n    FlushImplicit(TRUE);\n    \n    D3D11DeviceContext::OMSetRenderTargets(', '          ID3D11DepthStencilView*           pDepthStencilView) {\n    if (tm_rt_change_flush()) FlushImplicit(TRUE);\n    \n    D3D11DeviceContext::OMSetRenderTargets(')
replace("src/d3d11/d3d11_context_imm.cpp", '    const UINT*                             pUAVInitialCounts) {\n    FlushImplicit(TRUE);\n\n    D3D11DeviceContext::OMSetRenderTargetsAndUnorderedAccessViews(', '    const UINT*                             pUAVInitialCounts) {\n    if (tm_rt_change_flush()) FlushImplicit(TRUE);\n\n    D3D11DeviceContext::OMSetRenderTargetsAndUnorderedAccessViews(')
replace("src/d3d11/d3d11_context_imm.cpp", 'namespace dxvk {\n  \n  D3D11ImmediateContext::D3D11ImmediateContext(', '// tm: the bridge places the submission boundaries between render passes itself (rp_split\n// in gfx/d3d11_bridge.cpp), because on Adreno a boundary between certain passes decides\n// whether a frame comes out right. DXVK\'s timer-driven flush at a render-target change\n// would add one wherever 750 us happen to have passed. g_tm_dxvk_implicit_flush = 0 turns\n// that one off (the other implicit flushes -- query polls, busy resources -- stay);\n// TM_DXVK_IMPLICIT_FLUSH sets it, the bridge chooses the default.\nextern "C" { int g_tm_dxvk_implicit_flush = -1; }\nstatic inline bool tm_rt_change_flush() {\n  if (g_tm_dxvk_implicit_flush < 0) {\n    const char* e = getenv("TM_DXVK_IMPLICIT_FLUSH");\n    g_tm_dxvk_implicit_flush = e && *e ? atoi(e) : 1;\n  }\n  return g_tm_dxvk_implicit_flush != 0;\n}\n\nnamespace dxvk {\n  \n  D3D11ImmediateContext::D3D11ImmediateContext(')
replace("src/d3d11/d3d11_context_imm.cpp", '#include "d3d11_cmdlist.h"', '#include <cstdlib>\n#include "d3d11_cmdlist.h"')

# ---- Command buffers in flight ---------------------------------------------
# The bridge splits render passes with a Flush (one submission each) where Adreno needs it
# (gfx/d3d11_bridge.cpp, rp_split): ~40 submissions per frame in a race. DXVK 1.9 lets only
# 18 command buffers be submitted-but-unfinished; beyond that the CS thread waits for the
# GPU, so the next frame could not be recorded while the GPU drew this one and CPU and GPU
# ran one after the other (frame time = CPU + GPU, "in Present" ~ GPU time). 96 covers
# about two frames; the frame latency limit (3 frames) still bounds how far ahead the CPU runs.
replace("src/dxvk/dxvk_limits.h", "    MaxNumQueuedCommandBuffers  =    18,", "    MaxNumQueuedCommandBuffers  =    96,")

# ---- Render-pass split without a submission (TM_RP_BARRIER) --------------------
# The bridge's Adreno render-pass splits (rp_split) were Flushes: ~40 submissions per frame,
# costing CPU time and about a third of the GPU time. tm_dxvk_barrier() ends the render
# pass and records a full memory barrier instead, in the same command buffer.
replace("src/dxvk/dxvk_context.h", "    void flushCommandList();\n",
        "    void flushCommandList();\n\n    /** tm: end the render pass, full memory barrier (render-pass split without a submission) */\n    void tmFullBarrier();\n")
replace("src/dxvk/dxvk_context.cpp", "  void DxvkContext::beginQuery(const Rc<DxvkGpuQuery>& query) {",
        "  void DxvkContext::tmFullBarrier() {\n"
        "    this->spillRenderPass(true);\n"
        "    m_execBarriers.recordCommands(m_cmd);\n"
        "    this->emitMemoryBarrier(0,\n"
        "      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,\n"
        "      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);\n"
        "  }\n\n\n"
        "  void DxvkContext::beginQuery(const Rc<DxvkGpuQuery>& query) {")
replace("src/d3d11/d3d11_context_imm.h", "    void STDMETHODCALLTYPE Flush();\n",
        "    void STDMETHODCALLTYPE Flush();\n\n    void TmBarrier();\n")
replace("src/d3d11/d3d11_context_imm.cpp", "  void STDMETHODCALLTYPE D3D11ImmediateContext::Flush() {",
        "  void D3D11ImmediateContext::TmBarrier() {\n"
        "    D3D10DeviceLock lock = LockContext();\n"
        "    EmitCs([] (DxvkContext* ctx) { ctx->tmFullBarrier(); });\n"
        "  }\n\n\n"
        "  void STDMETHODCALLTYPE D3D11ImmediateContext::Flush() {")
f = args.output / "src/d3d11/d3d11_context_imm.cpp"
f.write_text(f.read_text() + '\n// tm: called by the bridge (gfx/d3d11_bridge.cpp, rp_split)\n'
             'extern "C" void tm_dxvk_barrier(ID3D11DeviceContext* c) {\n'
             '  if (c->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE) static_cast<dxvk::D3D11ImmediateContext*>(c)->TmBarrier();\n'
             '  else c->Flush();\n'
             '}\n')

# ---- Feature levels on smaller Adreno GPUs --------------------------------
# DXVK 1.9 asks for a set of Vulkan features per D3D feature level and offers the highest
# level whose set the GPU has. The Adreno 710 driver of the iPlay60 mini Turbo (driver
# 0615.84) lacks some of the 9_3..11_0 ones, so the game got level 9_2, took its low-end
# render path and crashed in it (0x007D1F40). Trackmania does not use stream output,
# multiple viewports or the other "nice to have" features below, so they are enabled when
# present and no longer decide the level. GPUs that have them all (Odin3, Adreno 830)
# behave exactly as before. The ones a GPU lacks are logged once.
D = "src/d3d11/d3d11_device.cpp"
import re
for feat in ("independentBlend", "multiViewport", "shaderImageGatherExtended", "dualSrcBlend", "imageCubeArray",
             "drawIndirectFirstInstance", "fragmentStoresAndAtomics", "multiDrawIndirect", "tessellationShader"):
    m = re.search(r"\n( +enabled\.core\.features\.%s +)= VK_TRUE;" % feat, (args.output / D).read_text())
    if not m:
        raise SystemExit("Patch mismatch: %s: %s" % (D, feat))
    replace(D, m.group(1) + "= VK_TRUE;", m.group(1) + "= supported.core.features.%s;" % feat)
replace(D, "      enabled.extTransformFeedback.transformFeedback              = VK_TRUE;\n      enabled.extTransformFeedback.geometryStreams                = VK_TRUE;",
        "      enabled.extTransformFeedback.transformFeedback              = supported.extTransformFeedback.transformFeedback;\n"
        "      enabled.extTransformFeedback.geometryStreams                = supported.extTransformFeedback.geometryStreams;")
replace(D, "      enabled.core.features.vertexPipelineStoresAndAtomics        = VK_TRUE;\n    }\n    \n    return enabled;",
        "      enabled.core.features.vertexPipelineStoresAndAtomics        = VK_TRUE;\n    }\n    \n"
        "    static bool tmLogged = false;\n"
        "    if (!tmLogged && featureLevel >= D3D_FEATURE_LEVEL_11_0) {\n"
        "      tmLogged = true;\n"
        "      std::string m;\n"
        "      const auto& f = supported.core.features;\n"
        "      if (!f.independentBlend) m += \" independentBlend\";\n"
        "      if (!f.multiViewport) m += \" multiViewport\";\n"
        "      if (!f.shaderImageGatherExtended) m += \" shaderImageGatherExtended\";\n"
        "      if (!f.dualSrcBlend) m += \" dualSrcBlend\";\n"
        "      if (!f.imageCubeArray) m += \" imageCubeArray\";\n"
        "      if (!f.drawIndirectFirstInstance) m += \" drawIndirectFirstInstance\";\n"
        "      if (!f.fragmentStoresAndAtomics) m += \" fragmentStoresAndAtomics\";\n"
        "      if (!f.multiDrawIndirect) m += \" multiDrawIndirect\";\n"
        "      if (!f.tessellationShader) m += \" tessellationShader\";\n"
        "      if (!supported.extTransformFeedback.transformFeedback) m += \" transformFeedback\";\n"
        "      if (!m.empty()) Logger::warn(str::format(\"tm: GPU lacks optional features:\", m));\n"
        "    }\n"
        "    return enabled;")

(args.output / "version.h").write_text('#define DXVK_VERSION "native-1.9.2b-tm-android"\n')
(args.output / "PINNED_REVISION").write_text(PIN + "\n")
(args.output / "PREPARE_SHA256").write_text(hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest() + "\n")
print("Prepared DXVK", PIN, "->", args.output)
