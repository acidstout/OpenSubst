/*
 * drives.h - low-level drive operations for the current logon session:
 * virtual drives (DOS devices), network drives (WNet) and the Explorer
 * label/icon overrides.
 *
 * Copyright (c) 2026 Rekow IT. MIT License, see LICENSE.
 */
#ifndef OPENSUBST_DRIVES_H
#define OPENSUBST_DRIVES_H

#include "common.h"

void  Drive_Query(wchar_t letter, DriveState *st);

/* Virtual drives. Return a Win32 error code (0 = success). */
DWORD Drive_MountSubst(wchar_t letter, const wchar_t *path);
DWORD Drive_UnmountSubst(wchar_t letter);

/* Network drives. 'errText' receives provider-specific error text, if any. */
DWORD Drive_MountNet(wchar_t letter, const wchar_t *unc, HWND owner, BOOL interactive,
                     wchar_t *errText, size_t cchErr);
DWORD Drive_UnmountNet(wchar_t letter, BOOL force);

/* Explorer label/icon override for a drive letter. Empty strings remove the
   respective override. 'unc' is the share for network drives, else NULL. */
void  Drive_SetShellInfo(wchar_t letter, const wchar_t *label, const wchar_t *icon,
                         const wchar_t *unc);
void  Drive_ClearShellInfo(wchar_t letter, const wchar_t *unc, const wchar_t *ourLabel);
void  Drive_NotifyShell(wchar_t letter, BOOL added);
void  Drive_NotifyShellInfo(wchar_t letter, BOOL iconChanged);

/* Paths. */
BOOL  Path_IsUnc(const wchar_t *p);
/* Turn user input into a canonical absolute path: removes quotes, "\\?\",
   trailing backslashes (except for roots). Returns FALSE if not usable. */
BOOL  Path_Normalize(const wchar_t *in, wchar_t *out, size_t cch);
BOOL  Path_IsDirectory(const wchar_t *p);
/* Same, but gives up after 'ms' (an unreachable server then counts as missing).
   Use this on the window thread. */
BOOL  Path_IsDirectoryTimeout(const wchar_t *p, DWORD ms);
/* TRUE for "X:\...", "\\server\share" (optionally with "\\?\" / "\??\"):
   input that does not depend on the current directory. */
BOOL  Path_IsAbsoluteSpec(const wchar_t *p);

#define DIR_TIMEOUT_UI   1500   /* list refresh, Auto-Restore */
#define DIR_TIMEOUT_USER 8000   /* the user clicked something  */

void  Error_Text(DWORD err, wchar_t *out, size_t cch);

/* Bitmask of letters currently in use in this session (bit 0 = A:). */
DWORD Drive_UsedMask(void);

#endif
