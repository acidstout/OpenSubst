/*
 * ui.h - main window, tray icon and dialogs.
 *
 * Copyright (c) 2026 Rekow IT. MIT License, see LICENSE.
 */
#ifndef OPENSUBST_UI_H
#define OPENSUBST_UI_H

#include "common.h"

/* Messages other instances send to the running one (RegisterWindowMessage). */
#define MSG_SHOWME   L"OpenSubst.ShowMe"
#define MSG_REFRESH  L"OpenSubst.Refresh"

/* Create the main window. 'startup' = launched from autostart (/STARTUP):
   restore drives and stay hidden. */
BOOL Ui_Create(BOOL startup, BOOL noWelcome);
int  Ui_Run(void);

#endif
