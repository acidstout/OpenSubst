/*
 * theme.h - Light and Dark appearance for the Win32 UI.
 *
 * Dark mode uses the private uxtheme.dll ordinals (Windows 10 1809+) and
 * paints the parts no theme covers (push buttons, the menu bar).
 *
 * Copyright (c) 2026 Rekow IT. MIT License, see LICENSE.
 */
#ifndef OPENSUBST_THEME_H
#define OPENSUBST_THEME_H

#include "common.h"

BOOL Theme_Init(void);
BOOL Theme_Supported(void);
BOOL Theme_SystemPrefersDark(void);

/* THEME_LIGHT or THEME_DARK (resolve THEME_AUTO first). */
void Theme_SetMode(int mode);
int  Theme_Mode(void);
BOOL Theme_IsDark(void);

void Theme_ApplyToMainWindow(HWND hwnd);
void Theme_ApplyToButton(HWND hwnd);
void Theme_ApplyToEdit(HWND hwnd);
void Theme_ApplyToCombo(HWND hwnd);
void Theme_ApplyToScrollable(HWND hwnd);
void Theme_ApplyToTooltip(HWND hwnd);

/* Menu bar painting in dark mode; returns TRUE if the message was handled. */
BOOL Theme_MenuBarMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *res);

COLORREF Theme_Bg(void);        /* window background         */
COLORREF Theme_Surface(void);   /* list / edit background    */
COLORREF Theme_Text(void);
COLORREF Theme_DimText(void);
COLORREF Theme_Line(void);
COLORREF Theme_Hot(void);       /* hovered row               */
COLORREF Theme_Selected(void);  /* selected row              */
COLORREF Theme_Accent(void);
COLORREF Theme_Error(void);
HBRUSH   Theme_BgBrush(void);
HBRUSH   Theme_SurfaceBrush(void);

BOOL Theme_IsColorSchemeChange(UINT msg, LPARAM lp);

#endif
