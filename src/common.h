/*
 * OpenSubst - free and open-source virtual drive manager for Windows.
 * Copyright (c) 2026 Rekow IT. MIT License, see LICENSE.
 *
 * common.h - shared includes, constants and data types.
 */
#ifndef OPENSUBST_COMMON_H
#define OPENSUBST_COMMON_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x06010000
#endif
#define COBJMACROS
#define OEMRESOURCE

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <winnetwk.h>
#include <dbt.h>
#include <strsafe.h>

#define APP_NAME        L"OpenSubst"
#define APP_VERSION     L"1.0.2"
#define APP_REGKEY      L"Software\\OpenSubst"
#define APP_WNDCLASS    L"OpenSubst.MainWindow"
#define APP_MUTEX       L"Local\\OpenSubst.SingleInstance"
#define APP_TASKNAME    L"OpenSubstUAC"

/* Room for paths longer than MAX_PATH: virtual drives are created with a raw
   NT target ("\??\..."), which is not limited to 260 characters. */
#define PATHBUF   2048
#define LABELBUF  128
#define ICONBUF   (MAX_PATH + 16)

/* ---- per-drive flags (stored in the registry) ---- */
#define DF_ENABLED      0x0001u   /* drive should be mounted               */
#define DF_UAC          0x0002u   /* also mount for elevated applications  */
#define DF_RECYCLE      0x0004u   /* Recycle Bin on this drive             */
#define DF_AUTORESTORE  0x0008u   /* re-mount whenever the drive goes away */
#define DF_NETWORK      0x0010u   /* network drive (UNC target)            */

/* ---- application flags ---- */
#define AF_STARTUP      0x0001u   /* create drives at Windows startup      */
#define AF_BACKGROUND   0x0002u   /* keep running in the tray              */
#define AF_MANAGENET    0x0004u   /* manage network drives                 */
#define AF_ONLYOURS     0x0008u   /* hide drives OpenSubst does not manage */
#define AF_ONTOP        0x0010u   /* always on top                         */
#define AF_TRAYICON     0x0020u   /* show tray icon while window is open   */
#define AF_GROUP        0x0040u   /* group the list by drive type          */
#define AF_WELCOMED     0x0100u   /* first-run welcome already shown       */

#define THEME_AUTO      0
#define THEME_LIGHT     1
#define THEME_DARK      2

typedef struct DriveConfig {
    BOOL    used;
    wchar_t letter;
    DWORD   flags;
    wchar_t path[PATHBUF];      /* folder or UNC path */
    wchar_t label[LABELBUF];    /* Explorer label, may be empty */
    wchar_t icon[ICONBUF];      /* "file,index", may be empty */
} DriveConfig;

typedef struct AppSettings {
    DWORD   flags;
    DWORD   theme;
    DWORD   defaultDriveFlags;  /* toggle states for the next new drive */
    wchar_t language[MAX_PATH]; /* "" = automatic, "en", "de" or a .lng file name */
    RECT    winRect;            /* in screen pixels, empty = default */
} AppSettings;

/* Current state of a drive letter as seen in this logon session. */
enum {
    DK_NONE = 0,        /* letter is free                              */
    DK_SUBST,           /* virtual drive (DOS device "\??\...")        */
    DK_NETWORK,         /* connected network drive                     */
    DK_NETUNAVAIL,      /* remembered network drive, not connected     */
    DK_OTHER            /* a real volume, CD-ROM, ...                  */
};

typedef struct DriveState {
    int     kind;
    wchar_t target[PATHBUF];    /* folder for DK_SUBST, UNC for network */
} DriveState;

/* Globals shared between modules (defined in main.c). */
extern HINSTANCE   g_hinst;
extern AppSettings g_set;
extern DriveConfig g_cfg[26];
extern wchar_t     g_exePath[MAX_PATH];
extern wchar_t     g_exeDir[MAX_PATH];

/* Capabilities of the running process (see main.c). */
extern BOOL g_elevated;     /* this process runs with a full admin token        */
extern BOOL g_splitToken;   /* admin with UAC filtering, running non-elevated   */
extern BOOL g_isAdminNow;   /* may write HKLM right now                          */
extern BOOL g_quiet;        /* /NOMSG: suppress message boxes                    */

/* Upper-case a single character (CharUpper's "character in the pointer" mode). */
static __inline wchar_t ch_upper(wchar_t c)
{
    return (wchar_t)(ULONG_PTR)CharUpperW((LPWSTR)(ULONG_PTR)c);
}

#define LETTER_INDEX(c) ((int)((c) - L'A'))
#define IS_DRIVE_LETTER(c) ((c) >= L'A' && (c) <= L'Z')

#endif /* OPENSUBST_COMMON_H */
