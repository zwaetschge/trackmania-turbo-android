/*
 * guards.c - places where the game reads through a NULL object and the port has to
 * answer the way the game's own error handling does.
 *
 * ManiaScript member access (0x00505680, CMwNod "get parameter by path"): the script VM
 * (0x0071F140 and friends) walks a path like Obj.Sub.Member through the engine's class
 * reflection. For a member that holds another object, the member type's accessor
 * (vtable slot 0x94; for object pointers 0x005126D0, thiscall(void **field, path, out),
 * ret 0xC) reads the field and walks on into that object. It does not check the owner:
 * an owner of NULL gives field = 0 + member offset, and the read faults. In a LAN game
 * the client's playground UI scripts reach such a path (seen with the x86 client right
 * when it enters the server's race: field 0x28). 0x00505680 has an SEH frame
 * (_except_handler4, scope table 0x01A49250), but its __try only covers the other branch.
 *
 * The VM treats any non-zero result of the access as "Error during access to parameter"
 * (0x0071F225 -> 0x0071BA70): a script runtime error, not a crash. The guard returns
 * that for a field address in the NULL page instead of letting the read fault.
 */
#include "tmrt.h"
#include <stdio.h>
#include <string.h>

#define VA_MEMBER_NOD_ACCESS 0x005126D0u   /* thiscall(void **field, path, out), ret 0xC */

static recomp_func_t g_orig_member_nod;

static void guard_member_nod(void) {
    uint32_t field = rd32(g_esp + 4), path = rd32(g_esp + 8);
    if (field >= 0x10000u) { g_orig_member_nod(); return; }
    static int logged;
    if (logged++ < 20) {
        /* the path: depth byte, count byte, member infos inline at +4 (up to 2) or at [+0x14] */
        char line[200] = ""; int k = 0;
        uint32_t depth = path ? rd8(path) : 0, count = path ? rd8(path + 1) : 0;
        for (uint32_t i = 0; path && i < count && i < 6 && k < (int)sizeof line - 24; i++) {
            uint32_t mi = i < 2 ? rd32(path + 4 + 8 * i) : rd32(rd32(path + 0x14) + 8 * i - 0x10);
            k += snprintf(line + k, sizeof line - k, " %08X", mi);
        }
        TM_WARN("guard: script member access through a NULL object (field %08X, path depth %u of %u:%s) -> script error",
                field, depth, count, line);
    }
    if (rd32(g_esp + 12)) wr32(rd32(g_esp + 12), 0);           /* out = nothing */
    RET(1, 3);
}

void tm_guards_register(void) {
    g_orig_member_nod = recomp_lookup(VA_MEMBER_NOD_ACCESS);
    if (g_orig_member_nod) tm_add_override(VA_MEMBER_NOD_ACCESS, guard_member_nod);
}
