/*
 * gfx.c - minimal GDI+ flat API bindings for C.
 *
 * Copyright (c) 2026 Rekow IT. MIT License, see LICENSE.
 */
#include "gfx.h"

typedef struct {
    UINT32 GdiplusVersion;
    void  *DebugEventCallback;
    BOOL   SuppressBackgroundThread;
    BOOL   SuppressExternalCodecs;
} GpStartupInput;

typedef void GpGraphics;
typedef void GpBrush;
typedef void GpPen;
typedef void GpPath;

int  WINAPI GdiplusStartup(ULONG_PTR *token, const GpStartupInput *in, void *out);
void WINAPI GdiplusShutdown(ULONG_PTR token);
int  WINAPI GdipCreateFromHDC(HDC hdc, GpGraphics **g);
int  WINAPI GdipDeleteGraphics(GpGraphics *g);
int  WINAPI GdipSetSmoothingMode(GpGraphics *g, int mode);
int  WINAPI GdipSetPixelOffsetMode(GpGraphics *g, int mode);
int  WINAPI GdipCreateSolidFill(DWORD argb, GpBrush **b);
int  WINAPI GdipDeleteBrush(GpBrush *b);
int  WINAPI GdipCreatePen1(DWORD argb, float width, int unit, GpPen **p);
int  WINAPI GdipDeletePen(GpPen *p);
int  WINAPI GdipSetPenStartCap(GpPen *p, int cap);
int  WINAPI GdipSetPenEndCap(GpPen *p, int cap);
int  WINAPI GdipCreatePath(int fillMode, GpPath **p);
int  WINAPI GdipDeletePath(GpPath *p);
int  WINAPI GdipAddPathArc(GpPath *p, float x, float y, float w, float h, float start, float sweep);
int  WINAPI GdipClosePathFigure(GpPath *p);
int  WINAPI GdipFillPath(GpGraphics *g, GpBrush *b, GpPath *p);
int  WINAPI GdipDrawPath(GpGraphics *g, GpPen *pen, GpPath *p);
int  WINAPI GdipFillEllipse(GpGraphics *g, GpBrush *b, float x, float y, float w, float h);
int  WINAPI GdipFillRectangle(GpGraphics *g, GpBrush *b, float x, float y, float w, float h);
int  WINAPI GdipDrawLine(GpGraphics *g, GpPen *p, float x1, float y1, float x2, float y2);

#define SmoothingModeAntiAlias  4
#define PixelOffsetModeHalf     4
#define UnitPixel               2
#define LineCapRound            2

struct GfxCtx {
    HDC         dc;
    GpGraphics *g;
};

static ULONG_PTR s_token;
static BOOL      s_ok;

void Gfx_Init(void)
{
    GpStartupInput in;
    ZeroMemory(&in, sizeof(in));
    in.GdiplusVersion = 1;
    s_ok = GdiplusStartup(&s_token, &in, NULL) == 0;
}

void Gfx_Shutdown(void)
{
    if (s_ok) GdiplusShutdown(s_token);
    s_ok = FALSE;
}

static DWORD argb(COLORREF c, BYTE a)
{
    return ((DWORD)a << 24) | ((DWORD)GetRValue(c) << 16) | ((DWORD)GetGValue(c) << 8) |
           (DWORD)GetBValue(c);
}

/* A small stack of contexts (UI thread only), so a paint that triggers
   another paint - e.g. through a modal loop - cannot clobber the outer one. */
#define MAX_CTX 8
static GfxCtx s_ctx[MAX_CTX + 1];   /* the extra slot is a GDI-only fallback */
static int    s_depth;

GfxCtx *Gfx_Begin(HDC dc)
{
    GfxCtx *c = s_depth < MAX_CTX ? &s_ctx[s_depth++] : &s_ctx[MAX_CTX];
    c->dc = dc;
    c->g = NULL;
    if (c != &s_ctx[MAX_CTX] && s_ok && GdipCreateFromHDC(dc, &c->g) == 0 && c->g) {
        GdipSetSmoothingMode(c->g, SmoothingModeAntiAlias);
        GdipSetPixelOffsetMode(c->g, PixelOffsetModeHalf);
    } else {
        c->g = NULL;
    }
    return c;
}

void Gfx_End(GfxCtx *g)
{
    if (!g) return;
    if (g->g) GdipDeleteGraphics(g->g);
    g->g = NULL;
    if (g != &s_ctx[MAX_CTX] && s_depth > 0) s_depth--;
}

static GpPath *round_path(float x, float y, float w, float h, float r)
{
    GpPath *p = NULL;
    float d;
    if (GdipCreatePath(0, &p) != 0) return NULL;
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    d = r * 2;
    if (d <= 0.01f) d = 0.01f;
    GdipAddPathArc(p, x, y, d, d, 180, 90);
    GdipAddPathArc(p, x + w - d, y, d, d, 270, 90);
    GdipAddPathArc(p, x + w - d, y + h - d, d, d, 0, 90);
    GdipAddPathArc(p, x, y + h - d, d, d, 90, 90);
    GdipClosePathFigure(p);
    return p;
}

/* GDI fallbacks: no alpha, no anti-aliasing. */
static void gdi_round(HDC dc, float x, float y, float w, float h, float r, COLORREF fillc,
                      BOOL doFill, COLORREF penc, int penw)
{
    HBRUSH b = doFill ? CreateSolidBrush(fillc) : (HBRUSH)GetStockObject(NULL_BRUSH);
    HPEN p = penw ? CreatePen(PS_SOLID, penw, penc) : (HPEN)GetStockObject(NULL_PEN);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, (int)x, (int)y, (int)(x + w + 0.5f), (int)(y + h + 0.5f), (int)(r * 2), (int)(r * 2));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    if (doFill) DeleteObject(b);
    if (penw) DeleteObject(p);
}

void Gfx_FillRoundRect(GfxCtx *g, float x, float y, float w, float h, float r,
                       COLORREF c, BYTE alpha)
{
    if (g->g) {
        GpPath *p = round_path(x, y, w, h, r);
        GpBrush *b = NULL;
        if (p && GdipCreateSolidFill(argb(c, alpha), &b) == 0) {
            GdipFillPath(g->g, b, p);
            GdipDeleteBrush(b);
        }
        if (p) GdipDeletePath(p);
    } else {
        gdi_round(g->dc, x, y, w, h, r, c, TRUE, c, 0);
    }
}

void Gfx_DrawRoundRect(GfxCtx *g, float x, float y, float w, float h, float r,
                       float width, COLORREF c, BYTE alpha)
{
    if (g->g) {
        float hw = width / 2;
        GpPath *p = round_path(x + hw, y + hw, w - width, h - width, r - hw);
        GpPen *pen = NULL;
        if (p && GdipCreatePen1(argb(c, alpha), width, UnitPixel, &pen) == 0) {
            GdipDrawPath(g->g, pen, p);
            GdipDeletePen(pen);
        }
        if (p) GdipDeletePath(p);
    } else {
        gdi_round(g->dc, x, y, w, h, r, c, FALSE, c, (int)(width + 0.5f));
    }
}

void Gfx_FillEllipse(GfxCtx *g, float x, float y, float w, float h, COLORREF c, BYTE alpha)
{
    if (g->g) {
        GpBrush *b = NULL;
        if (GdipCreateSolidFill(argb(c, alpha), &b) == 0) {
            GdipFillEllipse(g->g, b, x, y, w, h);
            GdipDeleteBrush(b);
        }
    } else {
        HBRUSH b = CreateSolidBrush(c);
        HGDIOBJ ob = SelectObject(g->dc, b), op = SelectObject(g->dc, GetStockObject(NULL_PEN));
        Ellipse(g->dc, (int)x, (int)y, (int)(x + w + 0.5f), (int)(y + h + 0.5f));
        SelectObject(g->dc, ob);
        SelectObject(g->dc, op);
        DeleteObject(b);
    }
}

void Gfx_Line(GfxCtx *g, float x1, float y1, float x2, float y2, float width,
              COLORREF c, BYTE alpha)
{
    if (g->g) {
        GpPen *pen = NULL;
        if (GdipCreatePen1(argb(c, alpha), width, UnitPixel, &pen) == 0) {
            GdipSetPenStartCap(pen, LineCapRound);
            GdipSetPenEndCap(pen, LineCapRound);
            GdipDrawLine(g->g, pen, x1, y1, x2, y2);
            GdipDeletePen(pen);
        }
    } else {
        HPEN p = CreatePen(PS_SOLID, (int)(width + 0.5f), c);
        HGDIOBJ op = SelectObject(g->dc, p);
        MoveToEx(g->dc, (int)x1, (int)y1, NULL);
        LineTo(g->dc, (int)x2, (int)y2);
        SelectObject(g->dc, op);
        DeleteObject(p);
    }
}

void Gfx_FillRect(GfxCtx *g, float x, float y, float w, float h, COLORREF c, BYTE alpha)
{
    if (g->g) {
        GpBrush *b = NULL;
        if (GdipCreateSolidFill(argb(c, alpha), &b) == 0) {
            GdipFillRectangle(g->g, b, x, y, w, h);
            GdipDeleteBrush(b);
        }
    } else if (alpha > 127) {
        RECT rc;
        HBRUSH b = CreateSolidBrush(c);
        SetRect(&rc, (int)x, (int)y, (int)(x + w), (int)(y + h));
        FillRect(g->dc, &rc, b);
        DeleteObject(b);
    }
}
