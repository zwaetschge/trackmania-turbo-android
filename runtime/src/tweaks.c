/*
 * tweaks.c - deliberate deviations from the original, each with a switch.
 *
 * TM_HUD_CALM (default 1; 0 = as the original)
 *   The game's HUD scripts flash some elements with ML::Sin(Now * 0.1): a sine with a
 *   period of 62.8 ms, i.e. a 16 Hz strobe. The best known is the TURBO badge beside the
 *   speedometer:
 *       Quad_Turbo.Opacity = 0.85 * (0.5 + 0.5 * ML::Sin(Now * 0.1));
 *   At 60 frames per second that is 3.8 samples per period, which does not read as a
 *   flash but as irregular flicker (bright, dim, gone, dim, ...). The original does the
 *   same at 60 fps; players take it for a rendering fault. With the tweak such a call
 *   returns a slow pulse between 0 and 1 instead (2.4 Hz), so the badge stays visible
 *   and breathes between half and full brightness.
 *
 *   How: the script library's Sin is a function object whose Execute method
 *   (0x0070BAA0, slot 6 of the vtable at 0x016F9394) the script VM calls indirectly, so an
 *   override can wrap it. A call belongs to a "Now * 0.1" stream when its argument is
 *   0.1 per millisecond ahead of the previous one for several frames in a row
 *   (tweaks_detect.h); of the 40-odd sines in the game's scripts only the TURBO badge
 *   moves that fast (the others use Now * 0.06 and slower).
 */
#include "tmrt.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define VA_ML_SIN_EXECUTE 0x0070BAA0u     /* thiscall Execute(ctx, args, result), ret 0xC */

#include "tweaks_detect.h"

static recomp_func_t g_orig_sin;
static uint64_t g_calm_calls;

static void hook_ml_sin(void) {
    uint32_t args = ARG(1), result = ARG(2);
    float x = 0; int calm = 0;
    uint32_t a0 = args ? rd32(args) : 0, v = a0 ? rd32(a0) : 0;
    if (v) { memcpy(&x, G2H(v + 4), 4); calm = calm_detect(x, tm_now_ns()); }
    g_orig_sin();                                   /* computes sin(x), makes the result value, returns to the VM */
    if (calm && result) {
        uint32_t obj = rd32(result);
        if (obj) {
            float r = 0.5f + 0.5f * (float)sin((double)x * 0.15);
            memcpy(G2H(obj + 4), &r, 4);
            if (!g_calm_calls++) TM_INFO("tweak: a 16 Hz script flash (Sin(Now * 0.1)) is shown as a slow pulse (TM_HUD_CALM=0 for the original)");
        }
    }
}

/* Called once the image is mapped. */
void tm_tweaks_register(void) {
    const char *e = getenv("TM_HUD_CALM");
    if (e && *e == '0') { TM_INFO("tweak: HUD flashes as in the original (TM_HUD_CALM=0)"); return; }
    g_orig_sin = recomp_lookup(VA_ML_SIN_EXECUTE);
    if (!g_orig_sin) { TM_WARN("tweak: script Sin not found, HUD flashes stay as they are"); return; }
    tm_add_override(VA_ML_SIN_EXECUTE, hook_ml_sin);
}
