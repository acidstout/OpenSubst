/*
 * recycle.c - known-folder based Recycle Bin registration.
 *
 * Copyright (c) 2026 Rekow IT. MIT License, see LICENSE.
 */
#include "recycle.h"
#include "settings.h"

#define FOLDER_DESCRIPTIONS L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FolderDescriptions"
#define USER_SHELL_FOLDERS  L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders"
#define BITBUCKET_KF        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\BitBucket\\KnownFolder"

#define KF_CATEGORY_PERUSER_ 4

/* One fixed GUID per letter; the last byte is the drive letter. */
void Recycle_Guid(wchar_t letter, wchar_t *out, size_t cch)
{
    StringCchPrintfW(out, cch, L"{6F2B9E31-7C04-4D5A-B8E3-0A51C0D4E5%02X}", (unsigned)letter);
}

BOOL Recycle_IsMachineRegistered(wchar_t letter)
{
    wchar_t guid[48], key[256];
    HKEY k;
    Recycle_Guid(letter, guid, ARRAYSIZE(guid));
    StringCchPrintfW(key, ARRAYSIZE(key), FOLDER_DESCRIPTIONS L"\\%s", guid);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &k)
        != ERROR_SUCCESS)
        return FALSE;
    RegCloseKey(k);
    return TRUE;
}

static DWORD write_definition(const wchar_t *key, wchar_t letter, REGSAM view)
{
    HKEY k;
    wchar_t name[64], rel[4];
    DWORD cat = KF_CATEGORY_PERUSER_;
    LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL, 0, KEY_SET_VALUE | view,
                             NULL, &k, NULL);
    if (r != ERROR_SUCCESS) return (DWORD)r;
    StringCchPrintfW(name, ARRAYSIZE(name), L"%c_Drive_OpenSubst", letter);
    rel[0] = letter; rel[1] = L':'; rel[2] = L'\\'; rel[3] = 0;
    r = RegSetValueExW(k, L"Category", 0, REG_DWORD, (const BYTE *)&cat, sizeof(cat));
    if (r == ERROR_SUCCESS)
        r = RegSetValueExW(k, L"Name", 0, REG_SZ, (const BYTE *)name,
                           (DWORD)((lstrlenW(name) + 1) * sizeof(wchar_t)));
    if (r == ERROR_SUCCESS)
        r = RegSetValueExW(k, L"RelativePath", 0, REG_SZ, (const BYTE *)rel,
                           (DWORD)((lstrlenW(rel) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return (DWORD)r;
}

DWORD Recycle_RegisterMachine(wchar_t letter, BOOL on)
{
    wchar_t guid[48], key[256];
    DWORD r64, r32;

    if (!IS_DRIVE_LETTER(letter)) return ERROR_INVALID_PARAMETER;
    Recycle_Guid(letter, guid, ARRAYSIZE(guid));
    StringCchPrintfW(key, ARRAYSIZE(key), FOLDER_DESCRIPTIONS L"\\%s", guid);

    if (on) {
        r64 = write_definition(key, letter, KEY_WOW64_64KEY);
        r32 = write_definition(key, letter, KEY_WOW64_32KEY);
    } else {
        r64 = Reg_DeleteTree(HKEY_LOCAL_MACHINE, key, KEY_WOW64_64KEY) ? 0 : ERROR_ACCESS_DENIED;
        r32 = Reg_DeleteTree(HKEY_LOCAL_MACHINE, key, KEY_WOW64_32KEY) ? 0 : ERROR_ACCESS_DENIED;
    }
    /* The 32-bit view is a bonus; the native view is what matters. */
    (void)r32;
    return r64;
}

DWORD Recycle_RegisterUser(wchar_t letter, BOOL on)
{
    wchar_t guid[48], key[256], root[4];
    HKEY k;
    LONG r;

    if (!IS_DRIVE_LETTER(letter)) return ERROR_INVALID_PARAMETER;
    Recycle_Guid(letter, guid, ARRAYSIZE(guid));
    root[0] = letter; root[1] = L':'; root[2] = L'\\'; root[3] = 0;
    StringCchPrintfW(key, ARRAYSIZE(key), BITBUCKET_KF L"\\%s", guid);

    if (!on) {
        if (RegOpenKeyExW(HKEY_CURRENT_USER, USER_SHELL_FOLDERS, 0, KEY_SET_VALUE, &k)
            == ERROR_SUCCESS) {
            RegDeleteValueW(k, guid);
            RegCloseKey(k);
        }
        Reg_DeleteTree(HKEY_CURRENT_USER, key, 0);
        return 0;
    }

    r = RegCreateKeyExW(HKEY_CURRENT_USER, USER_SHELL_FOLDERS, 0, NULL, 0, KEY_SET_VALUE,
                        NULL, &k, NULL);
    if (r != ERROR_SUCCESS) return (DWORD)r;
    r = RegSetValueExW(k, guid, 0, REG_EXPAND_SZ, (const BYTE *)root, sizeof(root));
    RegCloseKey(k);
    if (r != ERROR_SUCCESS) return (DWORD)r;

    r = RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_SET_VALUE | KEY_QUERY_VALUE,
                        NULL, &k, NULL);
    if (r != ERROR_SUCCESS) return (DWORD)r;
    {
        DWORD type, cb = sizeof(DWORD), cap = 0, nuke = 0;
        /* Keep a capacity the user set in the Recycle Bin properties. */
        if (RegQueryValueExW(k, L"MaxCapacity", NULL, &type, (BYTE *)&cap, &cb) != ERROR_SUCCESS ||
            type != REG_DWORD || cap == 0) {
            ULARGE_INTEGER total;
            /* Windows' default: 5 % of the volume, in MB. */
            if (GetDiskFreeSpaceExW(root, NULL, &total, NULL))
                cap = (DWORD)(total.QuadPart / 20 / (1024 * 1024));
            if (cap < 1024) cap = 1024;
            RegSetValueExW(k, L"MaxCapacity", 0, REG_DWORD, (const BYTE *)&cap, sizeof(cap));
        }
        RegSetValueExW(k, L"NukeOnDelete", 0, REG_DWORD, (const BYTE *)&nuke, sizeof(nuke));
    }
    RegCloseKey(k);
    return 0;
}
