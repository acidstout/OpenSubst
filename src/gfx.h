/*
 * gfx.h - a few anti-aliased shapes through the GDI+ flat API
 * (toggle switches, check boxes, round buttons). Falls back to plain GDI
 * if GDI+ cannot be started.
 *
 * Copyright (c) 2026 Rekow IT. MIT License, see LICENSE.
 */
#ifndef OPENSUBST_GFX_H
#define OPENSUBST_GFX_H

#include "common.h"

void Gfx_Init(void);
void Gfx_Shutdown(void);

typedef struct GfxCtx GfxCtx;
GfxCtx *Gfx_Begin(HDC dc);
void    Gfx_End(GfxCtx *g);

/* Colours are COLORREF plus an alpha 0..255. */
void Gfx_FillRoundRect(GfxCtx *g, float x, float y, float w, float h, float r,
                       COLORREF c, BYTE alpha);
void Gfx_DrawRoundRect(GfxCtx *g, float x, float y, float w, float h, float r,
                       float width, COLORREF c, BYTE alpha);
void Gfx_FillEllipse(GfxCtx *g, float x, float y, float w, float h, COLORREF c, BYTE alpha);
void Gfx_Line(GfxCtx *g, float x1, float y1, float x2, float y2, float width,
              COLORREF c, BYTE alpha);
void Gfx_FillRect(GfxCtx *g, float x, float y, float w, float h, COLORREF c, BYTE alpha);

#endif
