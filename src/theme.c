/*
 * theme.c - appearance modes. Dark mode code adapted from
 * earlier projects (see notes in theme.h).
 *
 * Copyright (c) 2026 Rekow IT. MIT License, see LICENSE.
 */
#include "theme.h"

/* ---- palettes ---- */
#define D_BG        RGB(0x20, 0x20, 0x20)
#define D_SURFACE   RGB(0x2B, 0x2B, 0x2B)
#define D_FACE      RGB(0x37, 0x37, 0x37)
#define D_HOT       RGB(0x34, 0x34, 0x34)
#define D_HOTBTN    RGB(0x45, 0x45, 0x45)
#define D_PRESSED   RGB(0x1C, 0x1C, 0x1C)
#define D_SEL       RGB(0x3A, 0x44, 0x50)
#define D_LINE      RGB(0x45, 0x45, 0x45)
#define D_TEXT      RGB(0xF0, 0xF0, 0xF0)
#define D_DIM       RGB(0xA0, 0xA0, 0xA0)
#define D_DISABLED  RGB(0x70, 0x70, 0x70)
#define D_ACCENT    RGB(0x60, 0xCD, 0xFF)
#define D_ERROR     RGB(0xFF, 0x99, 0xA4)

#define L_BG        RGB(0xF3, 0xF3, 0xF3)
#define L_SURFACE   RGB(0xFF, 0xFF, 0xFF)
#define L_HOT       RGB(0xF5, 0xF5, 0xF5)
#define L_SEL       RGB(0xE1, 0xEC, 0xF8)
#define L_LINE      RGB(0xE0, 0xE0, 0xE0)
#define L_TEXT      RGB(0x1A, 0x1A, 0x1A)
#define L_DIM       RGB(0x60, 0x60, 0x60)
#define L_ACCENT    RGB(0x00, 0x5F, 0xB8)
#define L_ERROR     RGB(0xC4, 0x2B, 0x1C)

typedef enum {
    APPMODE_DEFAULT = 0, APPMODE_ALLOWDARK = 1, APPMODE_FORCEDARK = 2, APPMODE_FORCELIGHT = 3
} PreferredAppMode;

typedef PreferredAppMode (WINAPI *fnSetPreferredAppMode)(PreferredAppMode);
typedef BOOL (WINAPI *fnAllowDarkModeForApp)(BOOL);
typedef BOOL (WINAPI *fnAllowDarkModeForWindow)(HWND, BOOL);
typedef void (WINAPI *fnRefreshImmersiveColorPolicyState)(void);
typedef void (WINAPI *fnFlushMenuThemes)(void);
typedef HRESULT (WINAPI *fnDwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
typedef LONG (WINAPI *fnRtlGetVersion)(RTL_OSVERSIONINFOW *);

typedef struct { int attr; PVOID data; SIZE_T size; } WINCOMPATTRDATA;
typedef BOOL (WINAPI *fnSetWindowCompositionAttribute)(HWND, WINCOMPATTRDATA *);

static fnSetPreferredAppMode              p_SetPreferredAppMode;
static fnAllowDarkModeForApp              p_AllowDarkModeForApp;
static fnAllowDarkModeForWindow           p_AllowDarkModeForWindow;
static fnRefreshImmersiveColorPolicyState p_RefreshImmersiveColorPolicyState;
static fnFlushMenuThemes                  p_FlushMenuThemes;
static fnDwmSetWindowAttribute            p_DwmSetWindowAttribute;
static fnSetWindowCompositionAttribute    p_SetWindowCompositionAttribute;

static BOOL   g_supported;
static BOOL   g_inited;
static int    g_mode = THEME_LIGHT;
static DWORD  g_build;
static HBRUSH g_brBg, g_brSurface;

static LRESULT CALLBACK button_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

static DWORD os_build(void)
{
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    fnRtlGetVersion get;
    RTL_OSVERSIONINFOW vi;
    if (!nt) return 0;
    get = (fnRtlGetVersion)(void *)GetProcAddress(nt, "RtlGetVersion");
    if (!get) return 0;
    ZeroMemory(&vi, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (get(&vi) != 0 || vi.dwMajorVersion < 10) return 0;
    return vi.dwBuildNumber;
}

BOOL Theme_Init(void)
{
    HMODULE ux, dwm, u32;
    if (g_inited) return g_supported;
    g_inited = TRUE;
    g_build = os_build();

    dwm = LoadLibraryExW(L"dwmapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dwm)
        p_DwmSetWindowAttribute =
            (fnDwmSetWindowAttribute)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
    u32 = GetModuleHandleW(L"user32.dll");
    if (u32)
        p_SetWindowCompositionAttribute = (fnSetWindowCompositionAttribute)(void *)
            GetProcAddress(u32, "SetWindowCompositionAttribute");

    if (g_build < 17763) return FALSE;
    ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return FALSE;
    p_RefreshImmersiveColorPolicyState =
        (fnRefreshImmersiveColorPolicyState)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(104));
    p_AllowDarkModeForWindow =
        (fnAllowDarkModeForWindow)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(133));
    p_FlushMenuThemes = (fnFlushMenuThemes)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (g_build < 18362)
        p_AllowDarkModeForApp = (fnAllowDarkModeForApp)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    else
        p_SetPreferredAppMode = (fnSetPreferredAppMode)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));

    g_supported = p_AllowDarkModeForWindow &&
                  (p_SetPreferredAppMode || p_AllowDarkModeForApp);
    return g_supported;
}

BOOL Theme_Supported(void) { return g_supported; }
int  Theme_Mode(void)      { return g_mode; }
BOOL Theme_IsDark(void)    { return g_mode == THEME_DARK; }

BOOL Theme_SystemPrefersDark(void)
{
    HKEY k;
    DWORD val = 1, cb = sizeof(val), type = REG_DWORD;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return FALSE;
    if (RegQueryValueExW(k, L"AppsUseLightTheme", NULL, &type, (LPBYTE)&val, &cb) != ERROR_SUCCESS)
        val = 1;
    RegCloseKey(k);
    return val == 0;
}

void Theme_SetMode(int mode)
{
    BOOL dark;
    if (mode == THEME_DARK && !g_supported) mode = THEME_LIGHT;
    if (mode != THEME_DARK) mode = THEME_LIGHT;
    g_mode = mode;
    dark = (mode == THEME_DARK);

    if (g_supported) {
        if (p_SetPreferredAppMode)
            p_SetPreferredAppMode(dark ? APPMODE_FORCEDARK : APPMODE_FORCELIGHT);
        else if (p_AllowDarkModeForApp)
            p_AllowDarkModeForApp(dark);
        if (p_RefreshImmersiveColorPolicyState) p_RefreshImmersiveColorPolicyState();
        if (p_FlushMenuThemes) p_FlushMenuThemes();
    }
    if (g_brBg) DeleteObject(g_brBg);
    if (g_brSurface) DeleteObject(g_brSurface);
    g_brBg = CreateSolidBrush(Theme_Bg());
    g_brSurface = CreateSolidBrush(Theme_Surface());
}

COLORREF Theme_Bg(void)
{
    return g_mode == THEME_DARK ? D_BG : L_BG;
}
COLORREF Theme_Surface(void)
{
    return g_mode == THEME_DARK ? D_SURFACE : L_SURFACE;
}
COLORREF Theme_Text(void)
{
    return g_mode == THEME_DARK ? D_TEXT : L_TEXT;
}
COLORREF Theme_DimText(void)
{
    return g_mode == THEME_DARK ? D_DIM : L_DIM;
}
COLORREF Theme_Line(void)
{
    return g_mode == THEME_DARK ? D_LINE : L_LINE;
}
COLORREF Theme_Hot(void)
{
    return g_mode == THEME_DARK ? D_HOT : L_HOT;
}
COLORREF Theme_Selected(void)
{
    return g_mode == THEME_DARK ? D_SEL : L_SEL;
}
COLORREF Theme_Accent(void)
{
    return g_mode == THEME_DARK ? D_ACCENT : L_ACCENT;
}
COLORREF Theme_Error(void)
{
    return g_mode == THEME_DARK ? D_ERROR : L_ERROR;
}

HBRUSH Theme_BgBrush(void)
{
    if (!g_brBg) g_brBg = CreateSolidBrush(Theme_Bg());
    return g_brBg;
}
HBRUSH Theme_SurfaceBrush(void)
{
    if (!g_brSurface) g_brSurface = CreateSolidBrush(Theme_Surface());
    return g_brSurface;
}

BOOL Theme_IsColorSchemeChange(UINT msg, LPARAM lp)
{
    if (msg != WM_SETTINGCHANGE || !lp) return FALSE;
    return lstrcmpiW((const wchar_t *)lp, L"ImmersiveColorSet") == 0;
}

/* ============================================================
 *  Windows
 * ============================================================ */

void Theme_ApplyToMainWindow(HWND hwnd)
{
    BOOL dark = (g_mode == THEME_DARK);

    if (g_supported) {
        BOOL v = dark;
        p_AllowDarkModeForWindow(hwnd, dark);
        if (p_RefreshImmersiveColorPolicyState) p_RefreshImmersiveColorPolicyState();
        if (p_DwmSetWindowAttribute &&
            FAILED(p_DwmSetWindowAttribute(hwnd, 20, &v, sizeof(v))))
            p_DwmSetWindowAttribute(hwnd, 19, &v, sizeof(v));
        if (p_SetWindowCompositionAttribute) {
            WINCOMPATTRDATA d;
            d.attr = 26;    /* WCA_USEDARKMODECOLORS */
            d.data = &v;
            d.size = sizeof(v);
            p_SetWindowCompositionAttribute(hwnd, &d);
        }
    }

    if (IsWindowVisible(hwnd)) {
        BOOL act = (GetActiveWindow() == hwnd);
        SendMessageW(hwnd, WM_NCACTIVATE, (WPARAM)!act, 0);
        SendMessageW(hwnd, WM_NCACTIVATE, (WPARAM)act, 0);
    }
    DrawMenuBar(hwnd);
    InvalidateRect(hwnd, NULL, TRUE);
}

static void allow_dark(HWND hwnd)
{
    if (g_supported) p_AllowDarkModeForWindow(hwnd, g_mode == THEME_DARK);
}

void Theme_ApplyToButton(HWND hwnd)
{
    if (!hwnd) return;
    allow_dark(hwnd);
    SetWindowTheme(hwnd, g_mode == THEME_DARK ? L"DarkMode_Explorer" : NULL, NULL);
    SetWindowSubclass(hwnd, button_proc, 1, FALSE);
    InvalidateRect(hwnd, NULL, TRUE);
}

void Theme_ApplyToEdit(HWND hwnd)
{
    if (!hwnd) return;
    allow_dark(hwnd);
    SetWindowTheme(hwnd, g_mode == THEME_DARK ? L"DarkMode_CFD" : NULL, NULL);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    InvalidateRect(hwnd, NULL, TRUE);
}

void Theme_ApplyToCombo(HWND hwnd)
{
    COMBOBOXINFO ci;
    if (!hwnd) return;
    allow_dark(hwnd);
    SetWindowTheme(hwnd, g_mode == THEME_DARK ? L"DarkMode_CFD" : NULL, NULL);
    ZeroMemory(&ci, sizeof(ci));
    ci.cbSize = sizeof(ci);
    if (GetComboBoxInfo(hwnd, &ci) && ci.hwndList) {
        allow_dark(ci.hwndList);
        SetWindowTheme(ci.hwndList, g_mode == THEME_DARK ? L"DarkMode_Explorer" : NULL, NULL);
    }
    InvalidateRect(hwnd, NULL, TRUE);
}

void Theme_ApplyToScrollable(HWND hwnd)
{
    if (!hwnd) return;
    allow_dark(hwnd);
    SetWindowTheme(hwnd, g_mode == THEME_DARK ? L"DarkMode_Explorer" : NULL, NULL);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    InvalidateRect(hwnd, NULL, TRUE);
}

void Theme_ApplyToTooltip(HWND hwnd)
{
    if (!hwnd) return;
    allow_dark(hwnd);
    SetWindowTheme(hwnd, g_mode == THEME_DARK ? L"DarkMode_Explorer" : NULL, NULL);
}

/* ============================================================
 *  Push buttons (no theme suits them in dark mode)
 * ============================================================ */

static void fill(HDC dc, const RECT *rc, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, rc, b);
    DeleteObject(b);
}

static LRESULT CALLBACK button_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR id, DWORD_PTR ref)
{
    BOOL hot = (BOOL)ref;
    BOOL dark = (g_mode == THEME_DARK);

    switch (msg) {
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, button_proc, id);
        break;
    case WM_ERASEBKGND:
        if (dark) return 1;
        break;
    case WM_ENABLE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_SETTEXT:
    case BM_SETSTATE:
    case BM_SETSTYLE:
        if (dark) {
            LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
            InvalidateRect(hwnd, NULL, FALSE);
            return r;
        }
        break;
    case WM_MOUSEMOVE:
        if (dark && !hot) {
            TRACKMOUSEEVENT tme;
            ZeroMemory(&tme, sizeof(tme));
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            SetWindowSubclass(hwnd, button_proc, id, TRUE);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        if (hot) {
            SetWindowSubclass(hwnd, button_proc, id, FALSE);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    case WM_PAINT:
        if (dark) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc, tr;
            wchar_t txt[128];
            HFONT f = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
            HFONT old = f ? (HFONT)SelectObject(dc, f) : NULL;
            BOOL enabled = IsWindowEnabled(hwnd);
            LRESULT st = SendMessageW(hwnd, BM_GETSTATE, 0, 0);
            BOOL pressed = (st & BST_PUSHED) != 0;
            BOOL def = (GetWindowLongPtrW(hwnd, GWL_STYLE) & 0x0F) == BS_DEFPUSHBUTTON;
            COLORREF face;
            HWND parent = GetParent(hwnd);

            GetClientRect(hwnd, &rc);
            /* The parent's background shows around the rounded corners. */
            fill(dc, &rc, parent ? Theme_Bg() : D_BG);
            if (!enabled)     face = D_BG;
            else if (pressed) face = D_PRESSED;
            else if (hot)     face = D_HOTBTN;
            else              face = D_FACE;
            {
                HBRUSH b = CreateSolidBrush(face);
                HPEN p = CreatePen(PS_SOLID, 1, enabled && (hot || def) ? D_ACCENT : D_LINE);
                HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
                int r = MulDiv(8, GetDeviceCaps(dc, LOGPIXELSX), 96);
                RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, r, r);
                SelectObject(dc, ob);
                SelectObject(dc, op);
                DeleteObject(b);
                DeleteObject(p);
            }
            txt[0] = 0;
            GetWindowTextW(hwnd, txt, ARRAYSIZE(txt));
            tr = rc;
            InflateRect(&tr, -4, 0);
            if (pressed) OffsetRect(&tr, 0, 1);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, enabled ? D_TEXT : D_DISABLED);
            DrawTextW(dc, txt, -1, &tr,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_HIDEPREFIX);
            if (enabled && GetFocus() == hwnd &&
                !(SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
                RECT fr = rc;
                InflateRect(&fr, -3, -3);
                SetTextColor(dc, D_TEXT);
                SetBkColor(dc, face);
                DrawFocusRect(dc, &fr);
            }
            if (old) SelectObject(dc, old);
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

/* ============================================================
 *  Dark menu bar (undocumented UAH messages, as used by Notepad++ etc.)
 * ============================================================ */

#define WM_UAHDRAWMENU      0x0091
#define WM_UAHDRAWMENUITEM  0x0092

typedef union {
    struct { DWORD cx; DWORD cy; } rgsizeBar[2];
    struct { DWORD cx; DWORD cy; } rgsizePopup[4];
} UAHMENUITEMMETRICS;

typedef struct {
    DWORD rgcx[4];
    DWORD fUpdateMaxWidths : 2;
} UAHMENUPOPUPMETRICS;

typedef struct {
    HMENU hmenu;
    HDC   hdc;
    DWORD dwFlags;
} UAHMENU;

typedef struct {
    int                 iPosition;
    UAHMENUITEMMETRICS  umim;
    UAHMENUPOPUPMETRICS umpm;
} UAHMENUITEM;

typedef struct {
    DRAWITEMSTRUCT dis;
    UAHMENU        um;
    UAHMENUITEM    umi;
} UAHDRAWMENUITEM;

static void paint_menu_line(HWND hwnd)
{
    MENUBARINFO mbi;
    RECT rcClient, rcWindow, line;
    HDC dc;
    ZeroMemory(&mbi, sizeof(mbi));
    mbi.cbSize = sizeof(mbi);
    if (!GetMenu(hwnd) || !GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) return;
    GetClientRect(hwnd, &rcClient);
    MapWindowPoints(hwnd, NULL, (POINT *)&rcClient, 2);
    GetWindowRect(hwnd, &rcWindow);
    OffsetRect(&rcClient, -rcWindow.left, -rcWindow.top);
    line = rcClient;
    line.bottom = line.top;
    line.top--;
    dc = GetWindowDC(hwnd);
    if (dc) {
        fill(dc, &line, D_BG);
        ReleaseDC(hwnd, dc);
    }
}

BOOL Theme_MenuBarMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *res)
{
    if (g_mode != THEME_DARK) return FALSE;

    switch (msg) {
    case WM_UAHDRAWMENU: {
        UAHMENU *um = (UAHMENU *)lp;
        MENUBARINFO mbi;
        RECT rcWindow, rc;
        ZeroMemory(&mbi, sizeof(mbi));
        mbi.cbSize = sizeof(mbi);
        GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi);
        GetWindowRect(hwnd, &rcWindow);
        rc = mbi.rcBar;
        OffsetRect(&rc, -rcWindow.left, -rcWindow.top);
        rc.top -= 1;
        fill(um->hdc, &rc, D_BG);
        *res = 0;
        return TRUE;
    }
    case WM_UAHDRAWMENUITEM: {
        UAHDRAWMENUITEM *di = (UAHDRAWMENUITEM *)lp;
        wchar_t text[128];
        MENUITEMINFOW mii;
        COLORREF bg = D_BG, fg = D_TEXT;
        DWORD dt = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
        UINT st = di->dis.itemState;

        text[0] = 0;
        ZeroMemory(&mii, sizeof(mii));
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_STRING;
        mii.dwTypeData = text;
        mii.cch = ARRAYSIZE(text) - 1;
        GetMenuItemInfoW(di->um.hmenu, (UINT)di->umi.iPosition, TRUE, &mii);

        if (st & (ODS_HOTLIGHT | ODS_SELECTED)) bg = D_HOTBTN;
        if (st & (ODS_GRAYED | ODS_DISABLED)) fg = D_DISABLED;
        else if (st & ODS_INACTIVE) fg = D_DIM;
        if (st & ODS_NOACCEL) dt |= DT_HIDEPREFIX;

        fill(di->um.hdc, &di->dis.rcItem, bg);
        SetBkMode(di->um.hdc, TRANSPARENT);
        SetTextColor(di->um.hdc, fg);
        DrawTextW(di->um.hdc, text, -1, &di->dis.rcItem, dt);
        *res = 0;
        return TRUE;
    }
    case WM_NCPAINT:
    case WM_NCACTIVATE:
        *res = DefWindowProcW(hwnd, msg, wp, lp);
        paint_menu_line(hwnd);
        return TRUE;
    }
    return FALSE;
}
