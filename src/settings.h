/*
 * settings.h - persistent settings in HKCU\Software\OpenSubst.
 *
 * Copyright (c) 2026 Rekow IT. MIT License, see LICENSE.
 */
#ifndef OPENSUBST_SETTINGS_H
#define OPENSUBST_SETTINGS_H

#include "common.h"

void Settings_Load(void);
void Settings_Save(void);

/* Stored drives: HKCU\Software\OpenSubst\Drives\<letter>. */
void Config_LoadAll(void);
void Config_Save(wchar_t letter);
void Config_Delete(wchar_t letter);

/* Autostart entry in HKCU\...\Run. */
BOOL Startup_IsRegistered(void);
void Startup_Register(BOOL on);

/* Small registry helpers shared with other modules. */
BOOL Reg_SetString(HKEY root, const wchar_t *key, const wchar_t *name,
                   const wchar_t *value, REGSAM view);
BOOL Reg_GetString(HKEY root, const wchar_t *key, const wchar_t *name,
                   wchar_t *out, DWORD cch, REGSAM view);
BOOL Reg_SetDword(HKEY root, const wchar_t *key, const wchar_t *name,
                  DWORD value, REGSAM view);
BOOL Reg_DeleteTree(HKEY root, const wchar_t *key, REGSAM view);

#endif
