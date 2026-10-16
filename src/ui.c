/*
 * ui.c - main window.
 *
 *  +------------------------------------------------+
 *  | VIRTUAL DRIVES                                 |
 *  | [icon] Projects (P:)              (on)    x    |
 *  |        D:\Projects                             |
 *  | NETWORK DRIVES                                 |
 *  | ...                                            |
 *  +------------------------------------------------+
 *  Path:   [..............................] [...]
 *  Icon:   [..............................] [...]
 *  Drive:  [P: v] [Label...........] [U][R][A]
 *          [ Mount drive ]              Options >>
 *  ------------------------------------------------
 *  [x] Create virtual drives at Windows startup
 *  [x] Run in background and show tray icon
 *  [x] Manage network drives with OpenSubst
 *  [x] Show only OpenSubst drives
 *
 * Copyright (c) 2026 Rekow IT. MIT License, see LICENSE.
 */
#include "ui.h"
#include "lang.h"
#include "settings.h"
#include "drives.h"
#include "engine.h"
#include "uac.h"
#include "recycle.h"
#include "theme.h"
#include "gfx.h"
#include "resource.h"

/* ---- control ids ---- */
enum {
    IDC_LIST = 100, IDC_PATH, IDC_PATHBTN, IDC_ICONEDIT, IDC_ICONBTN, IDC_LETTER, IDC_LABEL,
    IDC_T_UAC, IDC_T_RECYCLE, IDC_T_AUTO,
    IDC_L_PATH, IDC_L_ICON, IDC_L_DRIVE
};
#define IDC_MOUNT IDOK

/* ---- menu ids ---- */
enum {
    IDM_REFRESH = 1000, IDM_ENABLEALL, IDM_DISABLEALL, IDM_REMOVEALL, IDM_REGTASK,
    IDM_UNREGTASK, IDM_EXIT, IDM_LANG_AUTO, IDM_LANG_EN, IDM_LANG_DE,
    IDM_THEME_AUTO, IDM_THEME_LIGHT, IDM_THEME_DARK,
    IDM_ONTOP, IDM_TRAYICON, IDM_GROUP, IDM_CLIHELP, IDM_EXPORTLANG, IDM_ABOUT,
    IDM_O_STARTUP, IDM_O_BACKGROUND, IDM_O_MANAGENET, IDM_O_ONLYOURS,
    IDM_D_OPEN, IDM_D_OPENPATH, IDM_D_ENABLED, IDM_D_UAC, IDM_D_RECYCLE, IDM_D_AUTO,
    IDM_D_MANAGE, IDM_D_REMOVE, IDM_T_SHOWHIDE,
    IDM_LANG_FILE = 1200,       /* + index */
    IDM_T_DRIVE = 1300          /* + letter index */
};

#define WM_APP_TRAY      (WM_APP + 1)
#define WM_APP_AFTERSHOW (WM_APP + 2)
#define WM_APP_ROWCLICK  (WM_APP + 3)

#define TIMER_POLL      1
#define TIMER_RESTORE   2
#define TIMER_SOON      3

enum { ROW_GROUP, ROW_DRIVE, ROW_EMPTY };
enum { PART_NONE, PART_TOGGLE, PART_REMOVE };

typedef struct Row {
    int        type;
    wchar_t    letter;
    BOOL       managed;
    BOOL       network;
    BOOL       pathMissing;
    DriveState st;
    HICON      icon;            /* owned if ownIcon */
    BOOL       ownIcon;
    StrId      groupName;
} Row;

#define MAX_ROWS 64

static HWND   s_wnd, s_list, s_path, s_pathBtn, s_icon, s_iconBtn, s_letter, s_label;
static HWND   s_tog[3], s_mount, s_lbl[3], s_tip;
static HFONT  s_font, s_fontBold, s_fontGroup, s_fontGlyph;
static BOOL   s_haveGlyphFont;
static int    s_dpi = 96;
static HMENU  s_menu;
static HACCEL s_accel;
static HICON  s_icoApp, s_icoAppSmall, s_icoFixed, s_icoNet;
static UINT   s_msgTaskbarCreated, s_msgShowMe, s_msgRefresh;
static BOOL   s_startup, s_noWelcome, s_exitWhenIdle, s_trayAdded, s_tempTray;
static BOOL   s_afterShowPosted;   /* start-up questions asked once, on first show */
static DWORD  s_trayProbe;
static BOOL   s_togState[3];
static DWORD  s_signature;

static Row    s_rows[MAX_ROWS];
static int    s_nrows;
static wchar_t s_selLetter;
static int    s_hotRow = -1, s_hotPart = PART_NONE;

static LangFile s_langs[32];
static int      s_nlangs;

static const DWORD s_togFlag[3] = { DF_UAC, DF_RECYCLE, DF_AUTORESTORE };

static void rebuild_list(void);
static void update_editor_for_letter(void);
static void refresh_editor_state(void);
static void layout(void);

/* ============================================================
 *  Small helpers
 * ============================================================ */

static int S(int v) { return MulDiv(v, s_dpi, 96); }

static int window_dpi(HWND hwnd)
{
    typedef UINT (WINAPI *fnGetDpiForWindow)(HWND);
    static fnGetDpiForWindow p;
    static BOOL looked;
    if (!looked) {
        looked = TRUE;
        p = (fnGetDpiForWindow)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                      "GetDpiForWindow");
    }
    if (p && hwnd) {
        UINT d = p(hwnd);
        if (d) return (int)d;
    }
    {
        HDC dc = GetDC(NULL);
        int d = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(NULL, dc);
        return d ? d : 96;
    }
}

/* Yes/No question. Questions that ask for administrator rights or destroy
   something default to "No": OpenSubst may take the focus when it is shown
   from the tray, and a key press meant for another window must not answer
   them. */
static int ask_yes_no_ex(HWND owner, const wchar_t *title, const wchar_t *content, BOOL warn,
                         BOOL defaultNo)
{
    TASKDIALOGCONFIG tc;
    int btn = IDNO;
    if (g_quiet) return IDYES;
    ZeroMemory(&tc, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = owner;
    tc.hInstance = g_hinst;
    tc.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW;
    tc.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
    tc.pszWindowTitle = T(S_APP_TITLE);
    tc.pszMainIcon = warn ? TD_WARNING_ICON : TD_INFORMATION_ICON;
    tc.pszMainInstruction = title;
    tc.pszContent = content;
    tc.nDefaultButton = defaultNo ? IDNO : IDYES;
    if (FAILED(TaskDialogIndirect(&tc, &btn, NULL, NULL))) {
        wchar_t msg[2048];
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"%s\r\n\r\n%s", title, content ? content : L"");
        btn = MessageBoxW(owner, msg, T(S_APP_TITLE), MB_YESNO | (warn ? MB_ICONWARNING : MB_ICONQUESTION) |
                          (defaultNo ? MB_DEFBUTTON2 : 0));
    }
    return btn;
}

static int ask_yes_no(HWND owner, const wchar_t *title, const wchar_t *content, BOOL warn)
{
    return ask_yes_no_ex(owner, title, content, warn, FALSE);
}

static void info_box(HWND owner, const wchar_t *title, const wchar_t *content)
{
    TASKDIALOGCONFIG tc;
    if (g_quiet) return;
    ZeroMemory(&tc, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = owner;
    tc.hInstance = g_hinst;
    tc.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.dwCommonButtons = TDCBF_OK_BUTTON;
    tc.pszWindowTitle = T(S_APP_TITLE);
    tc.pszMainIcon = TD_INFORMATION_ICON;
    tc.pszMainInstruction = title;
    tc.pszContent = content;
    if (FAILED(TaskDialogIndirect(&tc, NULL, NULL, NULL)))
        MessageBoxW(owner, content, title, MB_OK | MB_ICONINFORMATION);
}

static void error_code_box(HWND owner, StrId head, DWORD code)
{
    wchar_t msg[1024], etext[512], line[640];
    Error_Text(code, etext, ARRAYSIZE(etext));
    StringCchPrintfW(line, ARRAYSIZE(line), T(S_E_ERRCODE), code, etext);
    StringCchPrintfW(msg, ARRAYSIZE(msg), L"%s\r\n\r\n%s", T(head), line);
    App_Message(owner, msg, TRUE);
}

static BOOL any_autorestore(void)
{
    int i;
    for (i = 0; i < 26; i++)
        if (g_cfg[i].used && (g_cfg[i].flags & (DF_ENABLED | DF_AUTORESTORE)) ==
                                 (DF_ENABLED | DF_AUTORESTORE))
            return TRUE;
    return FALSE;
}

static BOOL is_mounted(const DriveConfig *c, const DriveState *st)
{
    if (c->flags & DF_NETWORK)
        return st->kind == DK_NETWORK && lstrcmpiW(st->target, c->path) == 0;
    return st->kind == DK_SUBST && lstrcmpiW(st->target, c->path) == 0;
}

static BOOL uac_available(void)     { return g_splitToken; }
static BOOL recycle_available(void) { return g_splitToken || g_isAdminNow; }

static void get_text(HWND h, wchar_t *out, int cch)
{
    out[0] = 0;
    GetWindowTextW(h, out, cch);
}

/* Open a folder in Explorer. Only folders: a stored path is user-writable
   input, and "opening" a file would run it - with our rights if OpenSubst
   runs as administrator. */
static void open_path(const wchar_t *p)
{
    wchar_t q[PATHBUF + 4];
    if (!p || !p[0] || !Path_IsAbsoluteSpec(p)) return;
    if (!Path_IsDirectoryTimeout(p, DIR_TIMEOUT_USER)) return;
    /* A trailing backslash keeps the shell from resolving it as a file. */
    if (FAILED(StringCchCopyW(q, ARRAYSIZE(q), p))) return;
    PathAddBackslashW(q);
    ShellExecuteW(s_wnd, L"explore", q, NULL, NULL, SW_SHOWNORMAL);
}

/* "file,index" -> icon of the requested size. */
static HICON load_custom_icon(const wchar_t *spec, int size)
{
    wchar_t file[ICONBUF], exp[ICONBUF];
    int index = 0;
    wchar_t *comma;
    HICON ico = NULL;
    StringCchCopyW(file, ARRAYSIZE(file), spec);
    comma = wcsrchr(file, L',');
    if (comma && comma > file + 1) {
        *comma = 0;
        index = _wtoi(comma + 1);
    }
    PathUnquoteSpacesW(file);
    ExpandEnvironmentStringsW(file, exp, ARRAYSIZE(exp));
    if (FAILED(SHDefExtractIconW(exp, index, 0, &ico, NULL, (UINT)MAKELONG(size, size))))
        ico = NULL;
    return ico;
}

static HICON load_stock_icon(SHSTOCKICONID id, int size)
{
    SHSTOCKICONINFO sii;
    HICON ico = NULL;
    ZeroMemory(&sii, sizeof(sii));
    sii.cbSize = sizeof(sii);
    if (SUCCEEDED(SHGetStockIconInfo(id, SHGSI_ICONLOCATION, &sii))) {
        if (FAILED(SHDefExtractIconW(sii.szPath, sii.iIcon, 0, &ico, NULL, (UINT)MAKELONG(size, size))))
            ico = NULL;
    }
    if (!ico && SUCCEEDED(SHGetStockIconInfo(id, SHGSI_ICON | SHGSI_LARGEICON, &sii)))
        ico = sii.hIcon;
    return ico;
}

/* ============================================================
 *  Fonts
 * ============================================================ */

static int CALLBACK font_exists_proc(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM lp)
{
    (void)lf; (void)tm; (void)type;
    *(BOOL *)lp = TRUE;
    return 0;
}

static BOOL font_exists(const wchar_t *face)
{
    LOGFONTW lf;
    BOOL found = FALSE;
    HDC dc = GetDC(NULL);
    ZeroMemory(&lf, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    StringCchCopyW(lf.lfFaceName, ARRAYSIZE(lf.lfFaceName), face);
    EnumFontFamiliesExW(dc, &lf, font_exists_proc, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}

static void create_fonts(void)
{
    NONCLIENTMETRICSW ncm;
    LOGFONTW lf;
    int sysdpi;
    HDC dc;

    if (s_font) DeleteObject(s_font);
    if (s_fontBold) DeleteObject(s_fontBold);
    if (s_fontGroup) DeleteObject(s_fontGroup);
    if (s_fontGlyph) DeleteObject(s_fontGlyph);

    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    dc = GetDC(NULL);
    sysdpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);

    lf = ncm.lfMessageFont;
    lf.lfHeight = MulDiv(lf.lfHeight, s_dpi, sysdpi ? sysdpi : 96);
    if (lf.lfHeight > -S(12) && lf.lfHeight < 0) lf.lfHeight = -S(12);
    lf.lfQuality = CLEARTYPE_QUALITY;
    s_font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    s_fontBold = CreateFontIndirectW(&lf);
    lf.lfHeight = MulDiv(lf.lfHeight, 85, 100);
    lf.lfWeight = FW_SEMIBOLD;
    s_fontGroup = CreateFontIndirectW(&lf);

    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = -S(16);
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    if (font_exists(L"Segoe Fluent Icons")) {
        StringCchCopyW(lf.lfFaceName, ARRAYSIZE(lf.lfFaceName), L"Segoe Fluent Icons");
        s_haveGlyphFont = TRUE;
    } else if (font_exists(L"Segoe MDL2 Assets")) {
        StringCchCopyW(lf.lfFaceName, ARRAYSIZE(lf.lfFaceName), L"Segoe MDL2 Assets");
        s_haveGlyphFont = TRUE;
    } else {
        StringCchCopyW(lf.lfFaceName, ARRAYSIZE(lf.lfFaceName), L"Segoe UI");
        lf.lfHeight = -S(11);
        lf.lfWeight = FW_BOLD;
        s_haveGlyphFont = FALSE;
    }
    s_fontGlyph = CreateFontIndirectW(&lf);
}

static void load_icons(void)
{
    if (s_icoFixed) DestroyIcon(s_icoFixed);
    if (s_icoNet) DestroyIcon(s_icoNet);
    s_icoFixed = load_stock_icon(SIID_DRIVEFIXED, S(32));
    s_icoNet = load_stock_icon(SIID_DRIVENET, S(32));
}

/* ============================================================
 *  Theme application
 * ============================================================ */

static int resolve_theme(void)
{
    if (g_set.theme == THEME_AUTO)
        return (Theme_Supported() && Theme_SystemPrefersDark()) ? THEME_DARK : THEME_LIGHT;
    return (int)g_set.theme;
}

static void apply_theme(void)
{
    int i;
    Theme_SetMode(resolve_theme());
    Theme_ApplyToMainWindow(s_wnd);
    Theme_ApplyToScrollable(s_list);
    Theme_ApplyToEdit(s_path);
    Theme_ApplyToEdit(s_icon);
    Theme_ApplyToEdit(s_label);
    Theme_ApplyToCombo(s_letter);
    Theme_ApplyToButton(s_pathBtn);
    Theme_ApplyToButton(s_iconBtn);
    Theme_ApplyToButton(s_mount);
    Theme_ApplyToTooltip(s_tip);
    for (i = 0; i < 3; i++) InvalidateRect(s_tog[i], NULL, TRUE);
    RedrawWindow(s_wnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

/* ============================================================
 *  Menus
 * ============================================================ */

static void build_menus(void)
{
    HMENU bar = CreateMenu(), cmd = CreatePopupMenu(), adv = CreatePopupMenu();
    HMENU view = CreatePopupMenu(), lang = CreatePopupMenu(), theme = CreatePopupMenu();
    /* "Settings": operation, drives, appearance - separated by lines. */
    HMENU help = CreatePopupMenu();
    HMENU old = s_menu;
    int i;

    AppendMenuW(cmd, MF_STRING, IDM_REFRESH, T(S_M_REFRESH));
    AppendMenuW(cmd, MF_SEPARATOR, 0, NULL);
    AppendMenuW(cmd, MF_STRING, IDM_ENABLEALL, T(S_M_ENABLEALL));
    AppendMenuW(cmd, MF_STRING, IDM_DISABLEALL, T(S_M_DISABLEALL));
    AppendMenuW(cmd, MF_SEPARATOR, 0, NULL);
    AppendMenuW(cmd, MF_STRING, IDM_REMOVEALL, T(S_M_REMOVEALL));
    AppendMenuW(cmd, MF_SEPARATOR, 0, NULL);
    AppendMenuW(adv, MF_STRING, IDM_REGTASK, T(S_M_REGTASK));
    AppendMenuW(adv, MF_STRING, IDM_UNREGTASK, T(S_M_UNREGTASK));
    AppendMenuW(cmd, MF_POPUP, (UINT_PTR)adv, T(S_M_ADVANCED));
    AppendMenuW(cmd, MF_SEPARATOR, 0, NULL);
    AppendMenuW(cmd, MF_STRING, IDM_EXIT, T(S_M_EXIT));

    AppendMenuW(lang, MF_STRING, IDM_LANG_AUTO, T(S_M_LANG_AUTO));
    AppendMenuW(lang, MF_SEPARATOR, 0, NULL);
    AppendMenuW(lang, MF_STRING, IDM_LANG_EN, L"English");
    AppendMenuW(lang, MF_STRING, IDM_LANG_DE, L"Deutsch");
    s_nlangs = Lang_ListFiles(s_langs, ARRAYSIZE(s_langs));
    if (s_nlangs) AppendMenuW(lang, MF_SEPARATOR, 0, NULL);
    for (i = 0; i < s_nlangs; i++)
        AppendMenuW(lang, MF_STRING, (UINT_PTR)(IDM_LANG_FILE + i), s_langs[i].name);
    AppendMenuW(lang, MF_SEPARATOR, 0, NULL);
    AppendMenuW(lang, MF_STRING, IDM_EXPORTLANG, T(S_M_EXPORTLANG));

    AppendMenuW(theme, MF_STRING, IDM_THEME_AUTO, T(S_M_THEME_AUTO));
    AppendMenuW(theme, MF_STRING, IDM_THEME_LIGHT, T(S_M_THEME_LIGHT));
    AppendMenuW(theme, MF_STRING, IDM_THEME_DARK, T(S_M_THEME_DARK));

    AppendMenuW(view, MF_STRING, IDM_O_STARTUP, T(S_M_O_STARTUP));
    AppendMenuW(view, MF_STRING, IDM_O_BACKGROUND, T(S_M_O_BACKGROUND));
    AppendMenuW(view, MF_STRING, IDM_TRAYICON, T(S_M_TRAYICON));
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, IDM_O_MANAGENET, T(S_M_O_MANAGENET));
    AppendMenuW(view, MF_STRING, IDM_O_ONLYOURS, T(S_M_O_ONLYOURS));
    AppendMenuW(view, MF_STRING, IDM_GROUP, T(S_M_GROUP));
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_POPUP, (UINT_PTR)lang, T(S_M_LANGUAGE));
    AppendMenuW(view, MF_POPUP, (UINT_PTR)theme, T(S_M_THEME));
    AppendMenuW(view, MF_STRING, IDM_ONTOP, T(S_M_ONTOP));

    AppendMenuW(help, MF_STRING, IDM_CLIHELP, T(S_M_CLIHELP));
    AppendMenuW(help, MF_SEPARATOR, 0, NULL);
    AppendMenuW(help, MF_STRING, IDM_ABOUT, T(S_M_ABOUT));

    AppendMenuW(bar, MF_POPUP, (UINT_PTR)cmd, T(S_M_COMMAND));
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, T(S_M_SETTINGS));
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, T(S_M_HELP));

    s_menu = bar;
    if (s_wnd) SetMenu(s_wnd, bar);
    if (old) DestroyMenu(old);
}

static void check(HMENU m, UINT id, BOOL on)
{
    CheckMenuItem(m, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
}

static void radio(HMENU m, UINT first, UINT last, UINT sel)
{
    CheckMenuRadioItem(m, first, last, sel, MF_BYCOMMAND);
}

static void init_menu_popup(HMENU m)
{
    BOOL canTask = g_splitToken || g_elevated;
    UINT langSel = IDM_LANG_AUTO;
    int i;

    check(m, IDM_ONTOP, (g_set.flags & AF_ONTOP) != 0);
    /* Background mode always has a tray icon; the menu item then shows that
       and cannot be switched off on its own. */
    check(m, IDM_TRAYICON, (g_set.flags & (AF_TRAYICON | AF_BACKGROUND)) != 0);
    EnableMenuItem(m, IDM_TRAYICON, MF_BYCOMMAND | ((g_set.flags & AF_BACKGROUND) ? MF_GRAYED : MF_ENABLED));
    check(m, IDM_GROUP, (g_set.flags & AF_GROUP) != 0);
    check(m, IDM_O_STARTUP, (g_set.flags & AF_STARTUP) != 0);
    check(m, IDM_O_BACKGROUND, (g_set.flags & AF_BACKGROUND) != 0);
    check(m, IDM_O_MANAGENET, (g_set.flags & AF_MANAGENET) != 0);
    check(m, IDM_O_ONLYOURS, (g_set.flags & AF_ONLYOURS) != 0);
    radio(m, IDM_THEME_AUTO, IDM_THEME_DARK, IDM_THEME_AUTO + g_set.theme);
    EnableMenuItem(m, IDM_THEME_DARK, MF_BYCOMMAND | (Theme_Supported() ? MF_ENABLED : MF_GRAYED));

    if (lstrcmpiW(g_set.language, L"en") == 0) langSel = IDM_LANG_EN;
    else if (lstrcmpiW(g_set.language, L"de") == 0) langSel = IDM_LANG_DE;
    else for (i = 0; i < s_nlangs; i++)
        if (lstrcmpiW(g_set.language, s_langs[i].file) == 0) langSel = (UINT)(IDM_LANG_FILE + i);
    check(m, IDM_LANG_AUTO, langSel == IDM_LANG_AUTO);
    check(m, IDM_LANG_EN, langSel == IDM_LANG_EN);
    check(m, IDM_LANG_DE, langSel == IDM_LANG_DE);
    for (i = 0; i < s_nlangs; i++) check(m, (UINT)(IDM_LANG_FILE + i), langSel == (UINT)(IDM_LANG_FILE + i));

    EnableMenuItem(m, IDM_REGTASK, MF_BYCOMMAND | (canTask ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(m, IDM_UNREGTASK, MF_BYCOMMAND | (canTask ? MF_ENABLED : MF_GRAYED));
}

/* ============================================================
 *  Tray icon
 * ============================================================ */

static int managed_count(void)
{
    int i, n = 0;
    for (i = 0; i < 26; i++) if (g_cfg[i].used) n++;
    return n;
}

static void tray_fill(NOTIFYICONDATAW *nid)
{
    wchar_t count[64];
    int n = managed_count();
    ZeroMemory(nid, sizeof(*nid));
    nid->cbSize = sizeof(*nid);
    nid->hWnd = s_wnd;
    nid->uID = 1;
    nid->uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid->uCallbackMessage = WM_APP_TRAY;
    nid->hIcon = s_icoAppSmall;
    if (n == 1) StringCchCopyW(count, ARRAYSIZE(count), T(S_ST_DRIVES_1));
    else StringCchPrintfW(count, ARRAYSIZE(count), T(S_ST_DRIVES_N), n);
    StringCchPrintfW(nid->szTip, ARRAYSIZE(nid->szTip), L"%s - %s", T(S_APP_TITLE), count);
}

static BOOL tray_wanted(void)
{
    return (g_set.flags & (AF_BACKGROUND | AF_TRAYICON)) || s_tempTray;
}

static void tray_update(void)
{
    NOTIFYICONDATAW nid;
    if (tray_wanted()) {
        tray_fill(&nid);
        if (s_trayAdded && Shell_NotifyIconW(NIM_MODIFY, &nid)) return;
        if (Shell_NotifyIconW(NIM_ADD, &nid)) {
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
            s_trayAdded = TRUE;
        } else {
            s_trayAdded = FALSE;
        }
    } else if (s_trayAdded) {
        tray_fill(&nid);
        Shell_NotifyIconW(NIM_DELETE, &nid);
        s_trayAdded = FALSE;
    }
}

static void show_main(BOOL show)
{
    if (show && !s_afterShowPosted) {
        /* Started hidden in the tray: ask the start-up questions now. */
        s_afterShowPosted = TRUE;
        PostMessageW(s_wnd, WM_APP_AFTERSHOW, 0, 0);
    }
    if (show) {
        if (IsIconic(s_wnd)) ShowWindow(s_wnd, SW_RESTORE);
        else ShowWindow(s_wnd, SW_SHOW);
        SetForegroundWindow(s_wnd);
    } else {
        ShowWindow(s_wnd, SW_HIDE);
    }
}

static void tray_menu(void)
{
    HMENU m = CreatePopupMenu(), drives = NULL;
    POINT pt;
    int i, cmd;

    AppendMenuW(m, MF_STRING, IDM_T_SHOWHIDE, T(S_M_T_SHOWHIDE));
    SetMenuDefaultItem(m, IDM_T_SHOWHIDE, FALSE);
    for (i = 0; i < 26; i++) {
        const DriveConfig *c = &g_cfg[i];
        wchar_t text[LABELBUF + PATHBUF + 16];
        if (!c->used) continue;
        if (!drives) {
            drives = m;
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        }
        StringCchPrintfW(text, ARRAYSIZE(text), L"%c:  %s", c->letter, c->label[0] ? c->label : c->path);
        AppendMenuW(m, MF_STRING | ((c->flags & DF_ENABLED) ? MF_CHECKED : 0),
                    (UINT_PTR)(IDM_T_DRIVE + i), text);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, T(S_M_EXIT));

    GetCursorPos(&pt);
    SetForegroundWindow(s_wnd);
    cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, s_wnd, NULL);
    PostMessageW(s_wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd) PostMessageW(s_wnd, WM_COMMAND, (WPARAM)cmd, 0);
}

/* ============================================================
 *  Drive list model
 * ============================================================ */

static void free_rows(void)
{
    int i;
    for (i = 0; i < s_nrows; i++)
        if (s_rows[i].ownIcon && s_rows[i].icon) DestroyIcon(s_rows[i].icon);
    s_nrows = 0;
}

static void add_drive_row(wchar_t letter, BOOL managed, BOOL net, const DriveState *st)
{
    Row *r;
    const DriveConfig *c = &g_cfg[LETTER_INDEX(letter)];
    if (s_nrows >= MAX_ROWS) return;
    r = &s_rows[s_nrows++];
    ZeroMemory(r, sizeof(*r));
    r->type = ROW_DRIVE;
    r->letter = letter;
    r->managed = managed;
    r->network = net;
    r->st = *st;
    if (managed && !net && !is_mounted(c, st) && (c->flags & DF_ENABLED))
        r->pathMissing = !Path_IsDirectoryTimeout(c->path, DIR_TIMEOUT_UI);
    if (managed && c->icon[0]) {
        r->icon = load_custom_icon(c->icon, S(32));
        r->ownIcon = r->icon != NULL;
    }
    if (!r->icon) r->icon = net ? s_icoNet : s_icoFixed;
}

static void add_group_row(StrId name)
{
    Row *r;
    if (s_nrows >= MAX_ROWS) return;
    r = &s_rows[s_nrows++];
    ZeroMemory(r, sizeof(*r));
    r->type = ROW_GROUP;
    r->groupName = name;
}

/* Hash of the drive state of all letters. 'states' may be NULL; a DriveState
   is 4 KB, so the 26 of them live in a static buffer, not on the stack. */
static DWORD compute_signature(DriveState *states)
{
    static DriveState scratch;
    DWORD h = 2166136261u;
    int i;
    for (i = 0; i < 26; i++) {
        const wchar_t *p;
        DriveState *st = states ? &states[i] : &scratch;
        Drive_Query((wchar_t)(L'A' + i), st);
        h = (h ^ (DWORD)st->kind) * 16777619u;
        for (p = st->target; *p; p++) h = (h ^ (DWORD)*p) * 16777619u;
        h = (h ^ (DWORD)Engine_IsPending((wchar_t)(L'A' + i))) * 16777619u;
    }
    return h;
}

static void fill_letter_combo(void)
{
    DWORD used = Drive_UsedMask();
    wchar_t cur = 0;
    int sel = (int)SendMessageW(s_letter, CB_GETCURSEL, 0, 0), i, pick = -1;
    if (sel >= 0) cur = (wchar_t)SendMessageW(s_letter, CB_GETITEMDATA, (WPARAM)sel, 0);

    SendMessageW(s_letter, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < 26; i++) {
        wchar_t letter = (wchar_t)(L'A' + i);
        BOOL managed = g_cfg[i].used;
        if (letter == L'A' || letter == L'B') {
            if (!managed) continue;     /* floppy letters only if the user chose them */
        }
        if ((used & (1u << i)) && !managed && letter != s_selLetter) continue;
        {
            wchar_t text[8];
            int idx;
            StringCchPrintfW(text, ARRAYSIZE(text), L"%c:", letter);
            idx = (int)SendMessageW(s_letter, CB_ADDSTRING, 0, (LPARAM)text);
            SendMessageW(s_letter, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)letter);
            if (letter == cur) pick = idx;
        }
    }
    if (pick < 0) {
        /* Default: the highest free letter, like most SUBST users pick. */
        int n = (int)SendMessageW(s_letter, CB_GETCOUNT, 0, 0);
        for (i = n - 1; i >= 0; i--) {
            wchar_t l = (wchar_t)SendMessageW(s_letter, CB_GETITEMDATA, (WPARAM)i, 0);
            if (!IS_DRIVE_LETTER(l)) continue;
            if (!g_cfg[LETTER_INDEX(l)].used && !(used & (1u << LETTER_INDEX(l)))) { pick = i; break; }
        }
        if (pick < 0 && n > 0) pick = n - 1;
    }
    SendMessageW(s_letter, CB_SETCURSEL, (WPARAM)pick, 0);
}

static void select_combo_letter(wchar_t letter)
{
    int n = (int)SendMessageW(s_letter, CB_GETCOUNT, 0, 0), i;
    for (i = 0; i < n; i++) {
        if ((wchar_t)SendMessageW(s_letter, CB_GETITEMDATA, (WPARAM)i, 0) == letter) {
            SendMessageW(s_letter, CB_SETCURSEL, (WPARAM)i, 0);
            return;
        }
    }
}

static wchar_t combo_letter(void)
{
    int sel = (int)SendMessageW(s_letter, CB_GETCURSEL, 0, 0);
    LRESULT d;
    if (sel < 0) return 0;
    d = SendMessageW(s_letter, CB_GETITEMDATA, (WPARAM)sel, 0);
    /* CB_ERR or anything unexpected must never become an array index. */
    if (d == CB_ERR || !IS_DRIVE_LETTER((wchar_t)d)) return 0;
    return (wchar_t)d;
}

static void rebuild_list(void)
{
    static DriveState states[26];
    BOOL group = (g_set.flags & AF_GROUP) != 0;
    int pass, i, top, selIdx = -1;
    wchar_t keepSel = s_selLetter;

    s_signature = compute_signature(states);
    top = (int)SendMessageW(s_list, LB_GETTOPINDEX, 0, 0);
    free_rows();

    for (pass = 0; pass < (group ? 2 : 1); pass++) {
        BOOL headerDone = FALSE;
        for (i = 0; i < 26; i++) {
            const DriveConfig *c = &g_cfg[i];
            const DriveState *st = &states[i];
            BOOL managed = c->used, net, show;
            if (managed) {
                net = (c->flags & DF_NETWORK) != 0;
                show = TRUE;
            } else {
                net = st->kind == DK_NETWORK || st->kind == DK_NETUNAVAIL;
                show = !(g_set.flags & AF_ONLYOURS) &&
                       (st->kind == DK_SUBST || (net && (g_set.flags & AF_MANAGENET)));
            }
            if (!show) continue;
            if (group && net != (pass == 1)) continue;
            if (group && !headerDone) {
                add_group_row(pass ? S_GRP_NETWORK : S_GRP_VIRTUAL);
                headerDone = TRUE;
            }
            add_drive_row((wchar_t)(L'A' + i), managed, net, st);
        }
    }
    if (s_nrows == 0) {
        s_rows[0].type = ROW_EMPTY;
        s_rows[0].icon = NULL;
        s_rows[0].ownIcon = FALSE;
        s_nrows = 1;
    }

    SendMessageW(s_list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(s_list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < s_nrows; i++) {
        SendMessageW(s_list, LB_ADDSTRING, 0, (LPARAM)i);
        if (s_rows[i].type == ROW_DRIVE && s_rows[i].letter == keepSel) selIdx = i;
    }
    s_selLetter = selIdx >= 0 ? keepSel : 0;
    SendMessageW(s_list, LB_SETCURSEL, (WPARAM)selIdx, 0);
    SendMessageW(s_list, LB_SETTOPINDEX, (WPARAM)top, 0);
    SendMessageW(s_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(s_list, NULL, TRUE);
    s_hotRow = -1;

    fill_letter_combo();
    if (s_selLetter) select_combo_letter(s_selLetter);
    refresh_editor_state();
    tray_update();
}

/* ============================================================
 *  Drawing
 * ============================================================ */

static void row_parts(const RECT *rc, RECT *toggle, RECT *remove)
{
    int cy = (rc->top + rc->bottom) / 2;
    int rw = S(28), tw = S(40), th = S(20);
    remove->right = rc->right - S(8);
    remove->left = remove->right - rw;
    remove->top = cy - rw / 2;
    remove->bottom = remove->top + rw;
    toggle->right = remove->left - S(10);
    toggle->left = toggle->right - tw;
    toggle->top = cy - th / 2;
    toggle->bottom = toggle->top + th;
}

static void draw_toggle(GfxCtx *g, const RECT *r, BOOL on, BOOL hot, BOOL enabled)
{
    float x = (float)r->left, y = (float)r->top;
    float w = (float)(r->right - r->left), h = (float)(r->bottom - r->top);
    float knob = h - (float)S(on ? 8 : 10) + (hot ? (float)S(2) : 0.0f);
    COLORREF accent = Theme_Accent();
    if (!enabled) accent = Theme_DimText();
    if (on) {
        Gfx_FillRoundRect(g, x, y, w, h, h / 2, accent, 255);
        Gfx_FillEllipse(g, x + w - h / 2 - knob / 2, y + (h - knob) / 2, knob, knob,
                        Theme_IsDark() ? RGB(0, 0, 0) : RGB(255, 255, 255), 255);
    } else {
        Gfx_FillRoundRect(g, x, y, w, h, h / 2, Theme_Surface(), 255);
        Gfx_DrawRoundRect(g, x, y, w, h, h / 2, (float)S(1), Theme_DimText(), 255);
        Gfx_FillEllipse(g, x + h / 2 - knob / 2, y + (h - knob) / 2, knob, knob, Theme_DimText(), 255);
    }
}

static void draw_remove(GfxCtx *g, const RECT *r, BOOL hot, COLORREF rowBg)
{
    float cx = (float)(r->left + r->right) / 2, cy = (float)(r->top + r->bottom) / 2;
    float a = (float)S(5);
    if (hot) {
        COLORREF hb = Theme_IsDark() ? RGB(0x4A, 0x4A, 0x4A) : RGB(0xE3, 0xE3, 0xE3);
        Gfx_FillEllipse(g, (float)r->left, (float)r->top, (float)(r->right - r->left),
                        (float)(r->bottom - r->top), hb, 255);
    }
    (void)rowBg;
    Gfx_Line(g, cx - a, cy - a, cx + a, cy + a, (float)S(1) + 0.5f, Theme_DimText(), 255);
    Gfx_Line(g, cx - a, cy + a, cx + a, cy - a, (float)S(1) + 0.5f, Theme_DimText(), 255);
}

static void row_title(const Row *r, wchar_t *out, size_t cch)
{
    const DriveConfig *c = &g_cfg[LETTER_INDEX(r->letter)];
    const wchar_t *path = r->managed ? c->path : r->st.target;
    wchar_t name[PATHBUF];
    if (r->managed && c->label[0]) {
        StringCchCopyW(name, ARRAYSIZE(name), c->label);
    } else if (path[0]) {
        const wchar_t *f;
        StringCchCopyW(name, ARRAYSIZE(name), path);
        PathRemoveBackslashW(name);
        f = PathFindFileNameW(name);
        if (f && *f && f != name) MoveMemory(name, f, ((size_t)lstrlenW(f) + 1) * sizeof(wchar_t));
    } else {
        name[0] = 0;
    }
    if (name[0]) StringCchPrintfW(out, cch, L"%s (%c:)", name, r->letter);
    else StringCchPrintfW(out, cch, L"%c:", r->letter);
}

static void row_status(const Row *r, wchar_t *out, size_t cch, BOOL *err)
{
    const DriveConfig *c = &g_cfg[LETTER_INDEX(r->letter)];
    *err = FALSE;
    if (!r->managed) {
        StringCchPrintfW(out, cch, L"%s  \x2014  %s", r->st.target, T(S_ST_NOTMANAGED));
        return;
    }
    if (Engine_IsPending(r->letter)) {
        StringCchPrintfW(out, cch, L"%s  \x2014  %s", T(S_ST_CONNECTING), c->path);
        return;
    }
    if (!(c->flags & DF_ENABLED) || is_mounted(c, &r->st)) {
        StringCchCopyW(out, cch, c->path);
        return;
    }
    *err = TRUE;
    if (r->st.kind == DK_NETUNAVAIL && r->network)
        StringCchPrintfW(out, cch, L"%s  \x2014  %s", T(S_ST_DISCONNECTED), c->path);
    else if (r->st.kind != DK_NONE)
        StringCchPrintfW(out, cch, L"%s  \x2014  %s", T(S_ST_INUSE), c->path);
    else if (r->pathMissing)
        StringCchPrintfW(out, cch, L"%s  \x2014  %s", T(S_ST_PATHMISSING), c->path);
    else
        StringCchPrintfW(out, cch, L"%s  \x2014  %s", T(S_ST_NOTMOUNTED), c->path);
}

static void draw_row(const DRAWITEMSTRUCT *di)
{
    const Row *r;
    RECT rc = di->rcItem, tr;
    HDC mem, dc = di->hDC;
    HBITMAP bmp, oldBmp;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int index = (int)di->itemID;
    COLORREF bg;
    GfxCtx *g;
    BOOL sel, hot;

    if (index < 0 || index >= s_nrows || w <= 0 || h <= 0) return;
    r = &s_rows[index];
    sel = (di->itemState & ODS_SELECTED) && r->type == ROW_DRIVE;
    hot = (index == s_hotRow) && r->type == ROW_DRIVE;

    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, w, h);
    oldBmp = (HBITMAP)SelectObject(mem, bmp);
    SetRect(&tr, 0, 0, w, h);
    bg = sel ? Theme_Selected() : (hot ? Theme_Hot() : Theme_Surface());
    {
        HBRUSH b = CreateSolidBrush(Theme_Surface());
        FillRect(mem, &tr, b);
        DeleteObject(b);
    }
    SetBkMode(mem, TRANSPARENT);
    g = Gfx_Begin(mem);

    if (r->type == ROW_GROUP) {
        HFONT of = (HFONT)SelectObject(mem, s_fontGroup);
        RECT t = tr;
        t.left += S(12);
        t.top += S(8);
        SetTextColor(mem, Theme_DimText());
        DrawTextW(mem, T(r->groupName), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(mem, of);
    } else if (r->type == ROW_EMPTY) {
        HFONT of = (HFONT)SelectObject(mem, s_font);
        RECT t = tr;
        InflateRect(&t, -S(16), -S(8));
        SetTextColor(mem, Theme_DimText());
        DrawTextW(mem, T(S_ST_EMPTY), -1, &t, DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(mem, of);
    } else {
        const DriveConfig *c = &g_cfg[LETTER_INDEX(r->letter)];
        RECT tog, rem, local = tr, t1, t2;
        wchar_t title[LABELBUF + 32], status[PATHBUF + 128];
        BOOL err, on = r->managed ? (c->flags & DF_ENABLED) != 0 : TRUE;
        BOOL mounted = r->managed ? is_mounted(c, &r->st) : TRUE;
        int isz = S(32), ix = S(12), iy = (h - isz) / 2;
        int textL = ix + isz + S(12);
        HFONT of;

        Gfx_FillRoundRect(g, (float)S(4), (float)S(2), (float)(w - S(8)), (float)(h - S(4)),
                          (float)S(6), bg, 255);
        if (sel)
            Gfx_FillRoundRect(g, (float)S(4), (float)(h / 2 - S(8)), (float)S(3), (float)S(16),
                              (float)S(2), Theme_Accent(), 255);

        if (r->icon) {
            DrawIconEx(mem, ix, iy, r->icon, isz, isz, 0, NULL, DI_NORMAL);
            if (!mounted) Gfx_FillRect(g, (float)ix, (float)iy, (float)isz, (float)isz, bg, 140);
        }

        row_parts(&local, &tog, &rem);
        draw_toggle(g, &tog, on, hot && s_hotPart == PART_TOGGLE, TRUE);
        draw_remove(g, &rem, hot && s_hotPart == PART_REMOVE, bg);

        row_title(r, title, ARRAYSIZE(title));
        row_status(r, status, ARRAYSIZE(status), &err);

        t1.left = textL;
        t1.right = tog.left - S(8);
        t1.top = h / 2 - S(18);
        t1.bottom = h / 2;
        t2 = t1;
        t2.top = h / 2;
        t2.bottom = h / 2 + S(18);

        of = (HFONT)SelectObject(mem, s_fontBold);
        SetTextColor(mem, mounted ? Theme_Text() : Theme_DimText());
        {
            /* Title, followed by small glyphs for UAC / Recycle Bin / Auto-Restore. */
            RECT calc = t1;
            DrawTextW(mem, title, -1, &t1, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            DrawTextW(mem, title, -1, &calc, DT_LEFT | DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
            if (r->managed && s_haveGlyphFont) {
                static const wchar_t glyphs[3] = { 0xEA18, 0xE74D, 0xE895 };
                int gx = calc.right + S(8), k;
                SelectObject(mem, s_fontGlyph);
                SetTextColor(mem, Theme_DimText());
                for (k = 0; k < 3; k++) {
                    RECT gr;
                    if (!(c->flags & s_togFlag[k])) continue;
                    if (gx + S(16) > t1.right) break;
                    SetRect(&gr, gx, t1.top, gx + S(16), t1.bottom);
                    DrawTextW(mem, &glyphs[k], 1, &gr, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
                    gx += S(18);
                }
            }
        }
        SelectObject(mem, s_font);
        SetTextColor(mem, err ? Theme_Error() : Theme_DimText());
        DrawTextW(mem, status, -1, &t2, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
        SelectObject(mem, of);

        if ((di->itemState & ODS_FOCUS) && !(di->itemState & ODS_NOFOCUSRECT) &&
            !(SendMessageW(s_list, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
            RECT fr = tr;
            InflateRect(&fr, -S(2), -S(1));
            SetTextColor(mem, Theme_Text());
            SetBkColor(mem, bg);
            DrawFocusRect(mem, &fr);
        }
    }

    Gfx_End(g);
    BitBlt(dc, rc.left, rc.top, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

/* Owner-drawn square toggle (UAC / Recycle Bin / Auto-Restore) */
static void draw_flag_button(const DRAWITEMSTRUCT *di, int k)
{
    static const wchar_t glyphs[3] = { 0xEA18, 0xE74D, 0xE895 };
    static const wchar_t *fallback[3] = { L"UAC", L"RB", L"AR" };
    HDC dc = di->hDC;
    RECT rc = di->rcItem;
    BOOL on = s_togState[k];
    BOOL disabled = (di->itemState & ODS_DISABLED) != 0;
    BOOL pressed = (di->itemState & ODS_SELECTED) != 0;
    float x = (float)rc.left, y = (float)rc.top;
    float w = (float)(rc.right - rc.left), h = (float)(rc.bottom - rc.top);
    COLORREF fg;
    GfxCtx *g;

    FillRect(dc, &rc, Theme_BgBrush());
    g = Gfx_Begin(dc);
    if (on && !disabled) {
        Gfx_FillRoundRect(g, x, y, w, h, (float)S(4), Theme_Accent(), pressed ? 90 : 60);
        Gfx_DrawRoundRect(g, x, y, w, h, (float)S(4), (float)S(1), Theme_Accent(), 255);
        fg = Theme_Accent();
    } else {
        Gfx_FillRoundRect(g, x, y, w, h, (float)S(4), pressed ? Theme_Hot() : Theme_Surface(), 255);
        Gfx_DrawRoundRect(g, x, y, w, h, (float)S(4), (float)S(1), Theme_Line(), 255);
        fg = Theme_DimText();
    }
    if (disabled) fg = Theme_Line();
    Gfx_End(g);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    {
        HFONT of = (HFONT)SelectObject(dc, s_haveGlyphFont ? s_fontGlyph : s_fontGroup);
        if (s_haveGlyphFont)
            DrawTextW(dc, &glyphs[k], 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        else
            DrawTextW(dc, fallback[k], -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of);
    }
    if ((di->itemState & ODS_FOCUS) && !(SendMessageW(di->hwndItem, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
        RECT fr = rc;
        InflateRect(&fr, -S(3), -S(3));
        SetTextColor(dc, Theme_Text());
        SetBkColor(dc, Theme_Bg());
        DrawFocusRect(dc, &fr);
    }
}

/* ============================================================
 *  Editor
 * ============================================================ */

static void set_toggles(DWORD flags)
{
    int k;
    for (k = 0; k < 3; k++) {
        s_togState[k] = (flags & s_togFlag[k]) != 0;
        InvalidateRect(s_tog[k], NULL, FALSE);
    }
}

static DWORD toggle_flags(void)
{
    DWORD f = 0;
    int k;
    for (k = 0; k < 3; k++) if (s_togState[k]) f |= s_togFlag[k];
    return f;
}

static void update_mount_caption(void)
{
    wchar_t l = combo_letter();
    BOOL apply = l && g_cfg[LETTER_INDEX(l)].used;
    SetWindowTextW(s_mount, T(apply ? S_BTN_APPLY : S_BTN_MOUNT));
}

/* Load the selected row into the editor fields. */
static void update_editor_for_letter(void)
{
    int sel = (int)SendMessageW(s_list, LB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < s_nrows && s_rows[sel].type == ROW_DRIVE) {
        const Row *r = &s_rows[sel];
        const DriveConfig *c = &g_cfg[LETTER_INDEX(r->letter)];
        s_selLetter = r->letter;
        if (r->managed) {
            SetWindowTextW(s_path, c->path);
            SetWindowTextW(s_label, c->label);
            SetWindowTextW(s_icon, c->icon);
            set_toggles(c->flags);
        } else {
            SetWindowTextW(s_path, r->st.target);
            SetWindowTextW(s_label, L"");
            SetWindowTextW(s_icon, L"");
            set_toggles(g_set.defaultDriveFlags);
        }
        select_combo_letter(r->letter);
    }
    refresh_editor_state();
}

static void refresh_editor_state(void)
{
    EnableWindow(s_tog[0], uac_available());
    EnableWindow(s_tog[1], recycle_available());
    update_mount_caption();
}

static BOOL ensure_task(HWND owner)
{
    DWORD code = 0;
    if (!g_splitToken) return TRUE;
    if (!Uac_LocationSecure()) {
        App_Message(owner, T(S_E_INSECURE), TRUE);
        return FALSE;
    }
    if (Uac_TaskRegistered()) return TRUE;
    if (ask_yes_no_ex(owner, T(S_Q_REGTASK_T), T(S_Q_REGTASK), FALSE, TRUE) != IDYES) return FALSE;
    if (!Uac_RunElevatedSelf(owner, L"/REGTASK", &code)) return FALSE;
    if (code != 0) {
        error_code_box(owner, S_E_REGTASK, code);
        return FALSE;
    }
    return TRUE;
}

static void ask_background_for_autorestore(void)
{
    if (g_set.flags & AF_BACKGROUND) return;
    if (ask_yes_no(s_wnd, T(S_Q_ENABLEBG_T), T(S_Q_ENABLEBG), FALSE) == IDYES) {
        g_set.flags |= AF_BACKGROUND;
        Settings_Save();
        tray_update();
    }
}

static void on_mount(void)
{
    wchar_t raw[PATHBUF], path[PATHBUF], label[LABELBUF], icon[ICONBUF];
    wchar_t letter = combo_letter();
    DriveConfig *c;
    DriveState st;
    DWORD flags = toggle_flags();
    BOOL net;

    get_text(s_path, raw, ARRAYSIZE(raw));
    get_text(s_label, label, ARRAYSIZE(label));
    get_text(s_icon, icon, ARRAYSIZE(icon));
    StrTrimW(label, L" \t");
    StrTrimW(icon, L" \t");

    if (!letter) { App_Message(s_wnd, T(S_E_CHOOSELETTER), TRUE); SetFocus(s_letter); return; }
    StrTrimW(raw, L" \t");
    if (!raw[0]) { App_Message(s_wnd, T(S_E_CHOOSEFOLDER), TRUE); SetFocus(s_path); return; }
    if (!Path_Normalize(raw, path, ARRAYSIZE(path))) {
        wchar_t msg[PATHBUF + 64];
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"%s%s", T(S_E_INVALIDPATH), raw);
        App_Message(s_wnd, msg, TRUE);
        SetFocus(s_path);
        return;
    }
    net = Path_IsUnc(path);
    if (net && !(g_set.flags & AF_MANAGENET)) {
        if (ask_yes_no(s_wnd, T(S_Q_NETPATH), NULL, FALSE) != IDYES) return;
        g_set.flags |= AF_MANAGENET;
        Settings_Save();
    }
    if (!net && !Path_IsDirectoryTimeout(path, DIR_TIMEOUT_USER)) {
        wchar_t msg[PATHBUF + 64];
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"%s%s", T(S_E_INVALIDPATH), path);
        App_Message(s_wnd, msg, TRUE);
        SetFocus(s_path);
        return;
    }
    if (!uac_available()) flags &= ~DF_UAC;
    if (!recycle_available()) flags &= ~DF_RECYCLE;
    if ((flags & DF_UAC) && !ensure_task(s_wnd)) flags &= ~DF_UAC;

    c = &g_cfg[LETTER_INDEX(letter)];
    Drive_Query(letter, &st);

    if (c->used) {
        DriveConfig old = *c;
        BOOL pathChanged = lstrcmpiW(old.path, path) != 0 ||
                           ((old.flags & DF_NETWORK) != 0) != net;
        BOOL labelChanged = lstrcmpW(old.label, label) != 0;
        BOOL iconChanged = lstrcmpiW(old.icon, icon) != 0;
        DWORD newFlags = (old.flags & DF_ENABLED) | DF_ENABLED | flags | (net ? DF_NETWORK : 0);
        BOOL wasMounted = is_mounted(&old, &st);

        if (pathChanged || labelChanged || iconChanged || newFlags != old.flags) {
            wchar_t q[128];
            StringCchPrintfW(q, ARRAYSIZE(q), T(S_Q_APPLY), letter);
            if (ask_yes_no(s_wnd, q, NULL, FALSE) != IDYES) return;
        }
        if (pathChanged && wasMounted) {
            if (Engine_Unmount(letter, s_wnd, TRUE) == ERROR_CANCELLED) return;
            wasMounted = FALSE;
        }
        StringCchCopyW(c->path, ARRAYSIZE(c->path), path);
        StringCchCopyW(c->label, ARRAYSIZE(c->label), label);
        StringCchCopyW(c->icon, ARRAYSIZE(c->icon), icon);
        c->flags = newFlags;
        Config_Save(letter);

        if (!wasMounted) {
            Engine_Mount(letter, s_wnd, TRUE);
        } else {
            if (labelChanged || iconChanged) Engine_RefreshShellInfo(letter, old.label, old.icon);
            if ((newFlags ^ old.flags) & DF_UAC) {
                if (newFlags & DF_UAC) Elev_Add(net ? EOP_NET : EOP_SUBST, letter, path);
                else Elev_Add(net ? EOP_UNNET : EOP_UNSUBST, letter, NULL);
            }
            if ((newFlags ^ old.flags) & DF_RECYCLE) Engine_SetRecycle(letter, (newFlags & DF_RECYCLE) != 0);
        }
    } else {
        DWORD r;
        BOOL adopt = (st.kind == DK_SUBST || st.kind == DK_NETWORK) && lstrcmpiW(st.target, path) == 0;
        if (st.kind != DK_NONE && !adopt) {
            wchar_t msg[256];
            StringCchPrintfW(msg, ARRAYSIZE(msg), T(S_E_LETTERINUSE), letter);
            App_Message(s_wnd, msg, TRUE);
            return;
        }
        c->used = TRUE;
        c->letter = letter;
        c->flags = DF_ENABLED | flags | (net ? DF_NETWORK : 0);
        StringCchCopyW(c->path, ARRAYSIZE(c->path), path);
        StringCchCopyW(c->label, ARRAYSIZE(c->label), label);
        StringCchCopyW(c->icon, ARRAYSIZE(c->icon), icon);
        r = Engine_Mount(letter, s_wnd, TRUE);
        if (r != 0 && r != ERROR_IO_PENDING) {
            c->used = FALSE;
            c->path[0] = c->label[0] = c->icon[0] = 0;
            c->flags = 0;
            Engine_Commit(s_wnd, TRUE);
            return;
        }
        Config_Save(letter);
    }

    g_set.defaultDriveFlags = DF_ENABLED | flags;
    Settings_Save();
    Engine_Commit(s_wnd, TRUE);
    s_selLetter = letter;
    rebuild_list();
}

static void browse_folder(void)
{
    IFileOpenDialog *dlg = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IFileOpenDialog, (void **)&dlg);
    wchar_t cur[PATHBUF];
    get_text(s_path, cur, ARRAYSIZE(cur));
    if (SUCCEEDED(hr)) {
        DWORD opts = 0;
        IShellItem *item = NULL;
        IFileOpenDialog_GetOptions(dlg, &opts);
        IFileOpenDialog_SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        IFileOpenDialog_SetTitle(dlg, T(S_BROWSE_TITLE));
        if (cur[0] && Path_IsDirectoryTimeout(cur, DIR_TIMEOUT_USER) &&
            SUCCEEDED(SHCreateItemFromParsingName(cur, NULL, &IID_IShellItem, (void **)&item))) {
            IFileOpenDialog_SetFolder(dlg, item);
            IShellItem_Release(item);
            item = NULL;
        }
        if (SUCCEEDED(IFileOpenDialog_Show(dlg, s_wnd)) &&
            SUCCEEDED(IFileOpenDialog_GetResult(dlg, &item))) {
            wchar_t *p = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &p)) && p) {
                SetWindowTextW(s_path, p);
                CoTaskMemFree(p);
            }
            IShellItem_Release(item);
        }
        IFileOpenDialog_Release(dlg);
    }
}

static void browse_icon(void)
{
    wchar_t file[MAX_PATH], cur[ICONBUF];
    int index = 0;
    get_text(s_icon, cur, ARRAYSIZE(cur));
    if (cur[0]) {
        wchar_t *comma;
        StringCchCopyW(file, ARRAYSIZE(file), cur);
        comma = wcsrchr(file, L',');
        if (comma) { *comma = 0; index = _wtoi(comma + 1); }
        PathUnquoteSpacesW(file);
    } else {
        StringCchCopyW(file, ARRAYSIZE(file), L"%SystemRoot%\\System32\\imageres.dll");
    }
    if (PickIconDlg(s_wnd, file, ARRAYSIZE(file), &index)) {
        wchar_t spec[ICONBUF];
        StringCchPrintfW(spec, ARRAYSIZE(spec), L"%s,%d", file, index);
        SetWindowTextW(s_icon, spec);
    }
}

static void on_drop(HDROP drop)
{
    wchar_t file[PATHBUF];
    if (DragQueryFileW(drop, 0, file, ARRAYSIZE(file))) {
        const wchar_t *ext = PathFindExtensionW(file);
        if (Path_IsDirectoryTimeout(file, DIR_TIMEOUT_USER)) {
            SetWindowTextW(s_path, file);
        } else if (lstrcmpiW(ext, L".ico") == 0 || lstrcmpiW(ext, L".exe") == 0 ||
                   lstrcmpiW(ext, L".dll") == 0 || lstrcmpiW(ext, L".icl") == 0) {
            wchar_t spec[ICONBUF];
            StringCchPrintfW(spec, ARRAYSIZE(spec), L"%s,0", file);
            SetWindowTextW(s_icon, spec);
        } else {
            PathRemoveFileSpecW(file);
            SetWindowTextW(s_path, file);
        }
    }
    DragFinish(drop);
}

/* ============================================================
 *  Drive actions (list buttons, context menu)
 * ============================================================ */

static const Row *row_for_letter(wchar_t letter)
{
    int i;
    for (i = 0; i < s_nrows; i++)
        if (s_rows[i].type == ROW_DRIVE && s_rows[i].letter == letter) return &s_rows[i];
    return NULL;
}

static void drive_set_enabled(wchar_t letter, BOOL on)
{
    DriveConfig *c;
    if (!IS_DRIVE_LETTER(letter)) return;
    c = &g_cfg[LETTER_INDEX(letter)];
    if (!c->used) return;
    if (on) {
        c->flags |= DF_ENABLED;
        Config_Save(letter);
        Engine_Mount(letter, s_wnd, TRUE);
    } else {
        /* Still mounted (cancelled or failed): keep it marked enabled. */
        if (Engine_Unmount(letter, s_wnd, TRUE) != 0) return;
        c->flags &= ~DF_ENABLED;
        Config_Save(letter);
    }
}

static void drive_toggle_flag(wchar_t letter, DWORD flag)
{
    DriveConfig *c;
    DriveState st;
    BOOL on, net;
    if (!IS_DRIVE_LETTER(letter)) return;
    c = &g_cfg[LETTER_INDEX(letter)];
    if (!c->used) return;
    on = !(c->flags & flag);
    net = (c->flags & DF_NETWORK) != 0;
    if (flag == DF_UAC && on && !ensure_task(s_wnd)) return;
    if (flag == DF_AUTORESTORE && on) ask_background_for_autorestore();
    Drive_Query(letter, &st);
    if (on) c->flags |= flag; else c->flags &= ~flag;
    Config_Save(letter);
    if (flag == DF_UAC && is_mounted(c, &st)) {
        if (on) Elev_Add(net ? EOP_NET : EOP_SUBST, letter, c->path);
        else Elev_Add(net ? EOP_UNNET : EOP_UNSUBST, letter, NULL);
    }
    if (flag == DF_RECYCLE) Engine_SetRecycle(letter, on);
}

static BOOL confirm_foreign(wchar_t letter)
{
    wchar_t q[256];
    StringCchPrintfW(q, ARRAYSIZE(q), T(S_Q_FOREIGN), letter);
    return ask_yes_no_ex(s_wnd, q, NULL, TRUE, TRUE) == IDYES;
}

static void drive_remove(wchar_t letter)
{
    DriveConfig *c;
    if (!IS_DRIVE_LETTER(letter)) return;
    c = &g_cfg[LETTER_INDEX(letter)];
    if (!c->used && !confirm_foreign(letter)) return;
    if (c->used) {
        Engine_Remove(letter, s_wnd, TRUE);
        if (!c->used) Config_Delete(letter);
    } else {
        Engine_UnmountForeign(letter, s_wnd, TRUE);
    }
    if (s_selLetter == letter) s_selLetter = 0;
}

static void drive_adopt(wchar_t letter)
{
    const Row *r = row_for_letter(letter);
    DriveConfig *c;
    wchar_t norm[PATHBUF];
    if (!r || r->managed || !IS_DRIVE_LETTER(letter)) return;
    /* The target as Windows reports it; it must still be a usable path. */
    if (!r->st.target[0] || !Path_Normalize(r->st.target, norm, ARRAYSIZE(norm))) return;
    c = &g_cfg[LETTER_INDEX(letter)];
    c->used = TRUE;
    c->letter = letter;
    c->flags = DF_ENABLED | (r->network ? DF_NETWORK : 0) |
               (g_set.defaultDriveFlags & (uac_available() ? DF_UAC : 0));
    StringCchCopyW(c->path, ARRAYSIZE(c->path), norm);
    c->label[0] = c->icon[0] = 0;
    if ((c->flags & DF_UAC) && !ensure_task(s_wnd)) c->flags &= ~DF_UAC;
    Config_Save(letter);
    Engine_Mount(letter, s_wnd, TRUE);
}

static void after_action(void)
{
    Engine_Commit(s_wnd, TRUE);
    rebuild_list();
}

static void row_click(int part, wchar_t letter)
{
    const Row *r = row_for_letter(letter);
    if (!r) return;
    if (part == PART_TOGGLE) {
        if (r->managed) drive_set_enabled(letter, !(g_cfg[LETTER_INDEX(letter)].flags & DF_ENABLED));
        else if (confirm_foreign(letter)) Engine_UnmountForeign(letter, s_wnd, TRUE);
    } else if (part == PART_REMOVE) {
        drive_remove(letter);
    }
    after_action();
}

static void open_drive(wchar_t letter, BOOL target)
{
    const Row *r = row_for_letter(letter);
    if (!r) return;
    if (target) {
        open_path(r->managed ? g_cfg[LETTER_INDEX(letter)].path : r->st.target);
    } else {
        wchar_t root[4] = { letter, L':', L'\\', 0 };
        open_path(root);
    }
}

static void list_context_menu(int x, int y)
{
    HMENU m = CreatePopupMenu();
    POINT pt;
    int idx = -1, cmd;
    wchar_t letter = 0;
    const Row *r = NULL;

    if (x == -1 && y == -1) {
        RECT ir;
        idx = (int)SendMessageW(s_list, LB_GETCURSEL, 0, 0);
        if (idx >= 0 && SendMessageW(s_list, LB_GETITEMRECT, (WPARAM)idx, (LPARAM)&ir) != LB_ERR) {
            pt.x = ir.left + S(40);
            pt.y = ir.bottom;
        } else {
            pt.x = pt.y = 0;
        }
        ClientToScreen(s_list, &pt);
    } else {
        POINT cp;
        DWORD hit;
        pt.x = x;
        pt.y = y;
        cp = pt;
        ScreenToClient(s_list, &cp);
        hit = (DWORD)SendMessageW(s_list, LB_ITEMFROMPOINT, 0, MAKELPARAM(cp.x, cp.y));
        if (!HIWORD(hit)) idx = (int)LOWORD(hit);
    }
    if (idx >= 0 && idx < s_nrows && s_rows[idx].type == ROW_DRIVE) {
        r = &s_rows[idx];
        letter = r->letter;
        SendMessageW(s_list, LB_SETCURSEL, (WPARAM)idx, 0);
        update_editor_for_letter();
    }

    if (r) {
        const DriveConfig *c = &g_cfg[LETTER_INDEX(letter)];
        BOOL mounted = r->managed ? is_mounted(c, &r->st) : TRUE;
        AppendMenuW(m, MF_STRING | (mounted ? 0 : MF_GRAYED), IDM_D_OPEN, T(S_M_D_OPEN));
        AppendMenuW(m, MF_STRING, IDM_D_OPENPATH, T(S_M_D_OPENPATH));
        SetMenuDefaultItem(m, IDM_D_OPEN, FALSE);
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        if (r->managed) {
            AppendMenuW(m, MF_STRING | ((c->flags & DF_ENABLED) ? MF_CHECKED : 0), IDM_D_ENABLED, T(S_M_D_ENABLED));
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING | ((c->flags & DF_UAC) ? MF_CHECKED : 0) | (uac_available() ? 0 : MF_GRAYED),
                        IDM_D_UAC, T(S_M_D_UAC));
            AppendMenuW(m, MF_STRING | ((c->flags & DF_RECYCLE) ? MF_CHECKED : 0) | (recycle_available() ? 0 : MF_GRAYED),
                        IDM_D_RECYCLE, T(S_M_D_RECYCLE));
            AppendMenuW(m, MF_STRING | ((c->flags & DF_AUTORESTORE) ? MF_CHECKED : 0), IDM_D_AUTO, T(S_M_D_AUTORESTORE));
        } else {
            AppendMenuW(m, MF_STRING, IDM_D_MANAGE, T(S_M_D_MANAGE));
        }
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, IDM_D_REMOVE, T(S_M_D_REMOVE));
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuW(m, MF_STRING, IDM_REFRESH, T(S_M_REFRESH));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_ENABLEALL, T(S_M_ENABLEALL));
    AppendMenuW(m, MF_STRING, IDM_DISABLEALL, T(S_M_DISABLEALL));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_REMOVEALL, T(S_M_REMOVEALL));

    cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, s_wnd, NULL);
    DestroyMenu(m);
    if (!cmd) return;
    if (cmd >= IDM_D_OPEN && cmd <= IDM_D_REMOVE && !letter) return;

    switch (cmd) {
    case IDM_D_OPEN:     open_drive(letter, FALSE); return;
    case IDM_D_OPENPATH: open_drive(letter, TRUE); return;
    case IDM_D_ENABLED:  drive_set_enabled(letter, !(g_cfg[LETTER_INDEX(letter)].flags & DF_ENABLED)); break;
    case IDM_D_UAC:      drive_toggle_flag(letter, DF_UAC); break;
    case IDM_D_RECYCLE:  drive_toggle_flag(letter, DF_RECYCLE); break;
    case IDM_D_AUTO:     drive_toggle_flag(letter, DF_AUTORESTORE); break;
    case IDM_D_MANAGE:   drive_adopt(letter); break;
    case IDM_D_REMOVE:   drive_remove(letter); break;
    default:
        PostMessageW(s_wnd, WM_COMMAND, (WPARAM)cmd, 0);
        return;
    }
    after_action();
}

/* ============================================================
 *  List subclass: hover and clicks on the toggle / remove buttons
 * ============================================================ */

static int hit_part(int idx, POINT pt)
{
    RECT ir, tog, rem;
    if (idx < 0 || idx >= s_nrows || s_rows[idx].type != ROW_DRIVE) return PART_NONE;
    if (SendMessageW(s_list, LB_GETITEMRECT, (WPARAM)idx, (LPARAM)&ir) == LB_ERR) return PART_NONE;
    row_parts(&ir, &tog, &rem);
    InflateRect(&tog, S(3), S(6));
    if (PtInRect(&tog, pt)) return PART_TOGGLE;
    if (PtInRect(&rem, pt)) return PART_REMOVE;
    return PART_NONE;
}

static void invalidate_row(int idx)
{
    RECT ir;
    if (idx >= 0 && SendMessageW(s_list, LB_GETITEMRECT, (WPARAM)idx, (LPARAM)&ir) != LB_ERR)
        InvalidateRect(s_list, &ir, FALSE);
}

static LRESULT CALLBACK list_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)ref;
    switch (msg) {
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, list_proc, id);
        break;
    case WM_MOUSEMOVE: {
        POINT pt;
        DWORD hit;
        int idx = -1, part;
        TRACKMOUSEEVENT tme;
        pt.x = GET_X_LPARAM(lp);
        pt.y = GET_Y_LPARAM(lp);
        hit = (DWORD)SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, lp);
        if (!HIWORD(hit)) idx = (int)LOWORD(hit);
        part = hit_part(idx, pt);
        if (idx != s_hotRow || part != s_hotPart) {
            int old = s_hotRow;
            s_hotRow = idx;
            s_hotPart = part;
            invalidate_row(old);
            invalidate_row(idx);
        }
        ZeroMemory(&tme, sizeof(tme));
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        break;
    }
    case WM_MOUSELEAVE: {
        int old = s_hotRow;
        s_hotRow = -1;
        s_hotPart = PART_NONE;
        invalidate_row(old);
        break;
    }
    case WM_SETCURSOR:
        if (s_hotPart != PART_NONE) {
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        POINT pt;
        DWORD hit;
        int idx = -1, part;
        pt.x = GET_X_LPARAM(lp);
        pt.y = GET_Y_LPARAM(lp);
        hit = (DWORD)SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, lp);
        if (!HIWORD(hit)) idx = (int)LOWORD(hit);
        part = hit_part(idx, pt);
        if (part != PART_NONE) {
            SetFocus(hwnd);
            PostMessageW(s_wnd, WM_APP_ROWCLICK, (WPARAM)part, (LPARAM)s_rows[idx].letter);
            return 0;
        }
        if (idx < 0 || HIWORD(hit) || s_rows[idx].type != ROW_DRIVE) {
            /* Click on a header or empty space: clear the selection. */
            SetFocus(hwnd);
            SendMessageW(hwnd, LB_SETCURSEL, (WPARAM)-1, 0);
            s_selLetter = 0;
            update_mount_caption();
            return 0;
        }
        break;
    }
    case WM_KEYDOWN: {
        int sel = (int)SendMessageW(hwnd, LB_GETCURSEL, 0, 0);
        wchar_t letter = (sel >= 0 && sel < s_nrows && s_rows[sel].type == ROW_DRIVE) ? s_rows[sel].letter : 0;
        if (wp == VK_DELETE && letter) {
            PostMessageW(s_wnd, WM_APP_ROWCLICK, PART_REMOVE, (LPARAM)letter);
            return 0;
        }
        if (wp == VK_SPACE && letter) {
            PostMessageW(s_wnd, WM_APP_ROWCLICK, PART_TOGGLE, (LPARAM)letter);
            return 0;
        }
        break;
    }
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, Theme_SurfaceBrush());
        return 1;
    }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

/* ============================================================
 *  Layout
 * ============================================================ */

static int text_width(HFONT f, const wchar_t *s)
{
    HDC dc = GetDC(s_wnd);
    HGDIOBJ of = SelectObject(dc, f);
    SIZE sz = { 0, 0 };
    wchar_t clean[256];
    int i, j = 0;
    for (i = 0; s[i] && j < 255; i++) if (s[i] != L'&') clean[j++] = s[i];
    clean[j] = 0;
    GetTextExtentPoint32W(dc, clean, j, &sz);
    SelectObject(dc, of);
    ReleaseDC(s_wnd, dc);
    return sz.cx;
}

static int row_height(void)
{
    HDC dc = GetDC(s_wnd);
    HGDIOBJ of = SelectObject(dc, s_font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, of);
    ReleaseDC(s_wnd, dc);
    return tm.tmHeight + S(10);
}

static RECT s_listFrame;

static void layout(void)
{
    RECT rc;
    int W, H, M = S(12), gap = S(8), rh = row_height(), lw, x0, bw, y, i;
    int btnH = rh + S(6);
    HDWP dwp;

    GetClientRect(s_wnd, &rc);
    W = rc.right;
    H = rc.bottom;
    lw = 0;
    for (i = 0; i < 3; i++) {
        wchar_t t[64];
        int tw;
        GetWindowTextW(s_lbl[i], t, ARRAYSIZE(t));
        tw = text_width(s_font, t);
        if (tw > lw) lw = tw;
    }
    lw += S(10);
    x0 = M + lw;
    bw = rh + S(4);

    dwp = BeginDeferWindowPos(24);
#define MOVE(h, x, yy, w, hh) dwp = DeferWindowPos(dwp, h, NULL, x, yy, (w) > 0 ? (w) : 0, hh, SWP_NOZORDER | SWP_NOACTIVATE)

    /* mount row */
    y = H - M - btnH;
    {
        wchar_t t[64];
        int mw;
        GetWindowTextW(s_mount, t, ARRAYSIZE(t));
        mw = text_width(s_font, t) + S(40);
        if (mw < S(130)) mw = S(130);
        {
            int w1 = text_width(s_font, T(S_BTN_MOUNT)) + S(40), w2 = text_width(s_font, T(S_BTN_APPLY)) + S(40);
            if (w1 > mw) mw = w1;
            if (w2 > mw) mw = w2;
        }
        MOVE(s_mount, x0, y, mw, btnH);
    }

    /* drive row */
    y -= gap + rh;
    MOVE(s_lbl[2], M, y, lw, rh);
    MOVE(s_letter, x0, y, S(70), rh * 8);
    {
        int tx = W - M - 3 * bw - 2 * S(4);
        MOVE(s_label, x0 + S(70) + S(6), y, tx - S(6) - (x0 + S(70) + S(6)), rh);
        for (i = 0; i < 3; i++) MOVE(s_tog[i], tx + i * (bw + S(4)), y, bw, rh);
    }

    /* icon row */
    y -= gap + rh;
    MOVE(s_lbl[1], M, y, lw, rh);
    MOVE(s_icon, x0, y, W - M - bw - S(4) - x0, rh);
    MOVE(s_iconBtn, W - M - bw, y, bw, rh);

    /* path row */
    y -= gap + rh;
    MOVE(s_lbl[0], M, y, lw, rh);
    MOVE(s_path, x0, y, W - M - bw - S(4) - x0, rh);
    MOVE(s_pathBtn, W - M - bw, y, bw, rh);

    /* list */
    SetRect(&s_listFrame, M, M, W - M, y - M);
    MOVE(s_list, M + 1, M + 1, W - 2 * M - 2, y - 2 * M - 2);
#undef MOVE
    EndDeferWindowPos(dwp);

    SendMessageW(s_letter, CB_SETITEMHEIGHT, (WPARAM)-1, rh - S(6));
    InvalidateRect(s_wnd, NULL, TRUE);
}

static void set_fonts(void)
{
    HWND kids[] = { s_list, s_path, s_pathBtn, s_icon, s_iconBtn, s_letter, s_label, s_mount,
                    s_lbl[0], s_lbl[1], s_lbl[2] };
    int i;
    for (i = 0; i < (int)ARRAYSIZE(kids); i++) SendMessageW(kids[i], WM_SETFONT, (WPARAM)s_font, TRUE);
}

static void set_texts(void)
{
    wchar_t title[128];
    StringCchCopyW(title, ARRAYSIZE(title), T(S_APP_TITLE));
    if (g_elevated) StringCchCatW(title, ARRAYSIZE(title), T(S_ADMIN_SUFFIX));
    SetWindowTextW(s_wnd, title);
    SetWindowTextW(s_lbl[0], T(S_LBL_PATH));
    SetWindowTextW(s_lbl[1], T(S_LBL_ICON));
    SetWindowTextW(s_lbl[2], T(S_LBL_DRIVE));
    SendMessageW(s_path, EM_SETCUEBANNER, TRUE, (LPARAM)T(S_CUE_PATH));
    SendMessageW(s_label, EM_SETCUEBANNER, TRUE, (LPARAM)T(S_CUE_LABEL));
    SendMessageW(s_icon, EM_SETCUEBANNER, TRUE, (LPARAM)T(S_CUE_ICON));
    SetWindowTextW(s_pathBtn, L"...");
    SetWindowTextW(s_iconBtn, L"...");
    update_mount_caption();

    /* tooltips */
    {
        static const int ids[] = { IDC_T_UAC, IDC_T_RECYCLE, IDC_T_AUTO, IDC_PATHBTN, IDC_ICONBTN };
        StrId tips[5];
        static wchar_t tipbuf[3][512];
        int k;
        tips[0] = S_TIP_UAC; tips[1] = S_TIP_RECYCLE; tips[2] = S_TIP_AUTORESTORE;
        tips[3] = S_TIP_BROWSE; tips[4] = S_TIP_BROWSE;
        for (k = 0; k < 5; k++) {
            TOOLINFOW ti;
            const wchar_t *text = T(tips[k]);
            if (k == 0 && !uac_available()) {
                StringCchPrintfW(tipbuf[0], ARRAYSIZE(tipbuf[0]), L"%s\r\n%s", T(S_TIP_UAC), T(S_TIP_NEEDADMIN));
                text = tipbuf[0];
            } else if (k == 1 && !recycle_available()) {
                StringCchPrintfW(tipbuf[1], ARRAYSIZE(tipbuf[1]), L"%s\r\n%s", T(S_TIP_RECYCLE), T(S_TIP_NEEDADMIN));
                text = tipbuf[1];
            }
            ZeroMemory(&ti, sizeof(ti));
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd = s_wnd;
            ti.uId = (UINT_PTR)GetDlgItem(s_wnd, ids[k]);
            ti.lpszText = (LPWSTR)text;
            SendMessageW(s_tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
            SendMessageW(s_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
    }
}

/* ============================================================
 *  Window creation
 * ============================================================ */

static HWND child(const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex, int id)
{
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                           s_wnd, (HMENU)(INT_PTR)id, g_hinst, NULL);
}

static void create_children(void)
{
    int i;
    /* Owner-drawn without LBS_HASSTRINGS: item i is s_rows[i]. */
    s_list = child(L"LISTBOX", NULL, WS_TABSTOP | WS_VSCROLL | LBS_OWNERDRAWVARIABLE | LBS_NOTIFY |
                   LBS_NOINTEGRALHEIGHT, 0, IDC_LIST);
    SetWindowSubclass(s_list, list_proc, 1, 0);

    s_lbl[0] = child(L"STATIC", NULL, SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, 0, IDC_L_PATH);
    s_path = child(L"EDIT", NULL, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_PATH);
    s_pathBtn = child(L"BUTTON", L"...", WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_PATHBTN);
    s_lbl[1] = child(L"STATIC", NULL, SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, 0, IDC_L_ICON);
    s_icon = child(L"EDIT", NULL, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_ICONEDIT);
    s_iconBtn = child(L"BUTTON", L"...", WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_ICONBTN);
    s_lbl[2] = child(L"STATIC", NULL, SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, 0, IDC_L_DRIVE);
    s_letter = child(L"COMBOBOX", NULL, WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0, IDC_LETTER);
    s_label = child(L"EDIT", NULL, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_LABEL);
    SendMessageW(s_label, EM_SETLIMITTEXT, LABELBUF - 1, 0);
    SendMessageW(s_path, EM_SETLIMITTEXT, PATHBUF - 1, 0);
    SendMessageW(s_icon, EM_SETLIMITTEXT, ICONBUF - 1, 0);
    for (i = 0; i < 3; i++)
        s_tog[i] = child(L"BUTTON", NULL, WS_TABSTOP | BS_OWNERDRAW, 0, IDC_T_UAC + i);
    s_mount = child(L"BUTTON", NULL, WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_MOUNT);

    s_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                            s_wnd, NULL, g_hinst, NULL);
    SendMessageW(s_tip, TTM_SETMAXTIPWIDTH, 0, S(360));
}

static void on_option(int i)
{
    switch (i) {
    case 0:
        if (g_set.flags & AF_STARTUP) {
            if (any_autorestore() &&
                ask_yes_no(s_wnd, T(S_Q_STOPSTARTUP_T), T(S_Q_STOPSTARTUP), TRUE) != IDYES)
                return;
            g_set.flags &= ~AF_STARTUP;
        } else {
            g_set.flags |= AF_STARTUP;
        }
        Startup_Register((g_set.flags & AF_STARTUP) != 0);
        break;
    case 1:
        if (g_set.flags & AF_BACKGROUND) {
            if (any_autorestore() &&
                ask_yes_no(s_wnd, T(S_Q_DISABLEBG_T), T(S_Q_DISABLEBG), TRUE) != IDYES)
                return;
            g_set.flags &= ~AF_BACKGROUND;
        } else {
            g_set.flags |= AF_BACKGROUND;
        }
        tray_update();
        break;
    case 2:
        g_set.flags ^= AF_MANAGENET;
        rebuild_list();
        break;
    case 3:
        g_set.flags ^= AF_ONLYOURS;
        rebuild_list();
        break;
    }
    Settings_Save();
}

static void set_language(const wchar_t *setting)
{
    StringCchCopyW(g_set.language, ARRAYSIZE(g_set.language), setting);
    if (!Lang_Select(g_set.language)) {
        App_Message(s_wnd, T(S_E_LANGFILE), TRUE);
        g_set.language[0] = 0;
        Lang_Select(L"");
    }
    Settings_Save();
    build_menus();
    set_texts();
    layout();
    rebuild_list();
    apply_theme();
}

static void export_language(void)
{
    OPENFILENAMEW ofn;
    wchar_t file[MAX_PATH];
    StringCchCopyW(file, ARRAYSIZE(file), L"MyLanguage.lng");
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = s_wnd;
    ofn.lpstrFilter = L"OpenSubst language (*.lng)\0*.lng\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = ARRAYSIZE(file);
    ofn.lpstrDefExt = L"lng";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return;
    if (Lang_ExportTemplate(file)) info_box(s_wnd, T(S_I_EXPORTED), file);
    else error_code_box(s_wnd, S_E_LANGFILE, GetLastError());
}

static void task_command(BOOL reg)
{
    DWORD code = 0;
    if (reg && !Uac_LocationSecure()) {
        App_Message(s_wnd, T(S_E_INSECURE), TRUE);
        return;
    }
    if (g_elevated) {
        HRESULT hr = reg ? Uac_RegisterTask() : Uac_UnregisterTask();
        code = (DWORD)hr;
        if (FAILED(hr)) { error_code_box(s_wnd, reg ? S_E_REGTASK : S_E_UNREGTASK, code); return; }
    } else {
        if (!Uac_RunElevatedSelf(s_wnd, reg ? L"/REGTASK" : L"/UNREGTASK", &code)) return;
        if (code != 0) { error_code_box(s_wnd, reg ? S_E_REGTASK : S_E_UNREGTASK, code); return; }
    }
    if (reg) {
        /* Bring the elevated session up to date. */
        int i;
        for (i = 0; i < 26; i++) {
            const DriveConfig *c = &g_cfg[i];
            DriveState st;
            if (!c->used || !(c->flags & DF_UAC) || !(c->flags & DF_ENABLED)) continue;
            Drive_Query(c->letter, &st);
            if (is_mounted(c, &st))
                Elev_Add((c->flags & DF_NETWORK) ? EOP_NET : EOP_SUBST, c->letter, c->path);
        }
        Engine_Commit(s_wnd, TRUE);
    }
    info_box(s_wnd, T(S_APP_TITLE), T(reg ? S_I_REGTASK : S_I_UNREGTASK));
}

static void all_drives(int what)    /* 0 enable, 1 disable, 2 remove */
{
    int i;
    if (what == 2 && ask_yes_no_ex(s_wnd, T(S_Q_REMOVEALL), T(S_Q_REMOVEALL_C), TRUE, TRUE) != IDYES)
        return;
    for (i = 0; i < 26; i++) {
        DriveConfig *c = &g_cfg[i];
        if (!c->used) continue;
        if (what == 0) {
            c->flags |= DF_ENABLED;
            Config_Save(c->letter);
            Engine_Mount(c->letter, s_wnd, FALSE);
        } else if (what == 1) {
            if (Engine_Unmount(c->letter, s_wnd, TRUE) != 0) continue;
            c->flags &= ~DF_ENABLED;
            Config_Save(c->letter);
        } else {
            wchar_t l = c->letter;
            Engine_Remove(l, s_wnd, TRUE);
            if (!c->used) Config_Delete(l);
        }
    }
    if (what == 2) s_selLetter = 0;
    after_action();
}

static void request_exit(void)
{
    if (any_autorestore() && IsWindowVisible(s_wnd) &&
        ask_yes_no(s_wnd, T(S_Q_EXIT_T), T(S_Q_EXIT), TRUE) != IDYES)
        return;
    DestroyWindow(s_wnd);
}

static void on_close(void)
{
    if ((g_set.flags & AF_BACKGROUND) || s_tempTray) {
        show_main(FALSE);
        return;
    }
    if (any_autorestore() && !g_quiet) {
        TASKDIALOGCONFIG tc;
        TASKDIALOG_BUTTON btns[2];
        int btn = IDCANCEL;
        btns[0].nButtonID = 100; btns[0].pszButtonText = T(S_BTN_HIDE);
        btns[1].nButtonID = 101; btns[1].pszButtonText = T(S_BTN_EXIT);
        ZeroMemory(&tc, sizeof(tc));
        tc.cbSize = sizeof(tc);
        tc.hwndParent = s_wnd;
        tc.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
        tc.pszWindowTitle = T(S_APP_TITLE);
        tc.pszMainIcon = TD_INFORMATION_ICON;
        tc.pszMainInstruction = T(S_Q_KEEP_T);
        tc.pszContent = T(S_Q_KEEP);
        tc.pButtons = btns;
        tc.cButtons = 2;
        tc.nDefaultButton = 100;
        TaskDialogIndirect(&tc, &btn, NULL, NULL);
        if (btn == 100) {
            s_tempTray = TRUE;
            tray_update();
            show_main(FALSE);
            return;
        }
        if (btn != 101) return;
    }
    DestroyWindow(s_wnd);
}

/* ============================================================
 *  First run and restore prompt
 * ============================================================ */

static void welcome(void)
{
    static const KNOWNFOLDERID *const ids[3] = { &FOLDERID_Documents, &FOLDERID_Pictures, &FOLDERID_Videos };
    wchar_t content[2048], list[1024];
    wchar_t paths[3][MAX_PATH], names[3][64];
    wchar_t letters[3];
    DWORD used = Drive_UsedMask();
    int n = 0, i;
    wchar_t l = L'Z';

    list[0] = 0;
    for (i = 0; i < 3; i++) {
        wchar_t *p = NULL;
        if (FAILED(SHGetKnownFolderPath(ids[i], 0, NULL, &p)) || !p) continue;
        while (l >= L'D' && ((used & (1u << LETTER_INDEX(l))) || g_cfg[LETTER_INDEX(l)].used)) l--;
        if (l < L'D') { CoTaskMemFree(p); break; }
        StringCchCopyW(paths[n], MAX_PATH, p);
        CoTaskMemFree(p);
        {
            IShellItem *si = NULL;
            names[n][0] = 0;
            if (SUCCEEDED(SHCreateItemFromParsingName(paths[n], NULL, &IID_IShellItem, (void **)&si))) {
                wchar_t *dn = NULL;
                if (SUCCEEDED(IShellItem_GetDisplayName(si, SIGDN_NORMALDISPLAY, &dn)) && dn) {
                    StringCchCopyW(names[n], 64, dn);
                    CoTaskMemFree(dn);
                }
                IShellItem_Release(si);
            }
            if (!names[n][0]) StringCchCopyW(names[n], 64, PathFindFileNameW(paths[n]));
        }
        letters[n] = l--;
        {
            wchar_t line[MAX_PATH + 96];
            StringCchPrintfW(line, ARRAYSIZE(line), L"\r\n%c:  %s  (%s)", letters[n], names[n], paths[n]);
            StringCchCatW(list, ARRAYSIZE(list), line);
        }
        n++;
    }
    g_set.flags |= AF_WELCOMED;
    Settings_Save();
    if (!n) return;

    StringCchPrintfW(content, ARRAYSIZE(content), L"%s\r\n\r\n%s%s", T(S_W_WELCOME_Q), T(S_W_WELCOME_C), list);
    if (ask_yes_no(s_wnd, T(S_W_WELCOME_T), content, FALSE) != IDYES) return;
    for (i = 0; i < n; i++) {
        DriveConfig *c = &g_cfg[LETTER_INDEX(letters[i])];
        c->used = TRUE;
        c->letter = letters[i];
        c->flags = DF_ENABLED;
        StringCchCopyW(c->path, ARRAYSIZE(c->path), paths[i]);
        StringCchCopyW(c->label, ARRAYSIZE(c->label), names[i]);
        c->icon[0] = 0;
        if (Engine_Mount(c->letter, s_wnd, TRUE) == 0) Config_Save(c->letter);
        else c->used = FALSE;
    }
    after_action();
}

static void after_show(void)
{
    int i, missing = 0;
    if (g_elevated && !g_quiet) info_box(s_wnd, T(S_APP_TITLE), T(S_I_ELEVATED));
    /* A task left over from an unprotected location is a standing UAC bypass:
       offer to remove it (it is never used, see Uac_TaskRegistered). */
    if ((g_splitToken || g_elevated) && !g_quiet && Uac_TaskPresent() && !Uac_LocationSecure() &&
        ask_yes_no_ex(s_wnd, T(S_Q_INSECURETASK_T), T(S_Q_INSECURETASK), TRUE, TRUE) == IDYES)
        task_command(FALSE);
    if (!(g_set.flags & AF_WELCOMED) && managed_count() == 0) {
        if (!s_noWelcome && !g_quiet) welcome();
        else { g_set.flags |= AF_WELCOMED; Settings_Save(); }
        return;
    }
    for (i = 0; i < 26; i++) {
        const DriveConfig *c = &g_cfg[i];
        DriveState st;
        if (!c->used || !(c->flags & DF_ENABLED)) continue;
        Drive_Query(c->letter, &st);
        if (st.kind == DK_NONE || st.kind == DK_NETUNAVAIL) missing++;
    }
    if (missing && !g_quiet && ask_yes_no(s_wnd, T(S_Q_RESTORE), NULL, FALSE) == IDYES) {
        Engine_RestoreAll(s_wnd, TRUE);
        rebuild_list();
    }
}

/* ============================================================
 *  Window procedure
 * ============================================================ */

static void paint_main(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(dc, &rc, Theme_BgBrush());
    if (!IsRectEmpty(&s_listFrame)) {
        HBRUSH b = CreateSolidBrush(Theme_Line());
        FrameRect(dc, &s_listFrame, b);
        DeleteObject(b);
    }
    EndPaint(hwnd, &ps);
}

static void handle_command(int id, int code, HWND ctl)
{
    (void)ctl;
    if (id >= IDM_LANG_FILE && id < IDM_LANG_FILE + s_nlangs) {
        set_language(s_langs[id - IDM_LANG_FILE].file);
        return;
    }
    if (id >= IDM_T_DRIVE && id < IDM_T_DRIVE + 26) {
        wchar_t l = (wchar_t)(L'A' + (id - IDM_T_DRIVE));
        drive_set_enabled(l, !(g_cfg[LETTER_INDEX(l)].flags & DF_ENABLED));
        after_action();
        return;
    }
    switch (id) {
    case IDC_MOUNT:
        if (GetFocus() == s_list) {
            if (s_selLetter) open_drive(s_selLetter, FALSE);
        } else {
            on_mount();
        }
        break;
    case IDCANCEL:
        if ((g_set.flags & AF_BACKGROUND) || s_tempTray) show_main(FALSE);
        break;
    case IDC_PATHBTN: browse_folder(); break;
    case IDC_ICONBTN: browse_icon(); break;
    case IDC_T_UAC:
    case IDC_T_RECYCLE:
    case IDC_T_AUTO: {
        int k = id - IDC_T_UAC;
        if (k == 0 && !s_togState[0] && !ensure_task(s_wnd)) break;
        s_togState[k] = !s_togState[k];
        if (k == 2 && s_togState[2]) ask_background_for_autorestore();
        InvalidateRect(s_tog[k], NULL, FALSE);
        break;
    }
    case IDM_O_STARTUP:
    case IDM_O_BACKGROUND:
    case IDM_O_MANAGENET:
    case IDM_O_ONLYOURS:
        on_option(id - IDM_O_STARTUP);
        break;
    case IDC_LIST:
        if (code == LBN_SELCHANGE) update_editor_for_letter();
        else if (code == LBN_DBLCLK && s_selLetter) open_drive(s_selLetter, FALSE);
        break;
    case IDC_LETTER:
        if (code == CBN_SELCHANGE) update_mount_caption();
        break;

    case IDM_REFRESH:
        Config_LoadAll();
        rebuild_list();
        break;
    case IDM_ENABLEALL:  all_drives(0); break;
    case IDM_DISABLEALL: all_drives(1); break;
    case IDM_REMOVEALL:  all_drives(2); break;
    case IDM_REGTASK:    task_command(TRUE); break;
    case IDM_UNREGTASK:  task_command(FALSE); break;
    case IDM_EXIT:       request_exit(); break;
    case IDM_LANG_AUTO:  set_language(L""); break;
    case IDM_LANG_EN:    set_language(L"en"); break;
    case IDM_LANG_DE:    set_language(L"de"); break;
    case IDM_EXPORTLANG: export_language(); break;
    case IDM_THEME_AUTO:
    case IDM_THEME_LIGHT:
    case IDM_THEME_DARK:
        g_set.theme = (DWORD)(id - IDM_THEME_AUTO);
        Settings_Save();
        apply_theme();
        break;
    case IDM_ONTOP:
        g_set.flags ^= AF_ONTOP;
        SetWindowPos(s_wnd, (g_set.flags & AF_ONTOP) ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        Settings_Save();
        break;
    case IDM_TRAYICON:
        g_set.flags ^= AF_TRAYICON;
        Settings_Save();
        tray_update();
        break;
    case IDM_GROUP:
        g_set.flags ^= AF_GROUP;
        Settings_Save();
        rebuild_list();
        break;
    case IDM_CLIHELP:
        info_box(s_wnd, T(S_CLI_TITLE), T(S_CLI_USAGE));
        break;
    case IDM_ABOUT: {
        wchar_t head[64];
        StringCchPrintfW(head, ARRAYSIZE(head), L"%s %s", T(S_APP_TITLE), APP_VERSION);
        info_box(s_wnd, head, T(S_A_ABOUT));
        break;
    }
    case IDM_T_SHOWHIDE:
        show_main(!IsWindowVisible(s_wnd));
        break;
    }
}

/* ------------------------------------------------------------
 *  Re-entrancy
 *
 *  Message boxes, menus and the waits for the elevated side run nested
 *  message loops. Without care, a timer, a finished network connection or a
 *  refresh request from another instance would then run in the middle of an
 *  action (e.g. between unmounting a drive and storing its new path). While
 *  an action runs, such messages are therefore held back and processed once
 *  the outermost action has finished.
 * ------------------------------------------------------------ */

static int    s_busy;
static BOOL   s_refreshPending, s_pollPending;
static LPARAM s_deferredJobs[26];
static int    s_nDeferred;

static LRESULT CALLBACK main_proc_inner(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

static BOOL ui_busy(void) { return s_busy > 0 || Elev_IsBusy(); }

static void process_deferred(void)
{
    if (!s_wnd || !IsWindow(s_wnd)) return;
    while (!ui_busy() && (s_nDeferred || s_refreshPending || s_pollPending)) {
        if (s_nDeferred) {
            LPARAM job = s_deferredJobs[--s_nDeferred];
            SendMessageW(s_wnd, WM_APP_NETDONE, 0, job);
        } else if (s_refreshPending) {
            s_refreshPending = FALSE;
            SendMessageW(s_wnd, s_msgRefresh, 0, 0);
        } else {
            s_pollPending = FALSE;
            SendMessageW(s_wnd, WM_TIMER, TIMER_SOON, 0);
        }
        if (!IsWindow(s_wnd)) return;
    }
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    BOOL action;
    LRESULT r;

    if (ui_busy()) {
        if (msg == WM_APP_NETDONE) {
            /* Keep only completions of jobs that really exist. */
            if (Engine_IsJob(lp) && s_nDeferred < (int)ARRAYSIZE(s_deferredJobs))
                s_deferredJobs[s_nDeferred++] = lp;
            return 0;
        }
        if (msg == s_msgRefresh && msg) { s_refreshPending = TRUE; return 0; }
        if (msg == WM_TIMER) { if (wp != TIMER_RESTORE) s_pollPending = TRUE; return 0; }
        /* User input that can only arrive posted while an action runs. */
        if (msg == WM_APP_TRAY || msg == WM_APP_ROWCLICK || msg == WM_CONTEXTMENU ||
            msg == WM_DROPFILES || msg == WM_CLOSE || msg == WM_APP_AFTERSHOW) {
            if (msg == WM_DROPFILES) DragFinish((HDROP)wp);
            return 0;
        }
        if (msg == WM_COMMAND && lp == 0 && Elev_IsBusy()) return 0;
    }

    action = msg == WM_COMMAND || msg == WM_CONTEXTMENU || msg == WM_APP_ROWCLICK ||
             msg == WM_APP_NETDONE || msg == WM_APP_AFTERSHOW || msg == WM_APP_TRAY ||
             msg == WM_DROPFILES || msg == WM_CLOSE || msg == WM_TIMER ||
             (msg == s_msgRefresh && msg);
    if (!action) return main_proc_inner(hwnd, msg, wp, lp);

    s_busy++;
    r = main_proc_inner(hwnd, msg, wp, lp);
    if (--s_busy == 0) process_deferred();
    return r;
}

static LRESULT CALLBACK main_proc_inner(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT res;
    if (Theme_MenuBarMessage(hwnd, msg, wp, lp, &res)) return res;

    if (msg == s_msgTaskbarCreated && msg) {
        s_trayAdded = FALSE;
        tray_update();
        return 0;
    }
    if (msg == s_msgShowMe && msg) {
        show_main(TRUE);
        return 0;
    }
    if (msg == s_msgRefresh && msg) {
        Config_LoadAll();
        rebuild_list();
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        s_wnd = hwnd;
        s_dpi = window_dpi(hwnd);
        create_fonts();
        load_icons();
        create_children();
        set_toggles(g_set.defaultDriveFlags);
        set_fonts();
        build_menus();
        set_texts();
        DragAcceptFiles(hwnd, TRUE);
        return 0;

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) layout();
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S(400);
        mm->ptMinTrackSize.y = S(380);
        return 0;
    }

    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        s_dpi = HIWORD(wp);
        create_fonts();
        load_icons();
        set_fonts();
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        rebuild_list();
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        paint_main(hwnd);
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND c = (HWND)lp;
        SetBkMode(dc, TRANSPARENT);
        SetBkColor(dc, Theme_Bg());
        (void)c;
        SetTextColor(dc, Theme_Text());
        return (LRESULT)Theme_BgBrush();
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, Theme_Text());
        SetBkColor(dc, Theme_Surface());
        return (LRESULT)Theme_SurfaceBrush();
    }

    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lp;
        if (mi->CtlID == IDC_LIST) {
            int i = (int)mi->itemID;
            int type = (i >= 0 && i < s_nrows) ? s_rows[i].type : ROW_DRIVE;
            mi->itemHeight = (UINT)(type == ROW_GROUP ? S(30) : type == ROW_EMPTY ? S(80) : S(56));
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *di = (const DRAWITEMSTRUCT *)lp;
        if (di->CtlID == IDC_LIST) { draw_row(di); return TRUE; }
        if (di->CtlID >= IDC_T_UAC && di->CtlID <= IDC_T_AUTO) {
            draw_flag_button(di, (int)(di->CtlID - IDC_T_UAC));
            return TRUE;
        }
        break;
    }

    case WM_COMMAND:
        handle_command(LOWORD(wp), HIWORD(wp), (HWND)lp);
        return 0;

    case WM_CONTEXTMENU:
        if ((HWND)wp == s_list) {
            list_context_menu(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        }
        break;

    case WM_INITMENUPOPUP:
        init_menu_popup((HMENU)wp);
        return 0;

    case WM_APP_ROWCLICK:
        row_click((int)wp, (wchar_t)lp);
        return 0;

    case WM_APP_NETDONE:
        Engine_OnNetDone(lp, IsWindowVisible(hwnd) ? hwnd : NULL);
        rebuild_list();
        if (s_exitWhenIdle && Engine_PendingCount() == 0) DestroyWindow(hwnd);
        return 0;

    case WM_APP_AFTERSHOW:
        after_show();
        return 0;

    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            show_main(!IsWindowVisible(hwnd) || IsIconic(hwnd));
            break;
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
            tray_menu();
            break;
        }
        return 0;

    case WM_TIMER:
        if (wp == TIMER_POLL || wp == TIMER_SOON) {
            if (wp == TIMER_SOON) KillTimer(hwnd, TIMER_SOON);
            if (compute_signature(NULL) != s_signature) rebuild_list();
            if (tray_wanted() && ++s_trayProbe % 15 == 0) tray_update();
        } else if (wp == TIMER_RESTORE) {
            if (Engine_AutoRestoreTick(hwnd)) rebuild_list();
        }
        return 0;

    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE)
            SetTimer(hwnd, TIMER_SOON, 600, NULL);
        return TRUE;

    case WM_SETTINGCHANGE:
        if (Theme_IsColorSchemeChange(msg, lp) && g_set.theme == THEME_AUTO) apply_theme();
        break;

    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
        apply_theme();
        break;

    case WM_DROPFILES:
        on_drop((HDROP)wp);
        return 0;

    case WM_CLOSE:
        on_close();
        return 0;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        if (wp) Settings_Save();
        return 0;

    case WM_DESTROY: {
        WINDOWPLACEMENT wpl;
        ZeroMemory(&wpl, sizeof(wpl));
        wpl.length = sizeof(wpl);
        if (GetWindowPlacement(hwnd, &wpl)) g_set.winRect = wpl.rcNormalPosition;
        Settings_Save();
        g_set.flags &= ~AF_TRAYICON;
        g_set.flags &= ~AF_BACKGROUND;
        s_tempTray = FALSE;
        tray_update();
        free_rows();
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ============================================================
 *  Public
 * ============================================================ */

BOOL Ui_Create(BOOL startup, BOOL noWelcome)
{
    WNDCLASSEXW wc;
    ACCEL acc[1];
    int w, h, x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    DWORD exStyle = 0;

    s_startup = startup;
    s_noWelcome = noWelcome;
    s_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    s_msgShowMe = RegisterWindowMessageW(MSG_SHOWME);
    s_msgRefresh = RegisterWindowMessageW(MSG_REFRESH);

    s_icoApp = (HICON)LoadImageW(g_hinst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    s_icoAppSmall = (HICON)LoadImageW(g_hinst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = main_proc;
    wc.hInstance = g_hinst;
    wc.hIcon = s_icoApp;
    wc.hIconSm = s_icoAppSmall;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = APP_WNDCLASS;
    if (!RegisterClassExW(&wc)) return FALSE;

    Theme_Init();
    Theme_SetMode(resolve_theme());
    Gfx_Init();

    {
        int dpi = window_dpi(NULL);
        w = MulDiv(460, dpi, 96);
        h = MulDiv(500, dpi, 96);
    }
    if (g_set.winRect.right > g_set.winRect.left && g_set.winRect.bottom > g_set.winRect.top) {
        RECT r = g_set.winRect;
        HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
        if (mon) {
            x = r.left;
            y = r.top;
            w = r.right - r.left;
            h = r.bottom - r.top;
        }
    }
    if (g_set.flags & AF_ONTOP) exStyle |= WS_EX_TOPMOST;
    exStyle |= WS_EX_CONTROLPARENT;

    s_wnd = CreateWindowExW(exStyle, APP_WNDCLASS, T(S_APP_TITLE),
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h,
                            NULL, NULL, g_hinst, NULL);
    if (!s_wnd) return FALSE;
    Engine_SetNotifyWindow(s_wnd);
    apply_theme();
    layout();
    rebuild_list();

    acc[0].fVirt = FVIRTKEY;
    acc[0].key = VK_F5;
    acc[0].cmd = IDM_REFRESH;
    s_accel = CreateAcceleratorTableW(acc, 1);

    SetTimer(s_wnd, TIMER_POLL, 2000, NULL);
    SetTimer(s_wnd, TIMER_RESTORE, 5000, NULL);

    if (startup || ((g_set.flags & AF_BACKGROUND) && (g_set.flags & AF_WELCOMED))) {
        /* Autostart, or background mode: restore silently, stay in the tray. */
        Engine_RestoreAll(NULL, FALSE);
        rebuild_list();
        if (!(g_set.flags & AF_BACKGROUND)) {
            /* Nothing to keep running for: leave once network drives are done. */
            s_exitWhenIdle = TRUE;
            if (Engine_PendingCount() == 0) {
                DestroyWindow(s_wnd);
                return TRUE;
            }
        }
        tray_update();
    } else {
        ShowWindow(s_wnd, SW_SHOW);
        UpdateWindow(s_wnd);
        tray_update();
        Theme_ApplyToMainWindow(s_wnd);
        s_afterShowPosted = TRUE;
        PostMessageW(s_wnd, WM_APP_AFTERSHOW, 0, 0);
    }
    return TRUE;
}

int Ui_Run(void)
{
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (s_wnd && s_accel && TranslateAcceleratorW(s_wnd, s_accel, &msg)) continue;
        if (s_wnd && IsDialogMessageW(s_wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Gfx_Shutdown();
    return (int)msg.wParam;
}
