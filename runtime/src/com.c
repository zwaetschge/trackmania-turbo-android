/*
 * com.c - guest-visible COM objects implemented by the host.
 *
 * A guest object is 16 bytes of guest memory: [vtable VA][refcount][host slot]
 * [class id]. The vtable lives in guest memory and holds synthetic shim VAs, so
 * lifted code calling `call [eax+N]` dispatches straight into host methods.
 * Methods are stdcall with `this` as ARG(0).
 */
#define _GNU_SOURCE
#include "tmrt.h"
#include "tmcom.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define MAX_SLOTS 65536
static void *g_slots[MAX_SLOTS];
static uint32_t g_nslots = 1;
static pthread_mutex_t g_com_mu = PTHREAD_MUTEX_INITIALIZER;

void tm_com_class_init(tm_com_class *c) {
    if (c->vtable) return;
    c->vtable = tm_galloc(4u * (uint32_t)c->nmethods);
    for (int i = 0; i < c->nmethods; i++) {
        char nm[96];
        snprintf(nm, sizeof nm, "%s::%s", c->name, c->method_names && c->method_names[i] ? c->method_names[i] : "?");
        char dll[64]; snprintf(dll, sizeof dll, "com.%s", c->name);
        tm_shim_def d = { dll, NULL, c->methods[i] };
        char mname[96]; snprintf(mname, sizeof mname, "%d_%s", i, c->method_names && c->method_names[i] ? c->method_names[i] : "m");
        d.name = strdup(mname);
        tm_register_shims(&d, 1);
        wr32(c->vtable + 4u * (uint32_t)i, tm_import_va(dll, d.name, 0));
    }
}

uint32_t tm_com_new(tm_com_class *c, void *host) {
    tm_com_class_init(c);
    uint32_t o = tm_galloc(16 + c->extra_bytes);
    pthread_mutex_lock(&g_com_mu);
    uint32_t slot = 0;
    for (uint32_t n = 0; n < MAX_SLOTS - 1; n++) {
        uint32_t i = 1 + (g_nslots + n) % (MAX_SLOTS - 1);
        if (!g_slots[i]) { slot = i; break; }
    }
    if (!slot) { pthread_mutex_unlock(&g_com_mu); tm_fatal("COM slot table full"); }
    g_slots[slot] = host;
    g_nslots = slot + 1;
    pthread_mutex_unlock(&g_com_mu);
    wr32(o + 0, c->vtable);
    wr32(o + 4, 1);
    wr32(o + 8, slot);
    wr32(o + 12, c->class_id);
    return o;
}

void *tm_com_host(uint32_t obj) {
    if (!obj) return NULL;
    uint32_t slot = rd32(obj + 8);
    return slot && slot < MAX_SLOTS ? g_slots[slot] : NULL;
}

uint32_t tm_com_class_of(uint32_t obj) { return obj ? rd32(obj + 12) : 0; }

uint32_t tm_com_addref(uint32_t obj) { uint32_t r = rd32(obj + 4) + 1; wr32(obj + 4, r); return r; }

uint32_t tm_com_release(uint32_t obj, void (*destroy)(void *host)) {
    uint32_t r = rd32(obj + 4) - 1;
    wr32(obj + 4, r);
    if (r == 0) {
        void *h = tm_com_host(obj);
        uint32_t slot = rd32(obj + 8);
        pthread_mutex_lock(&g_com_mu);
        if (slot < MAX_SLOTS) g_slots[slot] = NULL;
        pthread_mutex_unlock(&g_com_mu);
        if (destroy && h) destroy(h);
        tm_gfree(obj);
    }
    return r;
}

int tm_guid_eq(uint32_t guid_va, const tm_guid *g) {
    return guid_va && !memcmp(G2H(guid_va), g, 16);
}

void tm_guid_str(uint32_t guid_va, char *out, size_t cap) {
    if (!guid_va) { snprintf(out, cap, "(null)"); return; }
    const uint8_t *b = G2H(guid_va);
    uint32_t d1; uint16_t d2, d3;
    memcpy(&d1, b, 4); memcpy(&d2, b + 4, 2); memcpy(&d3, b + 6, 2);
    snprintf(out, cap, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", d1, d2, d3, b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

const tm_guid TM_IID_IUnknown = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
