/* tmcom.h - host-implemented COM objects visible to guest code. */
#ifndef TMCOM_H
#define TMCOM_H
#include "tmrt.h"

typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } tm_guid;

typedef struct {
    const char *name;
    int nmethods;
    tm_shim_fn *methods;
    const char **method_names;
    uint32_t class_id;
    uint32_t extra_bytes;
    uint32_t vtable;          /* guest VA, created on first use */
} tm_com_class;

void     tm_com_class_init(tm_com_class *c);
uint32_t tm_com_new(tm_com_class *c, void *host);
void    *tm_com_host(uint32_t obj);
uint32_t tm_com_class_of(uint32_t obj);
uint32_t tm_com_addref(uint32_t obj);
uint32_t tm_com_release(uint32_t obj, void (*destroy)(void *host));
int      tm_guid_eq(uint32_t guid_va, const tm_guid *g);
void     tm_guid_str(uint32_t guid_va, char *out, size_t cap);
extern const tm_guid TM_IID_IUnknown;

#define S_OK 0u
#define E_NOINTERFACE 0x80004002u
#define E_NOTIMPL 0x80004001u
#define E_INVALIDARG 0x80070057u
#define E_FAIL 0x80004005u
#define DXGI_ERROR_NOT_FOUND 0x887A0002u
#define DXGI_ERROR_MORE_DATA 0x887A0003u
#define DXGI_ERROR_UNSUPPORTED 0x887A0004u

#endif
