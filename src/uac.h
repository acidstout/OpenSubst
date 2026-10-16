/*
 * uac.h - drives for elevated applications.
 *
 * With UAC, an administrator has two logon sessions: the filtered one the
 * desktop runs in and the elevated one. Each has its own set of DOS devices,
 * so a virtual drive created normally is invisible to elevated programs.
 *
 * OpenSubst registers a scheduled task (once, with one UAC prompt) that runs
 * "OpenSubst.exe /UACTASK <payload>" with the highest available rights. The
 * task can be started by the user without a prompt, and the elevated copy
 * then repeats the drive operations in the elevated session. The payload is
 * hex-encoded, so nothing in it can be mistaken for command-line syntax, and
 * every operation is validated again on the elevated side: only free letters
 * or letters that already hold a virtual/network drive can be touched, never
 * the system drive or a real volume.
 *
 * Without a registered task the same payload can be run once through a normal
 * "Run as administrator" prompt (used for the Recycle Bin registration).
 *
 * Copyright (c) 2026 Rekow IT. MIT License, see LICENSE.
 */
#ifndef OPENSUBST_UAC_H
#define OPENSUBST_UAC_H

#include "common.h"

/* Operation codes for Elev_Add. */
#define EOP_SUBST       L'S'    /* arg = folder */
#define EOP_UNSUBST     L's'
#define EOP_NET         L'N'    /* arg = UNC path */
#define EOP_UNNET       L'n'
#define EOP_RECYCLE     L'R'    /* machine part of the Recycle Bin */
#define EOP_UNRECYCLE   L'r'

void  Elev_Add(wchar_t op, wchar_t letter, const wchar_t *arg);
BOOL  Elev_Pending(void);
void  Elev_Discard(void);

/* Execute the queued operations. Namespace operations (S/s/N/n) run only
   through the UAC task; Recycle Bin operations run directly when this
   process is an administrator, through the task, or - if allowPrompt is set -
   through a UAC prompt. Returns 0 if everything succeeded. */
DWORD Elev_Flush(HWND owner, BOOL allowPrompt);

/* TRUE while Elev_Flush waits for the elevated side (messages are pumped). */
BOOL  Elev_IsBusy(void);

/* TRUE if no standard (non-elevated) process can replace this exe, plant a
   DLL next to it or rename its folder. The UAC task is only allowed then. */
BOOL  Uac_LocationSecure(void);

/* Task exists, starts this exe and the exe's location is secure. */
BOOL    Uac_TaskRegistered(void);
/* Task exists and starts this exe (regardless of the location). */
BOOL    Uac_TaskPresent(void);
HRESULT Uac_RegisterTask(void);      /* this process must be elevated */
HRESULT Uac_UnregisterTask(void);    /* this process must be elevated */

/* Start this exe elevated with the given arguments and wait for it.
   Returns FALSE if the user cancelled the prompt. */
BOOL  Uac_RunElevatedSelf(HWND owner, const wchar_t *args, DWORD *exitCode);

/* Entry point of the elevated side (/UACTASK or /ELEVATED). */
int   Uac_ElevatedMain(const wchar_t *payload, BOOL fromTask);

#endif
