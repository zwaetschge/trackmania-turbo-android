/*
 * d3dcompiler.c - D3DReflect and D3DGetInputSignatureBlob over the game's
 * precompiled DXBC shaders (GpuCache_D3D11_SM*).
 *
 * The engine reflects every shader to map its named material parameters to
 * constant-buffer offsets and its textures/samplers to bind slots, before any
 * draw. We parse the DXBC chunks (RDEF, ISGN, OSGN) and expose them through the
 * real ID3D11ShaderReflection COM interfaces.
 *
 * The DXBC bytes are copied into a guest buffer owned by the reflector, so the
 * LPCSTR Name fields we hand back point at guest-addressable memory that stays
 * valid until the reflector is released (robust if the game frees its own blob).
 */
#define _GNU_SOURCE
#include "tmrt.h"
#include "tmcom.h"
#include <stdlib.h>
#include <string.h>

#define S_OK 0u
#define E_FAIL 0x80004005u
#define E_NOTIMPL 0x80004001u

/* ------------------------------------------------------------ parsed model */
typedef struct Refl Refl;
typedef struct {
    uint32_t obj;                 /* cached COM object VA (0 = not yet created) */
    uint32_t type_obj;
    Refl *r;
    int cb, var;
} VarRef;
typedef struct {
    uint32_t obj;
    Refl *r;
    int cb;
    int nvars;
    VarRef *vars;                 /* [nvars] */
} CbufRef;
struct Refl {
    uint32_t blob;                /* guest VA of our DXBC copy */
    uint32_t size;
    uint32_t creator;             /* guest string */
    uint32_t rdef, isgn, osgn, shex, stat;   /* chunk body guest VAs (0 = none) */
    uint32_t cbuf_count, cbuf_off;           /* cbuf_off relative to rdef */
    uint32_t bind_count, bind_off;
    int var_stride;               /* 40 (SM5) or 24 (SM4) */
    uint32_t version;             /* program version token */
    uint32_t in_count, out_count;
    uint32_t self;                /* the reflector COM object VA */
    int ncbufs;
    CbufRef *cbufs;               /* [ncbufs] */
    uint32_t *type_cache;         /* dedup by rdef type offset (parallel arrays) */
    uint32_t *type_obj_cache;
    int ntypes, captypes;
};

static tm_com_class c_refl, c_cbuf, c_var, c_type, c_blob;

static uint32_t chunk(uint32_t blob, const char *tag, uint32_t *size) {
    if (memcmp(G2H(blob), "DXBC", 4)) return 0;
    uint32_t n = rd32(blob + 28);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t off = rd32(blob + 32 + 4 * i);
        if (!memcmp(G2H(blob + off), tag, 4)) { if (size) *size = rd32(blob + off + 4); return blob + off + 8; }
    }
    return 0;
}

/* ------------------------------------------------------------ type objects */
typedef struct { Refl *r; uint32_t type_va; } TypeRef;

static uint32_t type_object(Refl *r, uint32_t type_va) {
    if (!type_va) return 0;
    for (int i = 0; i < r->ntypes; i++) if (r->type_cache[i] == type_va) return r->type_obj_cache[i];
    if (r->ntypes >= r->captypes) {
        r->captypes = r->captypes ? r->captypes * 2 : 16;
        r->type_cache = realloc(r->type_cache, r->captypes * 4);
        r->type_obj_cache = realloc(r->type_obj_cache, r->captypes * 4);
    }
    TypeRef *t = calloc(1, sizeof *t); t->r = r; t->type_va = type_va;
    uint32_t obj = tm_com_new(&c_type, t);
    r->type_cache[r->ntypes] = type_va; r->type_obj_cache[r->ntypes] = obj; r->ntypes++;
    return obj;
}

static void type_GetDesc(void) {
    TypeRef *t = tm_com_host(ARG(0)); uint32_t p = ARG(1);
    if (!t || !p) RET(E_FAIL, 2);
    uint32_t tv = t->type_va, rdef = t->r->rdef;
    memset(G2H(p), 0, 32);
    wr32(p + 0, rd16(tv + 0));           /* Class */
    wr32(p + 4, rd16(tv + 2));           /* Type */
    wr32(p + 8, rd16(tv + 4));           /* Rows */
    wr32(p + 12, rd16(tv + 6));          /* Columns */
    wr32(p + 16, rd16(tv + 8));          /* Elements */
    wr32(p + 20, rd16(tv + 10));         /* Members */
    wr32(p + 24, 0);                     /* Offset */
    uint32_t nameOff = rd32(tv + 16);    /* SM5 type name offset (relative to rdef) */
    wr32(p + 28, nameOff ? rdef + nameOff : 0);
    RET(S_OK, 2);
}
static void type_ret0(void) { RET(0, 1); }
static void type_ret0_2(void) { RET(0, 2); }
static void type_ret_fail(void) { RET(E_FAIL, 2); }
static tm_shim_fn type_methods[] = {
    type_GetDesc, type_ret0_2 /*GetMemberTypeByIndex*/, type_ret0_2 /*ByName*/, type_ret0_2 /*GetMemberTypeName*/,
    type_ret_fail /*IsEqual*/, type_ret0 /*GetSubType*/, type_ret0 /*GetBaseClass*/, type_ret0 /*GetNumInterfaces*/,
    type_ret0_2 /*GetInterfaceByIndex*/, type_ret_fail /*IsOfType*/, type_ret_fail /*ImplementsInterface*/ };

/* ------------------------------------------------------------ variables */
static uint32_t var_entry(Refl *r, int cb, int var) {
    uint32_t cbe = r->rdef + r->cbuf_off + (uint32_t)cb * 24;
    uint32_t varOff = rd32(cbe + 8);
    return r->rdef + varOff + (uint32_t)var * (uint32_t)r->var_stride;
}
static void var_GetDesc(void) {
    VarRef *v = tm_com_host(ARG(0)); uint32_t p = ARG(1);
    if (!v || !p) RET(E_FAIL, 2);
    uint32_t ve = var_entry(v->r, v->cb, v->var), rdef = v->r->rdef;
    memset(G2H(p), 0, 36);
    wr32(p + 0, rdef + rd32(ve + 0));    /* Name */
    wr32(p + 4, rd32(ve + 4));           /* StartOffset */
    wr32(p + 8, rd32(ve + 8));           /* Size */
    wr32(p + 12, rd32(ve + 12));         /* uFlags */
    uint32_t dv = rd32(ve + 20);         /* DefaultValue offset */
    wr32(p + 16, dv ? rdef + dv : 0);
    if (v->r->var_stride >= 40) {
        wr32(p + 20, rd32(ve + 24));     /* StartTexture */
        wr32(p + 24, rd32(ve + 28));     /* TextureSize */
        wr32(p + 28, rd32(ve + 32));     /* StartSampler */
        wr32(p + 32, rd32(ve + 36));     /* SamplerSize */
    } else {
        wr32(p + 20, 0xFFFFFFFFu); wr32(p + 28, 0xFFFFFFFFu);
    }
    RET(S_OK, 2);
}
static void var_GetType(void) {
    VarRef *v = tm_com_host(ARG(0));
    if (!v) RET(0, 1);
    if (!v->type_obj) {
        uint32_t ve = var_entry(v->r, v->cb, v->var);
        v->type_obj = type_object(v->r, v->r->rdef + rd32(ve + 16));
    }
    RET(v->type_obj, 1);
}
static void var_GetBuffer(void) {
    VarRef *v = tm_com_host(ARG(0));
    RET(v ? v->r->cbufs[v->cb].obj : 0, 1);
}
static void var_GetInterfaceSlot(void) { RET(0xFFFFFFFFu, 2); }
static tm_shim_fn var_methods[] = { var_GetDesc, var_GetType, var_GetBuffer, var_GetInterfaceSlot };

/* ------------------------------------------------------------ constant buffers */
static uint32_t cbuf_var(CbufRef *c, int idx) {
    if (idx < 0 || idx >= c->nvars) return 0;
    VarRef *v = &c->vars[idx];
    if (!v->obj) { v->r = c->r; v->cb = c->cb; v->var = idx; v->obj = tm_com_new(&c_var, v); }
    return v->obj;
}
static void cbuf_GetDesc(void) {
    CbufRef *c = tm_com_host(ARG(0)); uint32_t p = ARG(1);
    if (!c || !p) RET(E_FAIL, 2);
    uint32_t cbe = c->r->rdef + c->r->cbuf_off + (uint32_t)c->cb * 24;
    memset(G2H(p), 0, 20);
    wr32(p + 0, c->r->rdef + rd32(cbe + 0));   /* Name */
    wr32(p + 4, rd32(cbe + 20));               /* Type */
    wr32(p + 8, rd32(cbe + 4));                /* Variables */
    wr32(p + 12, rd32(cbe + 12));              /* Size */
    wr32(p + 16, rd32(cbe + 16));              /* uFlags */
    RET(S_OK, 2);
}
static void cbuf_GetVariableByIndex(void) {
    CbufRef *c = tm_com_host(ARG(0));
    RET(c ? cbuf_var(c, (int)ARG(1)) : 0, 2);
}
static void cbuf_GetVariableByName(void) {
    CbufRef *c = tm_com_host(ARG(0));
    if (!c || !ARG(1)) RET(0, 2);
    const char *want = G2H(ARG(1));
    for (int i = 0; i < c->nvars; i++) {
        uint32_t ve = var_entry(c->r, c->cb, i);
        if (!strcmp((const char *)G2H(c->r->rdef + rd32(ve + 0)), want)) RET(cbuf_var(c, i), 2);
    }
    RET(0, 2);  /* real d3dcompiler returns an invalid-variable singleton; engines name-check first */
}
static tm_shim_fn cbuf_methods[] = { cbuf_GetDesc, cbuf_GetVariableByIndex, cbuf_GetVariableByName };

static uint32_t refl_cbuf(Refl *r, int idx) {
    if (idx < 0 || idx >= r->ncbufs) return 0;
    CbufRef *c = &r->cbufs[idx];
    if (!c->obj) {
        c->r = r; c->cb = idx;
        uint32_t cbe = r->rdef + r->cbuf_off + (uint32_t)idx * 24;
        c->nvars = (int)rd32(cbe + 4);
        c->vars = calloc((size_t)(c->nvars > 0 ? c->nvars : 1), sizeof(VarRef));
        c->obj = tm_com_new(&c_cbuf, c);
    }
    return c->obj;
}

/* ------------------------------------------------------------ shader reflection */
static void write_sig(uint32_t sig_body, uint32_t idx, uint32_t out) {
    /* ISGN/OSGN: u32 count, u32 reserved, then count × 24-byte elements. */
    uint32_t e = sig_body + 8 + idx * 24;
    memset(G2H(out), 0, 32);
    wr32(out + 0, sig_body + rd32(e + 0));   /* SemanticName (ptr into chunk) */
    wr32(out + 4, rd32(e + 4));              /* SemanticIndex */
    wr32(out + 12, rd32(e + 8));             /* SystemValueType */
    wr32(out + 16, rd32(e + 12));            /* ComponentType */
    wr32(out + 8, rd32(e + 16));             /* Register */
    wr8(out + 20, rd8(e + 20));              /* Mask */
    wr8(out + 21, rd8(e + 21));              /* ReadWriteMask */
    /* Stream (24) and MinPrecision (28) left 0 */
}

static void refl_GetDesc(void) {
    Refl *r = tm_com_host(ARG(0)); uint32_t p = ARG(1);
    if (!r || !p) RET(E_FAIL, 2);
    memset(G2H(p), 0, 168);
    wr32(p + 0, r->version);          /* Version */
    wr32(p + 4, r->creator);          /* Creator */
    wr32(p + 12, r->cbuf_count);      /* ConstantBuffers */
    wr32(p + 16, r->bind_count);      /* BoundResources */
    wr32(p + 20, r->in_count);        /* InputParameters */
    wr32(p + 24, r->out_count);       /* OutputParameters */
    RET(S_OK, 2);
}
static void refl_GetConstantBufferByIndex(void) {
    Refl *r = tm_com_host(ARG(0));
    RET(r ? refl_cbuf(r, (int)ARG(1)) : 0, 2);
}
static void refl_GetConstantBufferByName(void) {
    Refl *r = tm_com_host(ARG(0));
    if (!r || !ARG(1)) RET(0, 2);
    const char *want = G2H(ARG(1));
    for (uint32_t i = 0; i < r->cbuf_count; i++) {
        uint32_t cbe = r->rdef + r->cbuf_off + i * 24;
        if (!strcmp((const char *)G2H(r->rdef + rd32(cbe + 0)), want)) RET(refl_cbuf(r, (int)i), 2);
    }
    RET(0, 2);
}
static void write_bind(Refl *r, uint32_t i, uint32_t out) {
    uint32_t e = r->rdef + r->bind_off + i * 32;
    memset(G2H(out), 0, 32);
    wr32(out + 0, r->rdef + rd32(e + 0));   /* Name */
    wr32(out + 4, rd32(e + 4));             /* Type */
    wr32(out + 8, rd32(e + 20));            /* BindPoint */
    wr32(out + 12, rd32(e + 24));           /* BindCount */
    wr32(out + 16, rd32(e + 28));           /* uFlags */
    wr32(out + 20, rd32(e + 8));            /* ReturnType */
    wr32(out + 24, rd32(e + 12));           /* Dimension */
    wr32(out + 28, rd32(e + 16));           /* NumSamples */
}
static void refl_GetResourceBindingDesc(void) {
    Refl *r = tm_com_host(ARG(0)); uint32_t i = ARG(1), p = ARG(2);
    if (!r || i >= r->bind_count || !p) RET(E_FAIL, 3);
    write_bind(r, i, p);
    RET(S_OK, 3);
}
static void refl_GetResourceBindingDescByName(void) {
    Refl *r = tm_com_host(ARG(0)); uint32_t p = ARG(2);
    if (!r || !ARG(1) || !p) RET(E_FAIL, 3);
    const char *want = G2H(ARG(1));
    for (uint32_t i = 0; i < r->bind_count; i++) {
        uint32_t e = r->rdef + r->bind_off + i * 32;
        if (!strcmp((const char *)G2H(r->rdef + rd32(e + 0)), want)) { write_bind(r, i, p); RET(S_OK, 3); }
    }
    RET(E_FAIL, 3);
}
static void refl_GetInputParameterDesc(void) {
    Refl *r = tm_com_host(ARG(0)); uint32_t i = ARG(1), p = ARG(2);
    if (!r || !r->isgn || i >= r->in_count || !p) RET(E_FAIL, 3);
    write_sig(r->isgn, i, p);
    RET(S_OK, 3);
}
static void refl_GetOutputParameterDesc(void) {
    Refl *r = tm_com_host(ARG(0)); uint32_t i = ARG(1), p = ARG(2);
    if (!r || !r->osgn || i >= r->out_count || !p) RET(E_FAIL, 3);
    write_sig(r->osgn, i, p);
    RET(S_OK, 3);
}
static void refl_qi(void) { if (ARG(2)) wr32(ARG(2), ARG(0)); RET(S_OK, 3); }
static void refl_addref(void) { RET(tm_com_addref(ARG(0)), 1); }
static void refl_null3(void) { RET(0, 3); }
static void refl_var_by_name(void) { RET(0, 2); }
static void refl_u0_0(void) { RET(0, 1); }
static void refl_prim(void) { RET(0, 1); }
static void refl_minfl(void) { if (ARG(1)) wr32(ARG(1), 0xb000); RET(S_OK, 2); }
static void refl_tgsize(void) { if (ARG(1)) wr32(ARG(1), 1); if (ARG(2)) wr32(ARG(2), 1); if (ARG(3)) wr32(ARG(3), 1); RET(1, 4); }
static void refl_requires(void) { g_edx = 0; RET(0, 1); }
static void refl_release(void);
static tm_shim_fn refl_methods[] = {
    refl_qi, refl_addref, refl_release,
    refl_GetDesc, refl_GetConstantBufferByIndex, refl_GetConstantBufferByName, refl_GetResourceBindingDesc,
    refl_GetInputParameterDesc, refl_GetOutputParameterDesc, refl_null3 /*PatchConstant*/, refl_var_by_name,
    refl_GetResourceBindingDescByName, refl_u0_0 /*Mov*/, refl_u0_0 /*Movc*/, refl_u0_0 /*Conversion*/,
    refl_u0_0 /*Bitwise*/, refl_prim /*GSInputPrimitive*/, refl_u0_0 /*IsSampleFrequency*/, refl_u0_0 /*NumInterfaceSlots*/,
    refl_minfl, refl_tgsize, refl_requires };

static void refl_destroy(void *host) {
    Refl *r = host;
    if (!r) return;
    /* cbuffer and variable host structs are interior to r->cbufs / c->vars, so
     * only their guest COM objects are released here; the arrays are freed whole.
     * Type host structs are separately allocated and are freed individually. */
    for (int i = 0; i < r->ncbufs; i++) {
        CbufRef *c = &r->cbufs[i];
        for (int j = 0; j < c->nvars; j++) if (c->vars[j].obj) tm_com_release(c->vars[j].obj, NULL);
        free(c->vars);
        if (c->obj) tm_com_release(c->obj, NULL);
    }
    for (int i = 0; i < r->ntypes; i++) { void *th = tm_com_host(r->type_obj_cache[i]); tm_com_release(r->type_obj_cache[i], NULL); free(th); }
    free(r->cbufs); free(r->type_cache); free(r->type_obj_cache);
    if (r->blob) tm_gfree(r->blob);
    if (r->creator) tm_gfree(r->creator);
    free(r);
}
static void refl_release(void) { RET(tm_com_release(ARG(0), refl_destroy), 1); }

/* ------------------------------------------------------------ D3DReflect */
static void shim_D3DReflect(void) {
    uint32_t src = ARG(0), size = ARG(1), ppout = ARG(3);
    if (!src || size < 32 || memcmp(G2H(src), "DXBC", 4)) { if (ppout) wr32(ppout, 0); RET(E_FAIL, 4); }
    Refl *r = calloc(1, sizeof *r);
    r->size = size;
    r->blob = tm_galloc(size);
    memcpy(G2H(r->blob), G2H(src), size);
    r->rdef = chunk(r->blob, "RDEF", NULL);
    uint32_t isz, osz;
    r->isgn = chunk(r->blob, "ISGN", &isz);
    r->osgn = chunk(r->blob, "OSGN", &osz);
    r->shex = chunk(r->blob, "SHEX", NULL);
    if (!r->shex) r->shex = chunk(r->blob, "SHDR", NULL);
    r->stat = chunk(r->blob, "STAT", NULL);
    if (r->rdef) {
        r->cbuf_count = rd32(r->rdef + 0);
        r->cbuf_off = rd32(r->rdef + 4);
        r->bind_count = rd32(r->rdef + 8);
        r->bind_off = rd32(r->rdef + 12);
        uint8_t major = rd8(r->rdef + 17);
        r->var_stride = major >= 5 ? 40 : 24;
        /* RDEF creator string */
        uint32_t crOff = rd32(r->rdef + 24);
        if (crOff) r->creator = tm_gstrdup((const char *)G2H(r->rdef + crOff));
    }
    r->version = r->shex ? rd32(r->shex) : 0xFFFF0400u;
    r->in_count = r->isgn ? rd32(r->isgn) : 0;
    r->out_count = r->osgn ? rd32(r->osgn) : 0;
    r->ncbufs = (int)r->cbuf_count;
    r->cbufs = calloc((size_t)(r->ncbufs > 0 ? r->ncbufs : 1), sizeof(CbufRef));
    r->self = tm_com_new(&c_refl, r);
    if (ppout) wr32(ppout, r->self);
    TM_INFO("D3DReflect: %u cbuffers, %u bindings, %u in, %u out (prog %08X)",
            r->cbuf_count, r->bind_count, r->in_count, r->out_count, r->version);
    RET(S_OK, 4);
}

/* ------------------------------------------------------------ ID3DBlob */
typedef struct { uint32_t data; uint32_t size; } BlobObj;
static void blob_qi(void) { if (ARG(2)) wr32(ARG(2), ARG(0)); RET(S_OK, 3); }
static void blob_addref(void) { RET(tm_com_addref(ARG(0)), 1); }
static void blob_destroy(void *host) { BlobObj *b = host; if (b) { if (b->data) tm_gfree(b->data); free(b); } }
static void blob_release(void) { RET(tm_com_release(ARG(0), blob_destroy), 1); }
static void blob_GetBufferPointer(void) { BlobObj *b = tm_com_host(ARG(0)); RET(b ? b->data : 0, 1); }
static void blob_GetBufferSize(void) { BlobObj *b = tm_com_host(ARG(0)); RET(b ? b->size : 0, 1); }
static tm_shim_fn blob_methods[] = { blob_qi, blob_addref, blob_release, blob_GetBufferPointer, blob_GetBufferSize };

static uint32_t make_blob(const void *data, uint32_t size) {
    BlobObj *b = calloc(1, sizeof *b);
    b->size = size; b->data = tm_galloc(size ? size : 1);
    if (size) memcpy(G2H(b->data), data, size);
    return tm_com_new(&c_blob, b);
}

/* D3DGetInputSignatureBlob: return a DXBC container holding just the ISGN chunk
 * (what CreateInputLayout consumes). */
static void shim_D3DGetInputSignatureBlob(void) {
    uint32_t src = ARG(0), size = ARG(1), ppout = ARG(2);
    if (!src || size < 32 || memcmp(G2H(src), "DXBC", 4)) { if (ppout) wr32(ppout, 0); RET(E_FAIL, 3); }
    uint32_t isz = 0, isgn = chunk(src, "ISGN", &isz);
    if (!isgn) { if (ppout) wr32(ppout, 0); RET(E_FAIL, 3); }
    /* DXBC(20) + 4 (dirtable:1) + chunk header(8) + body */
    uint32_t total = 32 + 4 + 8 + isz;
    uint8_t *buf = malloc(total);
    memset(buf, 0, 32);
    memcpy(buf, "DXBC", 4);
    *(uint32_t *)(buf + 24) = total;
    *(uint32_t *)(buf + 28) = 1;               /* chunk count */
    *(uint32_t *)(buf + 32) = 36;              /* chunk[0] offset */
    memcpy(buf + 36, "ISGN", 4);
    *(uint32_t *)(buf + 40) = isz;
    memcpy(buf + 44, G2H(isgn), isz);
    uint32_t blob = make_blob(buf, total);
    free(buf);
    if (ppout) wr32(ppout, blob);
    RET(S_OK, 3);
}

static const tm_shim_def d3dc_shims[] = {
    TM_SHIM("d3dcompiler_46.dll", D3DReflect),
    TM_SHIM("d3dcompiler_46.dll", D3DGetInputSignatureBlob),
};
void tm_d3dcompiler_register(void) {
    c_refl = (tm_com_class){ "ID3D11ShaderReflection", (int)(sizeof refl_methods / sizeof refl_methods[0]), refl_methods, NULL, 0x52454600, 0 };
    c_cbuf = (tm_com_class){ "ID3D11ShaderReflectionConstantBuffer", 3, cbuf_methods, NULL, 0x52454601, 0 };
    c_var = (tm_com_class){ "ID3D11ShaderReflectionVariable", 4, var_methods, NULL, 0x52454602, 0 };
    c_type = (tm_com_class){ "ID3D11ShaderReflectionType", (int)(sizeof type_methods / sizeof type_methods[0]), type_methods, NULL, 0x52454603, 0 };
    c_blob = (tm_com_class){ "ID3DBlob", 5, blob_methods, NULL, 0x424C4F42, 0 };
    tm_register_shims(d3dc_shims, sizeof d3dc_shims / sizeof d3dc_shims[0]);
}
