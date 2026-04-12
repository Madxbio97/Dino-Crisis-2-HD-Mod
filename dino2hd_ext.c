/*
 * Dino Crisis 2 — DX9 Texture Dumper + Replacer v7
 * Hash-table caches, negative caching, LRU eviction, no_repl optimization.
 *
 * BUILD (MSVC x86):
 *   cl /LD /O2 dino2hd_ext.c /link /OUT:dino2hd_ext.asi kernel32.lib user32.lib
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

/* ═══ LOG ═══ */
static FILE *g_log = NULL;
static void LOG(const char *fmt, ...) {
    if (!g_log) g_log = fopen("dino2hd_ext.log", "a");
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fflush(g_log);
}

/* ═══ D3D9 TYPES ═══ */
typedef struct IDirect3D9        IDirect3D9;
typedef struct IDirect3DDevice9  IDirect3DDevice9;
typedef struct IDirect3DTexture9 IDirect3DTexture9;
typedef struct { INT Pitch; void *pBits; } D3DLOCKED_RECT;
typedef struct {
    DWORD Format, Type, Usage, Pool, MSType, MSQual;
    UINT Width, Height;
} D3DSURFACE_DESC;

#define D3DFMT_A8R8G8B8 21
#define D3DFMT_X8R8G8B8 22
#define D3DFMT_R5G6B5   23
#define D3DFMT_X1R5G5B5 24
#define D3DFMT_A1R5G5B5 25
#define D3DFMT_A4R4G4B4 26
#define D3DPOOL_MANAGED  1

#define VT_Release         2
#define VT_CreateDevice   16
#define VT_CreateTexture  23
#define VT_SetTexture     65
#define VT_EndScene       42
#define VT_TEX_GetDesc    17
#define VT_TEX_LockRect   19
#define VT_TEX_UnlockRect 20

typedef IDirect3D9*(WINAPI *Direct3DCreate9_t)(UINT);
typedef HRESULT(WINAPI *EndScene_t)(IDirect3DDevice9*);
typedef HRESULT(WINAPI *SetTexture_t)(IDirect3DDevice9*, DWORD, IDirect3DTexture9*);
typedef HRESULT(WINAPI *CreateTexture_t)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, DWORD, DWORD, IDirect3DTexture9**, HANDLE*);
typedef HRESULT(WINAPI *TexGetDesc_t)(IDirect3DTexture9*, UINT, D3DSURFACE_DESC*);
typedef HRESULT(WINAPI *TexLockRect_t)(IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
typedef HRESULT(WINAPI *TexUnlockRect_t)(IDirect3DTexture9*, UINT);

/* ═══ WEBP ═══ */
typedef uint8_t*(*WDecBGRA)(const uint8_t*,size_t,int*,int*);
typedef size_t(*WEncBGRA)(const uint8_t*,int,int,int,uint8_t**);
typedef void(*WFree)(void*);
static WDecBGRA pWDec; static WEncBGRA pWEnc; static WFree pWFree;
static BOOL g_webp = FALSE;

static void load_webp(void) {
    HMODULE h = LoadLibraryA("libwebp.dll");
    if (!h) return;
    pWDec  = (WDecBGRA)GetProcAddress(h,"WebPDecodeBGRA");
    pWEnc  = (WEncBGRA)GetProcAddress(h,"WebPEncodeLosslessBGRA");
    pWFree = (WFree)GetProcAddress(h,"WebPFree");
    g_webp = pWDec && pWEnc && pWFree;
    LOG("WebP: %s\n", g_webp ? "OK" : "FAIL");
}

/* ═══ CONFIG ═══ */
static int g_dump    = 1;
static int g_replace = 1;

/* ═══ DETOUR ═══ */
typedef struct {
    uint8_t trampoline[32];
    void *target;
    int steal;
} detour_t;

static int insn_len(const uint8_t *p) {
    if (p[0]==0x55||p[0]==0x53||p[0]==0x56||p[0]==0x57||p[0]==0x50||p[0]==0x51||p[0]==0x52) return 1;
    if (p[0]==0x8B&&p[1]==0xEC) return 2;
    if (p[0]==0x8B&&p[1]==0xFF) return 2;
    if (p[0]==0x89&&(p[1]&0xC0)==0x40) return 3;
    if (p[0]==0x83&&p[1]==0xEC) return 3;
    if (p[0]==0x81&&p[1]==0xEC) return 6;
    if (p[0]==0x6A) return 2;
    if (p[0]==0x68||p[0]==0xB8||p[0]==0xE9) return 5;
    if (p[0]==0x33&&(p[1]&0xC0)==0xC0) return 2;
    if (p[0]==0x90||p[0]==0xCC||p[0]==0xC3) return 1;
    if (p[0]==0xC2) return 3;
    if (p[0]==0x64&&p[1]==0xA1) return 6;
    if (p[0]==0xFF&&p[1]==0x25) return 6;
    if (p[0]==0x8B&&(p[1]&0xC7)==0x44) return 4;
    if (p[0]==0x8B&&(p[1]&0xC7)==0x01) return 2;
    return 0;
}

static BOOL detour_install(detour_t *d, void *target, void *hook) {
    memset(d,0,sizeof(*d)); d->target = target;
    const uint8_t *p = (const uint8_t*)target;
    int total = 0;
    while (total < 5) { int l=insn_len(p+total); if(!l) return FALSE; total+=l; }
    d->steal = total;
    DWORD old;
    VirtualProtect(d->trampoline, 32, PAGE_EXECUTE_READWRITE, &old);
    memcpy(d->trampoline, target, total);
    d->trampoline[total] = 0xE9;
    *(uint32_t*)(d->trampoline+total+1) = (uint32_t)((uint8_t*)target+total) - (uint32_t)(d->trampoline+total+5);
    VirtualProtect(target, total, PAGE_EXECUTE_READWRITE, &old);
    *(uint8_t*)target = 0xE9;
    *(uint32_t*)((uint8_t*)target+1) = (uint32_t)hook - ((uint32_t)target+5);
    for (int i=5;i<total;i++) ((uint8_t*)target)[i]=0x90;
    VirtualProtect(target, total, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, total);
    return TRUE;
}

/* ═══ GLOBALS (forward decl needed by caches) ═══ */
static int g_frames = 0, g_settex_calls = 0;
static int g_dump_count = 0, g_replace_count = 0;

/* ═══════════════════════════════════════════════════════════════
 * CACHE 1: content hash → replacement texture (LRU eviction)
 * Open-addressing hash table with linear probing.
 * Tracks VRAM usage; evicts least-recently-used when over budget.
 * ═══════════════════════════════════════════════════════════════ */
#define REPL_SIZE 8192  /* power of 2, ~50% load = 4096 entries max */
#define REPL_MASK (REPL_SIZE - 1)
#define VRAM_BUDGET (1200ULL * 1024 * 1024)  /* 1.2 GB max for HD textures */

typedef ULONG(WINAPI *Release_t)(void*);

typedef struct {
    uint32_t hash;       /* 0 = empty */
    UINT orig_w, orig_h;
    IDirect3DTexture9 *tex;
    BOOL has_file;
    int  last_frame;     /* last frame this texture was used */
    UINT tex_bytes;      /* approx memory of this HD texture */
} repl_entry_t;
static repl_entry_t g_repl[REPL_SIZE];
static int g_nrepl = 0;
static uint64_t g_vram_used = 0;

static IDirect3DTexture9 *find_replacement(uint32_t hash, BOOL *checked) {
    *checked = FALSE;
    uint32_t slot = hash & REPL_MASK;
    for (int i = 0; i < 64; i++) {
        repl_entry_t *e = &g_repl[(slot + i) & REPL_MASK];
        if (e->hash == 0) return NULL;
        if (e->hash == hash) {
            *checked = TRUE;
            if (e->has_file && e->tex) {
                e->last_frame = g_frames;
                return e->tex;
            }
            if (e->has_file && !e->tex) {
                /* Was evicted — need reload. Signal "not checked" so caller reloads. */
                *checked = FALSE;
                e->hash = 0;  /* clear slot so store_replacement can reuse it */
                g_nrepl--;
                return NULL;
            }
            return NULL;  /* has_file=FALSE → no file on disk */
        }
    }
    return NULL;
}

static void evict_lru(void) {
    /* Find and release the least-recently-used loaded texture */
    int best_idx = -1;
    int best_frame = 0x7FFFFFFF;
    for (int i = 0; i < REPL_SIZE; i++) {
        repl_entry_t *e = &g_repl[i];
        if (e->hash && e->has_file && e->tex && e->last_frame < best_frame) {
            best_frame = e->last_frame;
            best_idx = i;
        }
    }
    if (best_idx >= 0) {
        repl_entry_t *e = &g_repl[best_idx];
        void **vt = *(void***)e->tex;
        ((Release_t)vt[VT_Release])(e->tex);
        g_vram_used -= e->tex_bytes;
        LOG("Evicted %08X (%ux%u, %u KB, frame %d)\n",
            e->hash, e->orig_w, e->orig_h, e->tex_bytes/1024, e->last_frame);
        e->tex = NULL;  /* keep slot: has_file=TRUE, tex=NULL → "evicted, can reload" */
    }
}

static void store_replacement(uint32_t hash, UINT orig_w, UINT orig_h,
                              IDirect3DTexture9 *tex, BOOL has_file, UINT tex_bytes) {
    uint32_t slot = hash & REPL_MASK;
    for (int i = 0; i < 64; i++) {
        repl_entry_t *e = &g_repl[(slot + i) & REPL_MASK];
        if (e->hash == 0 || e->hash == hash) {
            BOOL is_new = (e->hash == 0);
            e->hash = hash; e->orig_w = orig_w; e->orig_h = orig_h;
            e->tex = tex; e->has_file = has_file;
            e->last_frame = g_frames;
            e->tex_bytes = tex_bytes;
            if (is_new) g_nrepl++;
            if (tex) g_vram_used += tex_bytes;
            return;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
 * CACHE 2: texture pointer → (quick_hash, full_hash, frame)
 * Open-addressing hash table with linear probing.
 * ═══════════════════════════════════════════════════════════════ */
#define PTR_SIZE 16384  /* power of 2 */
#define PTR_MASK (PTR_SIZE - 1)
#define RECHECK_INTERVAL 5  /* check every 5 frames (was 2) */

typedef struct {
    IDirect3DTexture9 *ptr;  /* NULL = empty */
    uint32_t full_hash;
    uint32_t quick_hash;
    int      last_frame;
    UINT     w, h;
    DWORD    fmt;
    BOOL     dumped;
    BOOL     no_repl;        /* TRUE = checked, no replacement file exists */
} ptrcache_t;
static ptrcache_t g_ptrcache[PTR_SIZE];

static ptrcache_t *find_ptr(IDirect3DTexture9 *p) {
    uint32_t slot = ((uint32_t)(uintptr_t)p >> 4) & PTR_MASK;
    for (int i = 0; i < 64; i++) {
        ptrcache_t *pc = &g_ptrcache[(slot + i) & PTR_MASK];
        if (pc->ptr == NULL) return NULL;   /* empty = not found */
        if (pc->ptr == p) return pc;
    }
    return NULL;
}

static ptrcache_t *alloc_ptr(IDirect3DTexture9 *p) {
    uint32_t slot = ((uint32_t)(uintptr_t)p >> 4) & PTR_MASK;
    for (int i = 0; i < 64; i++) {
        ptrcache_t *pc = &g_ptrcache[(slot + i) & PTR_MASK];
        if (pc->ptr == NULL || pc->ptr == p) {
            memset(pc, 0, sizeof(*pc));
            pc->ptr = p;
            return pc;
        }
    }
    /* All 64 probe slots full — evict the first one (extremely rare) */
    ptrcache_t *pc = &g_ptrcache[slot];
    memset(pc, 0, sizeof(*pc));
    pc->ptr = p;
    return pc;
}

/* ═══ HASHING ═══ */

/* Быстрый хеш: первая строка + средняя + последняя (для детекта изменений) */
static uint32_t quick_hash(void *bits, int pitch, UINT w, UINT h, int bpp) {
    uint32_t hv = 0x811C9DC5;
    UINT rows[3] = {0, h/2, h > 0 ? h-1 : 0};
    for (int r = 0; r < 3; r++) {
        uint8_t *row = (uint8_t*)bits + rows[r] * pitch;
        for (UINT x = 0; x < w * bpp; x++) { hv ^= row[x]; hv *= 0x01000193; }
    }
    return hv;
}

/* Полный хеш: все пиксели */
static uint32_t full_hash(void *bits, int pitch, UINT w, UINT h, int bpp) {
    uint32_t hv = 0x811C9DC5;
    for (UINT y = 0; y < h; y++) {
        uint8_t *row = (uint8_t*)bits + y * pitch;
        for (UINT x = 0; x < w * bpp; x++) { hv ^= row[x]; hv *= 0x01000193; }
    }
    return hv;
}

/* ═══ GLOBALS ═══ */
static CRITICAL_SECTION g_cs;
static IDirect3DDevice9 *g_dev = NULL;

/* ═══ PIXEL CONVERSION ═══ */
static void convert_row_to_bgra(uint8_t* dst, uint8_t* src, UINT w, DWORD fmt, BOOL force_opaque) {
    for (UINT x = 0; x < w; x++) {
        uint8_t r, g, b, a;
        switch (fmt) {
        case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: {
            uint32_t px = ((uint32_t*)src)[x];
            b = px & 0xFF; g = (px >> 8) & 0xFF; r = (px >> 16) & 0xFF;
            a = force_opaque ? 255 : ((px >> 24) & 0xFF);
            break;
        }
        case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5: {
            uint16_t px = ((uint16_t*)src)[x];
            b = (px & 0x1F) << 3; g = ((px >> 5) & 0x1F) << 3; r = ((px >> 10) & 0x1F) << 3;
            a = force_opaque ? 255 : ((px & 0x8000) ? 255 : 0);
            break;
        }
        case D3DFMT_R5G6B5: {
            uint16_t px = ((uint16_t*)src)[x];
            b = (px & 0x1F) << 3; g = ((px >> 5) & 0x3F) << 2; r = ((px >> 11) & 0x1F) << 3; a = 255;
            break;
        }
        case D3DFMT_A4R4G4B4: {
            uint16_t px = ((uint16_t*)src)[x];
            b = (px & 0xF) << 4; g = ((px >> 4) & 0xF) << 4; r = ((px >> 8) & 0xF) << 4;
            a = force_opaque ? 255 : (((px >> 12) & 0xF) << 4);
            break;
        }
        default: r = g = b = 128; a = 255; break;
        }
        dst[x * 4] = b; dst[x * 4 + 1] = g; dst[x * 4 + 2] = r; dst[x * 4 + 3] = a;
    }
}

/* ═══ DUMP ═══ */
static void dump_texture(uint32_t hash, void *bits, int pitch, UINT w, UINT h, DWORD fmt) {
    if (!g_webp) return;
    char path[260]; sprintf(path,"dump\\textures\\%08X_%ux%u.webp", hash, w, h);
    /* Skip if already dumped */
    if (GetFileAttributesA(path)!=INVALID_FILE_ATTRIBUTES) return;
    uint8_t *bgra = (uint8_t*)malloc(w*h*4);
    if (!bgra) return;
    BOOL opaque = (w == 320 && h == 240);
    for (UINT y = 0; y < h; y++)
        convert_row_to_bgra(bgra + y * w * 4, (uint8_t*)bits + y * pitch, w, fmt, opaque);
    uint8_t *out=NULL;
    size_t sz=pWEnc(bgra, w, h, w*4, &out);
    if (sz>0 && out) {
        FILE *f=fopen(path,"wb"); if(f){fwrite(out,1,sz,f);fclose(f);g_dump_count++;}
        pWFree(out);
    }
    free(bgra);
}

/* ═══ LOAD REPLACEMENT ═══ */
static IDirect3DTexture9 *load_replacement(uint32_t hash, UINT orig_w, UINT orig_h) {
    if (!g_dev || !g_webp) return NULL;

    /* Уже проверяли этот хеш? */
    BOOL checked;
    IDirect3DTexture9 *cached = find_replacement(hash, &checked);
    if (checked) return cached;  /* returns tex or NULL (no file) */

    char path[260];
    sprintf(path, "hires\\textures\\%08X_%ux%u.webp", hash, orig_w, orig_h);
    FILE *f = fopen(path, "rb");
    if (!f) {
        store_replacement(hash, orig_w, orig_h, NULL, FALSE, 0);
        return NULL;
    }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t *buf = (uint8_t*)malloc(sz);
    if (!buf) { fclose(f); return NULL; }
    fread(buf,1,sz,f); fclose(f);

    int w, h;
    uint8_t *pixels = pWDec(buf, sz, &w, &h);
    free(buf);
    if (!pixels) return NULL;

    UINT tex_bytes = (UINT)w * (UINT)h * 4;

    /* Evict LRU textures if over memory budget */
    while (g_vram_used + tex_bytes > VRAM_BUDGET) {
        evict_lru();
        if (g_vram_used == 0) break;  /* nothing left to evict */
    }

    LOG("Loading hires: %s (%dx%d, %u KB, vram=%u MB)\n",
        path, w, h, tex_bytes/1024, (unsigned)(g_vram_used/(1024*1024)));

    void **dev_vt = *(void***)g_dev;
    IDirect3DTexture9 *newtex = NULL;
    HRESULT hr = ((CreateTexture_t)dev_vt[VT_CreateTexture])(
        g_dev, (UINT)w, (UINT)h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &newtex, NULL);
    if (hr != 0 || !newtex) { pWFree(pixels); return NULL; }

    void **tvt = *(void***)newtex;
    D3DLOCKED_RECT lr;
    if (((TexLockRect_t)tvt[VT_TEX_LockRect])(newtex, 0, &lr, NULL, 0) == 0) {
        for (int y = 0; y < h; y++) {
            uint8_t *src = pixels + y*w*4;
            uint8_t *dst = (uint8_t*)lr.pBits + y*lr.Pitch;
            memcpy(dst, src, w*4);
        }
        ((TexUnlockRect_t)tvt[VT_TEX_UnlockRect])(newtex, 0);
    }
    pWFree(pixels);

    store_replacement(hash, orig_w, orig_h, newtex, TRUE, tex_bytes);
    g_replace_count++;
    return newtex;
}

/* ═══════════════════════════════════════════════════════════════
 * PROCESS TEXTURE: хеширование + дамп + поиск замены
 * Возвращает замену или NULL.
 * ═══════════════════════════════════════════════════════════════ */
static IDirect3DTexture9 *process_texture(IDirect3DTexture9 *tex) {
    if (!tex) return NULL;

    void **vt = *(void***)tex;
    D3DSURFACE_DESC desc;
    if (((TexGetDesc_t)vt[VT_TEX_GetDesc])(tex, 0, &desc) != 0) return NULL;

    int bpp = (desc.Format==D3DFMT_A8R8G8B8||desc.Format==D3DFMT_X8R8G8B8) ? 4 : 2;

    /* Lock для чтения */
    D3DLOCKED_RECT lr;
    if (((TexLockRect_t)vt[VT_TEX_LockRect])(tex, 0, &lr, NULL, 0x10) != 0)
        return NULL;

    /* Быстрый хеш для детекта изменений */
    uint32_t qh = quick_hash(lr.pBits, lr.Pitch, desc.Width, desc.Height, bpp);

    EnterCriticalSection(&g_cs);

    ptrcache_t *pc = find_ptr(tex);
    BOOL content_changed = FALSE;

    if (pc) {
        if (pc->quick_hash != qh) {
            content_changed = TRUE;
            pc->quick_hash = qh;
            pc->full_hash = 0;
            pc->dumped = FALSE;
            pc->no_repl = FALSE;  /* content changed, re-check */
            pc->last_frame = g_frames;
        } else if (g_frames - pc->last_frame < RECHECK_INTERVAL) {
            uint32_t fh = pc->full_hash;
            BOOL skip = pc->no_repl;
            LeaveCriticalSection(&g_cs);
            ((TexUnlockRect_t)vt[VT_TEX_UnlockRect])(tex, 0);
            if (skip) return NULL;  /* known no replacement — skip entirely */
            if (g_replace && fh) {
                BOOL chk;
                return find_replacement(fh, &chk);
            }
            return NULL;
        }
        pc->last_frame = g_frames;
    } else {
        /* Новая текстура (или коллизия слота — вытеснение автоматическое) */
        pc = alloc_ptr(tex);
        pc->quick_hash = qh;
        pc->last_frame = g_frames;
        content_changed = TRUE;
    }

    LeaveCriticalSection(&g_cs);

    /* Полный хеш (нужен для дампа и поиска замены) */
    uint32_t fh = full_hash(lr.pBits, lr.Pitch, desc.Width, desc.Height, bpp);

    /* Дамп */
    if (g_dump && !pc->dumped) {
        dump_texture(fh, lr.pBits, lr.Pitch, desc.Width, desc.Height, desc.Format);
        pc->dumped = TRUE;
    }

    ((TexUnlockRect_t)vt[VT_TEX_UnlockRect])(tex, 0);

    /* Обновляем кэш */
    EnterCriticalSection(&g_cs);
    pc->full_hash = fh;
    pc->w = desc.Width;
    pc->h = desc.Height;
    pc->fmt = desc.Format;
    LeaveCriticalSection(&g_cs);

    /* Ищем/загружаем замену */
    if (g_replace && g_dev) {
        IDirect3DTexture9 *repl = load_replacement(fh, desc.Width, desc.Height);
        if (!repl) {
            /* No replacement file — mark to skip full_hash next time */
            EnterCriticalSection(&g_cs);
            pc->no_repl = TRUE;
            LeaveCriticalSection(&g_cs);
        }
        return repl;
    }
    return NULL;
}

/* ═══ HOOKS ═══ */
static detour_t g_det_endscene, g_det_settex;

static HRESULT WINAPI hk_EndScene(IDirect3DDevice9 *dev) {
    g_frames++;
    if (!g_dev) { g_dev = dev; LOG("Device: %p\n", dev); }
    return ((EndScene_t)g_det_endscene.trampoline)(dev);
}

static HRESULT WINAPI hk_SetTexture(IDirect3DDevice9 *dev, DWORD stage, IDirect3DTexture9 *tex) {
    g_settex_calls++;
    if (tex) {
        IDirect3DTexture9 *repl = process_texture(tex);
        if (repl) tex = repl;
    }
    return ((SetTexture_t)g_det_settex.trampoline)(dev, stage, tex);
}

/* ═══ INSTALL ═══ */
static BOOL install_hooks(void) {
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    if (!d3d9) d3d9 = LoadLibraryA("d3d9.dll");
    if (!d3d9) return FALSE;
    Direct3DCreate9_t pC = (Direct3DCreate9_t)GetProcAddress(d3d9,"Direct3DCreate9");
    IDirect3D9 *d3d = pC(32);
    if (!d3d) return FALSE;

    WNDCLASSA wc={0}; wc.lpfnWndProc=DefWindowProcA;
    wc.hInstance=GetModuleHandleA(NULL); wc.lpszClassName="DC2TMP";
    RegisterClassA(&wc);
    HWND hw=CreateWindowA("DC2TMP","",WS_OVERLAPPED,0,0,4,4,NULL,NULL,wc.hInstance,NULL);

    uint8_t pp[64]; memset(pp,0,64);
    *(UINT*)(pp)=4; *(UINT*)(pp+4)=4; *(DWORD*)(pp+8)=22;
    *(DWORD*)(pp+12)=1; *(DWORD*)(pp+24)=1;
    *(HWND*)(pp+28)=hw; *(BOOL*)(pp+32)=TRUE;

    void **d3d_vt=*(void***)d3d;
    typedef HRESULT(WINAPI*CD_t)(IDirect3D9*,UINT,DWORD,HWND,DWORD,void*,IDirect3DDevice9**);
    IDirect3DDevice9 *tmp=NULL;
    ((CD_t)d3d_vt[VT_CreateDevice])(d3d,0,1,hw,0x20,pp,&tmp);
    if (!tmp) return FALSE;

    void **vt=*(void***)tmp;
    void *fn_ES=vt[VT_EndScene], *fn_ST=vt[VT_SetTexture];
    LOG("EndScene=%p SetTexture=%p\n", fn_ES, fn_ST);

    ((ULONG(WINAPI*)(void*))vt[VT_Release])(tmp);
    ((ULONG(WINAPI*)(void*))d3d_vt[VT_Release])(d3d);
    DestroyWindow(hw); UnregisterClassA("DC2TMP",wc.hInstance);

    if (!detour_install(&g_det_endscene, fn_ES, hk_EndScene)) return FALSE;
    if (!detour_install(&g_det_settex, fn_ST, hk_SetTexture)) return FALSE;
    LOG("Hooks OK\n");
    return TRUE;
}

/* ═══ THREAD ═══ */
static DWORD WINAPI init_thread(LPVOID x) {
    LOG("\n===== DC2 Texture Hook v7 =====\n");
    Sleep(5000);
    InitializeCriticalSection(&g_cs);
    load_webp();
    CreateDirectoryA("dump",NULL); CreateDirectoryA("dump\\textures",NULL);
    CreateDirectoryA("hires",NULL); CreateDirectoryA("hires\\textures",NULL);
    if (!install_hooks()) { LOG("FATAL\n"); return 1; }
    for (int i=0;i<600;i++) {
        Sleep(5000);
        if (i<20||(i%12==0))
            LOG("[%ds] fr=%d st=%d repl=%d/%d dump=%d vram=%uMB\n",
                (i+1)*5,g_frames,g_settex_calls,g_nrepl,g_replace_count,g_dump_count,
                (unsigned)(g_vram_used/(1024*1024)));
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD r, LPVOID p) {
    if (r==DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h); CreateThread(NULL,0,init_thread,NULL,0,NULL);
    }
    return TRUE;
}
