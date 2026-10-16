/*
 * settings.c - registry persistence.
 *
 * Copyright (c) 2026 Rekow IT. MIT License, see LICENSE.
 */
#include "settings.h"
#include "drives.h"

#define RUN_KEY   L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE APP_NAME

/* ============================================================
 *  Registry helpers
 * ============================================================ */

BOOL Reg_SetString(HKEY root, const wchar_t *key, const wchar_t *name,
                   const wchar_t *value, REGSAM view)
{
    HKEY k;
    LONG r = RegCreateKeyExW(root, key, 0, NULL, 0, KEY_SET_VALUE | view, NULL, &k, NULL);
    if (r != ERROR_SUCCESS) return FALSE;
    r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)value,
                       (DWORD)((lstrlenW(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

BOOL Reg_GetString(HKEY root, const wchar_t *key, const wchar_t *name,
                   wchar_t *out, DWORD cch, REGSAM view)
{
    HKEY k;
    DWORD type = 0, cb = cch * (DWORD)sizeof(wchar_t);
    LONG r;
    if (cch) out[0] = 0;
    if (RegOpenKeyExW(root, key, 0, KEY_QUERY_VALUE | view, &k) != ERROR_SUCCESS) return FALSE;
    r = RegQueryValueExW(k, name, NULL, &type, (BYTE *)out, &cb);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        if (cch) out[0] = 0;
        return FALSE;
    }
    out[cch - 1] = 0;
    if (cb / sizeof(wchar_t) < cch) out[cb / sizeof(wchar_t)] = 0;
    return TRUE;
}

BOOL Reg_SetDword(HKEY root, const wchar_t *key, const wchar_t *name,
                  DWORD value, REGSAM view)
{
    HKEY k;
    LONG r = RegCreateKeyExW(root, key, 0, NULL, 0, KEY_SET_VALUE | view, NULL, &k, NULL);
    if (r != ERROR_SUCCESS) return FALSE;
    r = RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static DWORD reg_get_dword(HKEY k, const wchar_t *name, DWORD def)
{
    DWORD v = def, cb = sizeof(v), type = 0;
    if (RegQueryValueExW(k, name, NULL, &type, (BYTE *)&v, &cb) != ERROR_SUCCESS ||
        type != REG_DWORD)
        return def;
    return v;
}

static void reg_get_str(HKEY k, const wchar_t *name, wchar_t *out, DWORD cch)
{
    DWORD cb = cch * (DWORD)sizeof(wchar_t), type = 0;
    out[0] = 0;
    if (RegQueryValueExW(k, name, NULL, &type, (BYTE *)out, &cb) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ)) {
        out[0] = 0;
        return;
    }
    out[cch - 1] = 0;
    if (cb / sizeof(wchar_t) < cch) out[cb / sizeof(wchar_t)] = 0;
}

static void reg_put_str(HKEY k, const wchar_t *name, const wchar_t *v)
{
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)v,
                   (DWORD)((lstrlenW(v) + 1) * sizeof(wchar_t)));
}

static void reg_put_dword(HKEY k, const wchar_t *name, DWORD v)
{
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
}

BOOL Reg_DeleteTree(HKEY root, const wchar_t *key, REGSAM view)
{
    LONG r;
    if (view) {
        /* RegDeleteTree has no view parameter: open the key in the requested
           view, empty it, then delete the (now empty) key itself. */
        HKEY k;
        r = RegOpenKeyExW(root, key, 0, KEY_ALL_ACCESS | view, &k);
        if (r == ERROR_FILE_NOT_FOUND) return TRUE;
        if (r != ERROR_SUCCESS) return FALSE;
        RegDeleteTreeW(k, NULL);
        RegCloseKey(k);
        r = RegDeleteKeyExW(root, key, view, 0);
    } else {
        r = RegDeleteTreeW(root, key);
    }
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}

/* ============================================================
 *  Application settings
 * ============================================================ */

void Settings_Load(void)
{
    HKEY k;

    ZeroMemory(&g_set, sizeof(g_set));
    g_set.flags = AF_GROUP;
    g_set.theme = THEME_AUTO;
    g_set.defaultDriveFlags = DF_ENABLED | DF_UAC;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, APP_REGKEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return;
    /* Drop bits of options that no longer exist (options panel, frame theme). */
    g_set.flags = reg_get_dword(k, L"Flags", g_set.flags) &
                  (AF_STARTUP | AF_BACKGROUND | AF_MANAGENET | AF_ONLYOURS | AF_ONTOP |
                   AF_TRAYICON | AF_GROUP | AF_WELCOMED);
    g_set.theme = reg_get_dword(k, L"Theme", g_set.theme);
    if (g_set.theme > THEME_DARK) g_set.theme = THEME_AUTO;   /* 3 was "Classic" */
    g_set.defaultDriveFlags = reg_get_dword(k, L"DriveFlags", g_set.defaultDriveFlags) &
                              (DF_ENABLED | DF_UAC | DF_RECYCLE | DF_AUTORESTORE);
    reg_get_str(k, L"Language", g_set.language, ARRAYSIZE(g_set.language));
    g_set.winRect.left   = (LONG)reg_get_dword(k, L"WinLeft", 0);
    g_set.winRect.top    = (LONG)reg_get_dword(k, L"WinTop", 0);
    g_set.winRect.right  = (LONG)reg_get_dword(k, L"WinRight", 0);
    g_set.winRect.bottom = (LONG)reg_get_dword(k, L"WinBottom", 0);
    RegCloseKey(k);
}

void Settings_Save(void)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, APP_REGKEY, 0, NULL, 0, KEY_SET_VALUE,
                        NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    reg_put_dword(k, L"Flags", g_set.flags);
    reg_put_dword(k, L"Theme", g_set.theme);
    reg_put_dword(k, L"DriveFlags", g_set.defaultDriveFlags);
    reg_put_str(k, L"Language", g_set.language);
    reg_put_dword(k, L"WinLeft", (DWORD)g_set.winRect.left);
    reg_put_dword(k, L"WinTop", (DWORD)g_set.winRect.top);
    reg_put_dword(k, L"WinRight", (DWORD)g_set.winRect.right);
    reg_put_dword(k, L"WinBottom", (DWORD)g_set.winRect.bottom);
    RegCloseKey(k);
}

/* ============================================================
 *  Stored drives
 * ============================================================ */

static void drive_key(wchar_t letter, wchar_t *out, size_t cch)
{
    StringCchPrintfW(out, cch, APP_REGKEY L"\\Drives\\%c", letter);
}

void Config_LoadAll(void)
{
    int i;
    for (i = 0; i < 26; i++) {
        wchar_t key[128];
        HKEY k;
        DriveConfig *c = &g_cfg[i];
        ZeroMemory(c, sizeof(*c));
        c->letter = (wchar_t)(L'A' + i);
        drive_key(c->letter, key, ARRAYSIZE(key));
        if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
            continue;
        reg_get_str(k, L"Path", c->path, ARRAYSIZE(c->path));
        reg_get_str(k, L"Label", c->label, ARRAYSIZE(c->label));
        reg_get_str(k, L"Icon", c->icon, ARRAYSIZE(c->icon));
        c->flags = reg_get_dword(k, L"Flags", DF_ENABLED);
        RegCloseKey(k);

        /* The registry is user-writable input. Accept only an absolute folder
           or UNC path, canonicalised exactly like typed input, and derive the
           network flag from it instead of trusting the stored one. */
        c->used = FALSE;
        if (c->path[0] && Path_IsAbsoluteSpec(c->path)) {
            wchar_t norm[PATHBUF];
            if (Path_Normalize(c->path, norm, ARRAYSIZE(norm)) &&
                !(ch_upper(norm[0]) == c->letter && norm[1] == L':')) {
                StringCchCopyW(c->path, ARRAYSIZE(c->path), norm);
                c->flags &= DF_ENABLED | DF_UAC | DF_RECYCLE | DF_AUTORESTORE;
                if (Path_IsUnc(norm)) c->flags |= DF_NETWORK;
                c->used = TRUE;
            }
        }
        if (!c->used) c->path[0] = c->label[0] = c->icon[0] = 0;
    }
}

void Config_Save(wchar_t letter)
{
    wchar_t key[128];
    HKEY k;
    DriveConfig *c;
    if (!IS_DRIVE_LETTER(letter)) return;
    c = &g_cfg[LETTER_INDEX(letter)];
    if (!c->used) { Config_Delete(letter); return; }
    drive_key(letter, key, ARRAYSIZE(key));
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)
        != ERROR_SUCCESS)
        return;
    reg_put_str(k, L"Path", c->path);
    reg_put_str(k, L"Label", c->label);
    reg_put_str(k, L"Icon", c->icon);
    reg_put_dword(k, L"Flags", c->flags);
    RegCloseKey(k);
}

void Config_Delete(wchar_t letter)
{
    wchar_t key[128];
    if (!IS_DRIVE_LETTER(letter)) return;
    drive_key(letter, key, ARRAYSIZE(key));
    RegDeleteTreeW(HKEY_CURRENT_USER, key);
    RegDeleteKeyW(HKEY_CURRENT_USER, key);
}

/* ============================================================
 *  Autostart
 * ============================================================ */

BOOL Startup_IsRegistered(void)
{
    wchar_t v[MAX_PATH + 32];
    if (!Reg_GetString(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, v, ARRAYSIZE(v), 0))
        return FALSE;
    return StrStrIW(v, g_exePath) != NULL;
}

void Startup_Register(BOOL on)
{
    if (on) {
        wchar_t v[MAX_PATH + 32];
        StringCchPrintfW(v, ARRAYSIZE(v), L"\"%s\" /startup", g_exePath);
        Reg_SetString(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, v, 0);
    } else {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegDeleteValueW(k, RUN_VALUE);
            RegCloseKey(k);
        }
    }
}
