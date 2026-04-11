/*
 * Dino Crisis 2 — BG/Mask Pair Logger v3
 * Logs (background_hash -> overlay_hash) pairs to dc2_pairs.tsv
 * INSERT = flush now, auto-flush every 60s
 *
 * BUILD (MSVC x86):
 *   cl /LD /O2 dc2_diag.c /link /OUT:dc2_diag.asi kernel32.lib user32.lib
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

static FILE *g_log = NULL;
static void LOG(const char *fmt, ...) {
    if (!g_log) g_log = fopen("dc2_diag.log", "a");
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fflush(g_log);
}

/* ═══ D3D9 ═══ */
typedef struct IDirect3D9 IDirect3D9;
typedef struct IDirect3DDevice9 IDirect3DDevice9;
typedef struct IDirect3DTexture9 IDirect3DTexture9;
typedef struct { INT Pitch; void *pBits; } D3DLOCKED_RECT;
typedef struct { DWORD Format,Type,Usage,Pool,MSType,MSQual; UINT Width,Height; } D3DSURFACE_DESC;

#define D3DFMT_A8R8G8B8 21
#define D3DFMT_X8R8G8B8 22
#define VT_Release 2
#define VT_CreateDevice 16
#define VT_SetTexture 65
#define VT_EndScene 42
#define VT_DrawPrimitiveUP 83
#define VT_TEX_GetDesc 17
#define VT_TEX_LockRect 19
#define VT_TEX_UnlockRect 20

typedef IDirect3D9*(WINAPI *Direct3DCreate9_t)(UINT);
typedef HRESULT(WINAPI *EndScene_t)(IDirect3DDevice9*);
typedef HRESULT(WINAPI *SetTexture_t)(IDirect3DDevice9*,DWORD,IDirect3DTexture9*);
typedef HRESULT(WINAPI *DrawPrimUP_t)(IDirect3DDevice9*,DWORD,UINT,const void*,UINT);
typedef HRESULT(WINAPI *TexGetDesc_t)(IDirect3DTexture9*,UINT,D3DSURFACE_DESC*);
typedef HRESULT(WINAPI *TexLockRect_t)(IDirect3DTexture9*,UINT,D3DLOCKED_RECT*,const RECT*,DWORD);
typedef HRESULT(WINAPI *TexUnlockRect_t)(IDirect3DTexture9*,UINT);

/* ═══ DETOUR ═══ */
typedef struct { uint8_t trampoline[32]; void *target; int steal; } detour_t;

static int insn_len(const uint8_t *p) {
    if (p[0]==0x55||p[0]==0x53||p[0]==0x56||p[0]==0x57||p[0]==0x50||p[0]==0x51||p[0]==0x52) return 1;
    if (p[0]==0x8B&&p[1]==0xEC) return 2; if (p[0]==0x8B&&p[1]==0xFF) return 2;
    if (p[0]==0x89&&(p[1]&0xC0)==0x40) return 3;
    if (p[0]==0x83&&p[1]==0xEC) return 3; if (p[0]==0x81&&p[1]==0xEC) return 6;
    if (p[0]==0x6A) return 2;
    if (p[0]==0x68||p[0]==0xB8||p[0]==0xE9) return 5;
    if (p[0]==0x33&&(p[1]&0xC0)==0xC0) return 2;
    if (p[0]==0x90||p[0]==0xCC||p[0]==0xC3) return 1; if (p[0]==0xC2) return 3;
    if (p[0]==0x64&&p[1]==0xA1) return 6; if (p[0]==0xFF&&p[1]==0x25) return 6;
    if (p[0]==0x8B&&(p[1]&0xC7)==0x44) return 4; if (p[0]==0x8B&&(p[1]&0xC7)==0x01) return 2;
    return 0;
}
static BOOL detour_install(detour_t *d,void *target,void *hook) {
    memset(d,0,sizeof(*d)); d->target=target;
    const uint8_t *p=(const uint8_t*)target; int total=0;
    while(total<5){int l=insn_len(p+total);if(!l)return FALSE;total+=l;}
    d->steal=total; DWORD old;
    VirtualProtect(d->trampoline,32,PAGE_EXECUTE_READWRITE,&old);
    memcpy(d->trampoline,target,total);
    d->trampoline[total]=0xE9;
    *(uint32_t*)(d->trampoline+total+1)=(uint32_t)((uint8_t*)target+total)-(uint32_t)(d->trampoline+total+5);
    VirtualProtect(target,total,PAGE_EXECUTE_READWRITE,&old);
    *(uint8_t*)target=0xE9;
    *(uint32_t*)((uint8_t*)target+1)=(uint32_t)hook-((uint32_t)target+5);
    for(int i=5;i<total;i++)((uint8_t*)target)[i]=0x90;
    VirtualProtect(target,total,old,&old);
    FlushInstructionCache(GetCurrentProcess(),target,total);
    return TRUE;
}

/* ═══ HASH ═══ */
static uint32_t calc_full_hash(void *bits,int pitch,UINT w,UINT h,int bpp) {
    uint32_t hv=0x811C9DC5;
    for(UINT y=0;y<h;y++){uint8_t *row=(uint8_t*)bits+y*pitch;
        for(UINT x=0;x<w*bpp;x++){hv^=row[x];hv*=0x01000193;}}
    return hv;
}

/* Quick hash: first + middle + last row only (fast content change detection) */
static uint32_t calc_quick_hash(void *bits,int pitch,UINT w,UINT h,int bpp) {
    uint32_t hv=0x811C9DC5;
    UINT rows[3]={0, h/2, h>0?h-1:0};
    for(int r=0;r<3;r++){
        uint8_t *row=(uint8_t*)bits+rows[r]*pitch;
        for(UINT x=0;x<w*bpp;x++){hv^=row[x];hv*=0x01000193;}
    }
    return hv;
}

/* ═══ TEXTURE CACHE with quick_hash invalidation ═══ */
#define MAX_TC 4096
typedef struct {
    IDirect3DTexture9 *ptr;
    uint32_t full_hash;
    uint32_t quick_hash;
    UINT w, h;
    DWORD fmt;
} tc_entry_t;
static tc_entry_t g_tc[MAX_TC];
static int g_ntc = 0;

static uint32_t get_tex_hash(IDirect3DTexture9 *tex, UINT *ow, UINT *oh) {
    if (!tex) return 0;

    void **vt = *(void***)tex;
    D3DSURFACE_DESC desc;
    if (((TexGetDesc_t)vt[VT_TEX_GetDesc])(tex, 0, &desc) != 0) return 0;
    int bpp = (desc.Format==D3DFMT_A8R8G8B8||desc.Format==D3DFMT_X8R8G8B8) ? 4 : 2;

    D3DLOCKED_RECT lr;
    if (((TexLockRect_t)vt[VT_TEX_LockRect])(tex, 0, &lr, NULL, 0x10) != 0) return 0;

    uint32_t qh = calc_quick_hash(lr.pBits, lr.Pitch, desc.Width, desc.Height, bpp);

    /* Find in cache */
    tc_entry_t *found = NULL;
    for (int i = 0; i < g_ntc; i++) {
        if (g_tc[i].ptr == tex) { found = &g_tc[i]; break; }
    }

    if (found && found->quick_hash == qh) {
        /* Content unchanged — return cached full hash */
        ((TexUnlockRect_t)vt[VT_TEX_UnlockRect])(tex, 0);
        *ow = found->w; *oh = found->h;
        return found->full_hash;
    }

    /* Content changed or new texture — compute full hash */
    uint32_t fh = calc_full_hash(lr.pBits, lr.Pitch, desc.Width, desc.Height, bpp);
    ((TexUnlockRect_t)vt[VT_TEX_UnlockRect])(tex, 0);

    if (found) {
        found->quick_hash = qh;
        found->full_hash = fh;
        found->w = desc.Width; found->h = desc.Height;
        found->fmt = desc.Format;
    } else if (g_ntc < MAX_TC) {
        tc_entry_t *e = &g_tc[g_ntc++];
        e->ptr = tex; e->quick_hash = qh; e->full_hash = fh;
        e->w = desc.Width; e->h = desc.Height; e->fmt = desc.Format;
    }

    *ow = desc.Width; *oh = desc.Height;
    return fh;
}

/* ═══ PAIRS ═══ */
#define MAX_PAIRS 65536
typedef struct {
    uint32_t bg_hash, overlay_hash;
    UINT ow, oh;
    int draw_order;
    float dx0,dy0,dx1,dy1, u0,v0,u1,v1;
} pair_t;
static pair_t g_pairs[MAX_PAIRS];
static int g_npairs = 0;

/* Dedup by (bg, overlay, uv_rect) — each unique tile is recorded */
static BOOL tile_exists(uint32_t bg, uint32_t ov, float u0, float v0, float u1, float v1) {
    for (int i = 0; i < g_npairs; i++) {
        pair_t *p = &g_pairs[i];
        if (p->bg_hash==bg && p->overlay_hash==ov &&
            p->u0==u0 && p->v0==v0 && p->u1==u1 && p->v1==v1)
            return TRUE;
    }
    return FALSE;
}
static void flush_pairs(void) {
    if (!g_npairs) return;
    FILE *f = fopen("dc2_pairs.tsv","w"); if (!f) return;
    fprintf(f, "bg_hash\toverlay_hash\toverlay_w\toverlay_h\tdraw_order\t"
               "dst_x0\tdst_y0\tdst_x1\tdst_y1\tuv_u0\tuv_v0\tuv_u1\tuv_v1\n");
    for (int i = 0; i < g_npairs; i++) {
        pair_t *p = &g_pairs[i];
        fprintf(f, "%08X\t%08X\t%u\t%u\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%.4f\t%.4f\t%.4f\t%.4f\n",
            p->bg_hash, p->overlay_hash, p->ow, p->oh, p->draw_order,
            p->dx0,p->dy0,p->dx1,p->dy1, p->u0,p->v0,p->u1,p->v1);
    }
    fclose(f);
    LOG("Flushed %d pairs\n", g_npairs);
}

/* ═══ STATE ═══ */
static IDirect3DDevice9 *g_dev = NULL;
static IDirect3DTexture9 *g_cur_tex = NULL;
static int g_frames = 0;
static uint32_t g_cur_bg = 0;
static int g_draw_idx = 0;

/* ═══ HOOKS ═══ */
static detour_t g_det_es, g_det_st, g_det_dpup;

static HRESULT WINAPI hk_SetTexture(IDirect3DDevice9 *dev, DWORD stage, IDirect3DTexture9 *tex) {
    if (stage == 0) g_cur_tex = tex;
    return ((SetTexture_t)g_det_st.trampoline)(dev, stage, tex);
}

static HRESULT WINAPI hk_DrawPrimitiveUP(IDirect3DDevice9 *dev, DWORD type,
    UINT primCount, const void *data, UINT stride)
{
    if (type==5 && primCount==2 && g_cur_tex && stride>=20 && stride<=64) {
        UINT tw, th;
        uint32_t h = get_tex_hash(g_cur_tex, &tw, &th);
        if (h) {
            int uv_off = (stride==28)?20:(stride==24)?16:(stride==32)?24:(int)stride-8;
            float mnx=1e9f,mny=1e9f,mxx=-1e9f,mxy=-1e9f;
            float mnu=1e9f,mnv=1e9f,mxu=-1e9f,mxv=-1e9f;
            for (int i=0;i<4;i++) {
                const uint8_t *v=(const uint8_t*)data+i*stride;
                float x=*(const float*)(v),y=*(const float*)(v+4);
                if(x<mnx)mnx=x;if(x>mxx)mxx=x;if(y<mny)mny=y;if(y>mxy)mxy=y;
                if(uv_off+8<=(int)stride){
                    float u=*(const float*)(v+uv_off),vv=*(const float*)(v+uv_off+4);
                    if(u<mnu)mnu=u;if(u>mxu)mxu=u;if(vv<mnv)mnv=vv;if(vv>mxv)mxv=vv;
                }
            }
            float dw=mxx-mnx, dh=mxy-mny;

            if(tw==320&&th==240&&dw>900.f&&dh>680.f&&mnu<0.01f&&mnv<0.01f&&mxu>0.99f&&mxv>0.99f){
                g_cur_bg=h; g_draw_idx=0;
            } else if(g_cur_bg && h!=g_cur_bg && tw==512 && th==512 && !tile_exists(g_cur_bg,h,mnu,mnv,mxu,mxv)){
                if(g_npairs<MAX_PAIRS){
                    pair_t *p=&g_pairs[g_npairs++];
                    p->bg_hash=g_cur_bg; p->overlay_hash=h;
                    p->ow=tw; p->oh=th; p->draw_order=g_draw_idx;
                    p->dx0=mnx;p->dy0=mny;p->dx1=mxx;p->dy1=mxy;
                    p->u0=mnu;p->v0=mnv;p->u1=mxu;p->v1=mxv;
                }
            }
            g_draw_idx++;
        }
    }
    return ((DrawPrimUP_t)g_det_dpup.trampoline)(dev,type,primCount,data,stride);
}

static HRESULT WINAPI hk_EndScene(IDirect3DDevice9 *dev) {
    g_frames++;
    if (!g_dev) { g_dev=dev; LOG("Device: %p\n",dev); }
    g_cur_bg=0; g_draw_idx=0;
    if (GetAsyncKeyState(VK_INSERT)&1) { flush_pairs(); LOG("[INS] %d pairs\n",g_npairs); }
    return ((EndScene_t)g_det_es.trampoline)(dev);
}

/* ═══ INSTALL ═══ */
static BOOL install_hooks(void) {
    HMODULE d3d9=GetModuleHandleA("d3d9.dll");
    if(!d3d9)d3d9=LoadLibraryA("d3d9.dll"); if(!d3d9)return FALSE;
    Direct3DCreate9_t pC=(Direct3DCreate9_t)GetProcAddress(d3d9,"Direct3DCreate9");
    IDirect3D9 *d3d=pC(32); if(!d3d)return FALSE;
    WNDCLASSA wc={0};wc.lpfnWndProc=DefWindowProcA;
    wc.hInstance=GetModuleHandleA(NULL);wc.lpszClassName="DC2P";
    RegisterClassA(&wc);
    HWND hw=CreateWindowA("DC2P","",WS_OVERLAPPED,0,0,4,4,NULL,NULL,wc.hInstance,NULL);
    uint8_t pp[64];memset(pp,0,64);
    *(UINT*)(pp)=4;*(UINT*)(pp+4)=4;*(DWORD*)(pp+8)=22;
    *(DWORD*)(pp+12)=1;*(DWORD*)(pp+24)=1;*(HWND*)(pp+28)=hw;*(BOOL*)(pp+32)=TRUE;
    void **d3d_vt=*(void***)d3d;
    typedef HRESULT(WINAPI*CD_t)(IDirect3D9*,UINT,DWORD,HWND,DWORD,void*,IDirect3DDevice9**);
    IDirect3DDevice9 *tmp=NULL;
    ((CD_t)d3d_vt[VT_CreateDevice])(d3d,0,1,hw,0x20,pp,&tmp);
    if(!tmp)return FALSE;
    void **vt=*(void***)tmp;
    void *fn_ES=vt[VT_EndScene],*fn_ST=vt[VT_SetTexture],*fn_DPUP=vt[VT_DrawPrimitiveUP];
    ((ULONG(WINAPI*)(void*))vt[VT_Release])(tmp);
    ((ULONG(WINAPI*)(void*))d3d_vt[VT_Release])(d3d);
    DestroyWindow(hw);UnregisterClassA("DC2P",wc.hInstance);
    if(!detour_install(&g_det_es,fn_ES,hk_EndScene))return FALSE;
    if(!detour_install(&g_det_st,fn_ST,hk_SetTexture))return FALSE;
    if(!detour_install(&g_det_dpup,fn_DPUP,hk_DrawPrimitiveUP))return FALSE;
    LOG("Hooks OK\n"); return TRUE;
}

static DWORD WINAPI init_thread(LPVOID x) {
    LOG("\n===== DC2 Pair Logger v3 =====\n");
    Sleep(5000);
    if(!install_hooks()){LOG("FATAL\n");return 1;}
    for(int i=0;i<600;i++){
        Sleep(5000);
        if(i<20||(i%12==0))
            LOG("[%ds] frames=%d pairs=%d cache=%d\n",(i+1)*5,g_frames,g_npairs,g_ntc);
        if((i+1)%12==0)flush_pairs();
    }
    flush_pairs(); return 0;
}

BOOL APIENTRY DllMain(HMODULE h,DWORD r,LPVOID p) {
    if(r==DLL_PROCESS_ATTACH){DisableThreadLibraryCalls(h);CreateThread(NULL,0,init_thread,NULL,0,NULL);}
    return TRUE;
}
