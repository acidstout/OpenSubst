/*
 * engine.h - drive management on top of the primitives: mounting stored
 * drives (including their label, icon, UAC and Recycle Bin parts),
 * asynchronous network connections and Auto-Restore.
 *
 * Copyright (c) 2026 Rekow IT. MIT License, see LICENSE.
 */
#ifndef OPENSUBST_ENGINE_H
#define OPENSUBST_ENGINE_H

#include "common.h"

#define WM_APP_NETDONE  (WM_APP + 10)   /* lParam = job, pass to Engine_OnNetDone */

/* Window that receives WM_APP_NETDONE. Without one, network drives are
   connected synchronously (command line use). */
void  Engine_SetNotifyWindow(HWND hwnd);

/* Mount/unmount the stored drive for 'letter'. Network drives connect in
   the background when a notify window is set; the call then returns
   ERROR_IO_PENDING. With 'interactive', errors are reported to the user. */
DWORD Engine_Mount(wchar_t letter, HWND owner, BOOL interactive);
DWORD Engine_Unmount(wchar_t letter, HWND owner, BOOL interactive);

/* Unmount and forget a stored drive (also its Recycle Bin registration). */
void  Engine_Remove(wchar_t letter, HWND owner, BOOL interactive);

/* Unmount a drive OpenSubst does not manage. */
DWORD Engine_UnmountForeign(wchar_t letter, HWND owner, BOOL interactive);

/* Re-apply label and icon of a mounted drive after editing them. */
void  Engine_RefreshShellInfo(wchar_t letter, const wchar_t *oldLabel, const wchar_t *oldIcon);

/* Switch the per-drive Recycle Bin on or off (queues the admin part). */
void  Engine_SetRecycle(wchar_t letter, BOOL on);

/* Execute queued elevated operations. */
void  Engine_Commit(HWND owner, BOOL interactive);

/* Mount all enabled stored drives that are not mounted. */
void  Engine_RestoreAll(HWND owner, BOOL interactive);

/* Periodic Auto-Restore check. Returns TRUE if something was started. */
BOOL  Engine_AutoRestoreTick(HWND owner);

/* Completion of a background network connection. Returns its letter. */
wchar_t Engine_OnNetDone(LPARAM job, HWND owner);
/* TRUE if 'job' is a pending background connection (validates WM_APP_NETDONE). */
BOOL  Engine_IsJob(LPARAM job);

BOOL  Engine_IsPending(wchar_t letter);
int   Engine_PendingCount(void);

/* Implemented by main.c: show or print a message. */
void  App_Message(HWND owner, const wchar_t *text, BOOL error);

#endif
