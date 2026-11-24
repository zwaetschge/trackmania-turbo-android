/* shims_all.c - register every shim module. */
#include "tmrt.h"
void tm_k32_core_register(void);
void tm_k32_str_register(void);
void tm_k32_file_register(void);
void tm_misc_register(void);
void tm_d3d11bridge_register(void);
void tm_d3dcompiler_register(void);
void tm_dinput8_register(void);
void tm_net_register(void);
void tm_openal_register(void);   /* audio/openal_bridge.c, when OpenAL is linked */

void tm_shims_register_all(void) {
    tm_k32_core_register();
    tm_k32_str_register();
    tm_k32_file_register();
    tm_misc_register();
    tm_d3d11bridge_register();
    tm_d3dcompiler_register();
    tm_dinput8_register();
    tm_net_register();
#ifdef TM_HAVE_OPENAL
    tm_openal_register();
#endif
}
