/*
 * drives.c - virtual and network drive primitives.
 *
 * Virtual drives are DOS device definitions in the caller's logon session,
 * exactly what SUBST.EXE creates. They are defined with a raw NT target
 * ("\??\C:\Folder"), which also works for paths longer than MAX_PATH.
 *
 * Copyright (c) 2026 Rekow IT. MIT License, see LICENSE.
 */
#include "drives.h"
#include "settings.h"

#define EXPLORER_DRIVES L"Software\\Classes\\Applications\\Explorer.exe\\Drives"
#define MOUNTPOINTS2    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\MountPoints2"
#define PREV_LABEL      L"_OpenSubstPreviousLabel"

static void dev_name(wchar_t letter, wchar_t dev[3])
{
    dev[0] = letter;
    dev[1] = L':';
    dev[2] = 0;
}

static void root_name(wchar_t letter, wchar_t root[4])
{
    root[0] = letter;
    root[1] = L':';
    root[2] = L'\\';
    root[3] = 0;
}

/* ============================================================
 *  Paths and errors
 * ============================================================ */

BOOL Path_IsUnc(const wchar_t *p)
{
    if (!p) return FALSE;
    if (StrCmpNIW(p, L"\\\\?\\UNC\\", 8) == 0) return TRUE;
    if (p[0] == L'\\' && p[1] == L'\\' && p[2] != L'?' && p[2] != L'.' && p[2] != 0)
        return TRUE;
    return FALSE;
}

/* "X:\..." with a letter, or a UNC path. */
static BOOL is_absolute(const wchar_t *p)
{
    if (!p) return FALSE;
    if (IS_DRIVE_LETTER(ch_upper(p[0])) && p[1] == L':' && (p[2] == L'\\' || p[2] == 0)) return TRUE;
    return Path_IsUnc(p);
}

BOOL Path_IsAbsoluteSpec(const wchar_t *p)
{
    if (!p) return FALSE;
    while (*p == L' ' || *p == L'\t' || *p == L'"') p++;
    if (StrCmpNIW(p, L"\\\\?\\UNC\\", 8) == 0) return p[8] != 0 && p[8] != L'\\';
    if (StrCmpNIW(p, L"\\\\?\\", 4) == 0 || StrCmpNIW(p, L"\\??\\", 4) == 0) return is_absolute(p + 4);
    return is_absolute(p);
}

BOOL Path_Normalize(const wchar_t *in, wchar_t *out, size_t cch)
{
    wchar_t tmp[PATHBUF];
    const wchar_t *s = in;
    size_t len;
    DWORD n;
    HRESULT hr = S_OK;

    if (!in || !out || cch < 4) return FALSE;
    out[0] = 0;
    while (*s == L' ' || *s == L'\t' || *s == L'"') s++;
    if (FAILED(StringCchCopyW(tmp, ARRAYSIZE(tmp), s))) return FALSE;
    len = (size_t)lstrlenW(tmp);
    while (len && (tmp[len - 1] == L' ' || tmp[len - 1] == L'\t' || tmp[len - 1] == L'"'))
        tmp[--len] = 0;
    if (!len) return FALSE;

    /* Forward slashes are common in pasted paths. */
    for (n = 0; n < len; n++) if (tmp[n] == L'/') tmp[n] = L'\\';

    /* Device-namespace prefixes are only accepted in front of a drive letter
       or UNC path; "\\?\Volume{...}", "\\?\GLOBALROOT\..." and friends would
       otherwise turn into a path relative to the current directory. */
    if (StrCmpNIW(tmp, L"\\\\?\\UNC\\", 8) == 0) {
        hr = StringCchPrintfW(out, cch, L"\\\\%s", tmp + 8);
    } else if (StrCmpNIW(tmp, L"\\\\?\\", 4) == 0 || StrCmpNIW(tmp, L"\\??\\", 4) == 0) {
        if (!is_absolute(tmp + 4)) return FALSE;
        hr = StringCchCopyW(out, cch, tmp + 4);
        if (SUCCEEDED(hr) && !Path_IsUnc(out)) {
            /* Collapse "." and ".." the same way as for plain paths. */
            wchar_t full[PATHBUF];
            n = GetFullPathNameW(out, ARRAYSIZE(full), full, NULL);
            if (n == 0 || n >= ARRAYSIZE(full)) return FALSE;
            hr = StringCchCopyW(out, cch, full);
        }
    } else if (StrCmpNW(tmp, L"\\\\.\\", 4) == 0) {
        return FALSE;   /* \\.\ devices are never folders */
    } else if (Path_IsUnc(tmp)) {
        hr = StringCchCopyW(out, cch, tmp);
    } else {
        if (tmp[0] && tmp[1] == L':' && tmp[2] == 0) {   /* "D:" means the root */
            tmp[2] = L'\\';
            tmp[3] = 0;
        }
        n = GetFullPathNameW(tmp, (DWORD)cch, out, NULL);
        if (n == 0 || n >= cch) return FALSE;
    }
    if (FAILED(hr)) return FALSE;   /* truncated: never use a shortened path */

    /* Drive letter upper case, no trailing backslash except "C:\". */
    if (out[0] && out[1] == L':') out[0] = ch_upper(out[0]);
    len = (size_t)lstrlenW(out);
    while (len > 3 && out[len - 1] == L'\\') out[--len] = 0;
    if (Path_IsUnc(out)) {
        /* "\\server\share" needs at least two components. */
        const wchar_t *p = out + 2, *sl = wcschr(p, L'\\');
        if (!sl || sl == p || !sl[1]) return FALSE;
        return TRUE;
    }
    return out[0] && out[1] == L':' && out[2] == L'\\';
}

/* Path as passed to the file system: long paths get the "\\?\" prefix. */
static BOOL attr_path(const wchar_t *p, wchar_t *out, size_t cch)
{
    if (!p || !p[0]) return FALSE;
    if ((size_t)lstrlenW(p) >= MAX_PATH - 12) {
        if (Path_IsUnc(p)) return SUCCEEDED(StringCchPrintfW(out, cch, L"\\\\?\\UNC\\%s", p + 2));
        return SUCCEEDED(StringCchPrintfW(out, cch, L"\\\\?\\%s", p));
    }
    return SUCCEEDED(StringCchCopyW(out, cch, p));
}

static BOOL attr_is_dir(DWORD a)
{
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

BOOL Path_IsDirectory(const wchar_t *p)
{
    wchar_t lp[PATHBUF + 8];
    if (!attr_path(p, lp, ARRAYSIZE(lp))) return FALSE;
    return attr_is_dir(GetFileAttributesW(lp));
}

/* GetFileAttributes on an unreachable server or a sleeping disk can block for
   half a minute. The window thread must never wait that long, so the call runs
   on a pool thread; if it does not finish in time the path counts as missing
   and the pool thread finishes (and frees the job) on its own. */
typedef struct AttrJob {
    volatile LONG refs;
    HANDLE  done;
    DWORD   attr;
    wchar_t path[PATHBUF + 8];
} AttrJob;

static void attr_job_release(AttrJob *j)
{
    if (InterlockedDecrement(&j->refs) == 0) {
        if (j->done) CloseHandle(j->done);
        HeapFree(GetProcessHeap(), 0, j);
    }
}

static DWORD WINAPI attr_worker(LPVOID param)
{
    AttrJob *j = (AttrJob *)param;
    j->attr = GetFileAttributesW(j->path);
    SetEvent(j->done);
    attr_job_release(j);
    return 0;
}

BOOL Path_IsDirectoryTimeout(const wchar_t *p, DWORD ms)
{
    AttrJob *j;
    BOOL ok = FALSE;

    j = (AttrJob *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(AttrJob));
    if (!j) return FALSE;
    if (!attr_path(p, j->path, ARRAYSIZE(j->path))) {
        HeapFree(GetProcessHeap(), 0, j);
        return FALSE;
    }
    j->attr = INVALID_FILE_ATTRIBUTES;
    j->refs = 2;
    j->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!j->done || !QueueUserWorkItem(attr_worker, j, WT_EXECUTELONGFUNCTION)) {
        /* No worker available: fall back to the direct call. */
        DWORD a = GetFileAttributesW(j->path);
        if (j->done) CloseHandle(j->done);
        HeapFree(GetProcessHeap(), 0, j);
        return attr_is_dir(a);
    }
    if (WaitForSingleObject(j->done, ms) == WAIT_OBJECT_0) ok = attr_is_dir(j->attr);
    attr_job_release(j);
    return ok;
}

void Error_Text(DWORD err, wchar_t *out, size_t cch)
{
    DWORD n = 0;
    out[0] = 0;
    if (err >= 2100 && err <= 2999) {   /* NERR_* messages live in netmsg.dll */
        HMODULE m = LoadLibraryExW(L"netmsg.dll", NULL, LOAD_LIBRARY_AS_DATAFILE);
        if (m) {
            n = FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
                               m, err, 0, out, (DWORD)cch, NULL);
            FreeLibrary(m);
        }
    }
    if (!n)
        n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           NULL, err, 0, out, (DWORD)cch, NULL);
    if (!n) {
        StringCchPrintfW(out, cch, L"%u", err);
        return;
    }
    while (n && (out[n - 1] == L'\r' || out[n - 1] == L'\n' || out[n - 1] == L' '))
        out[--n] = 0;
}

/* ============================================================
 *  State
 * ============================================================ */

void Drive_Query(wchar_t letter, DriveState *st)
{
    wchar_t dev[3];
    wchar_t buf[PATHBUF + 16];
    DWORD n, cch;

    st->kind = DK_NONE;
    st->target[0] = 0;
    dev_name(letter, dev);

    n = QueryDosDeviceW(dev, buf, ARRAYSIZE(buf));
    if (!n && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        /* Defined, but with a target longer than anything OpenSubst creates:
           certainly not free, and not ours. */
        st->kind = DK_OTHER;
        return;
    }
    if (n) {
        buf[ARRAYSIZE(buf) - 1] = 0;
        if (StrCmpNW(buf, L"\\??\\", 4) == 0) {
            st->kind = DK_SUBST;
            if (StrCmpNIW(buf + 4, L"UNC\\", 4) == 0)
                StringCchPrintfW(st->target, ARRAYSIZE(st->target), L"\\\\%s", buf + 8);
            else
                StringCchCopyW(st->target, ARRAYSIZE(st->target), buf + 4);
            return;
        }
        cch = ARRAYSIZE(st->target);
        n = WNetGetConnectionW(dev, st->target, &cch);
        if (n == NO_ERROR || n == ERROR_MORE_DATA) {
            if (n == ERROR_MORE_DATA) st->target[0] = 0;
            st->target[ARRAYSIZE(st->target) - 1] = 0;
            st->kind = DK_NETWORK;
            return;
        }
        st->target[0] = 0;
        st->kind = DK_OTHER;
        return;
    }

    /* Not a DOS device: a remembered but disconnected network drive? */
    cch = ARRAYSIZE(st->target);
    n = WNetGetConnectionW(dev, st->target, &cch);
    st->target[ARRAYSIZE(st->target) - 1] = 0;
    if (n == ERROR_CONNECTION_UNAVAIL) {
        st->kind = DK_NETUNAVAIL;
    } else if (n == NO_ERROR) {
        st->kind = DK_NETWORK;
    } else if (n == ERROR_MORE_DATA) {
        st->target[0] = 0;
        st->kind = DK_NETUNAVAIL;
    } else {
        st->target[0] = 0;
    }
}

DWORD Drive_UsedMask(void)
{
    /* GetLogicalDrives misses remembered-but-disconnected network drives. */
    DWORD mask = GetLogicalDrives();
    int i;
    for (i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) {
            wchar_t dev[3], unc[8];
            DWORD cch = ARRAYSIZE(unc);
            DWORD r;
            dev_name((wchar_t)(L'A' + i), dev);
            r = WNetGetConnectionW(dev, unc, &cch);
            if (r == ERROR_CONNECTION_UNAVAIL || r == ERROR_MORE_DATA || r == NO_ERROR)
                mask |= 1u << i;
        }
    }
    return mask;
}

/* ============================================================
 *  Virtual drives
 * ============================================================ */

DWORD Drive_MountSubst(wchar_t letter, const wchar_t *path)
{
    wchar_t dev[3];
    wchar_t target[PATHBUF + 16];

    if (!IS_DRIVE_LETTER(letter) || !path || !path[0]) return ERROR_INVALID_PARAMETER;
    /* A drive pointing into itself would be unreachable. */
    if (ch_upper(path[0]) == letter && path[1] == L':')
        return ERROR_INVALID_PARAMETER;
    if (!Path_IsDirectory(path)) return ERROR_PATH_NOT_FOUND;

    dev_name(letter, dev);
    if (FAILED(Path_IsUnc(path)
                   ? StringCchPrintfW(target, ARRAYSIZE(target), L"\\??\\UNC\\%s", path + 2)
                   : StringCchPrintfW(target, ARRAYSIZE(target), L"\\??\\%s", path)))
        return ERROR_FILENAME_EXCED_RANGE;

    /* DefineDosDevice does not fail on a letter in use - it stacks the new
       definition on top and hides the old drive. Never do that: the caller
       has to remove an existing definition explicitly. */
    {
        wchar_t cur[8];
        if (QueryDosDeviceW(dev, cur, ARRAYSIZE(cur)) || GetLastError() != ERROR_FILE_NOT_FOUND)
            return ERROR_ALREADY_ASSIGNED;
    }

    if (!DefineDosDeviceW(DDD_RAW_TARGET_PATH, dev, target))
        return GetLastError();
    return 0;
}

DWORD Drive_UnmountSubst(wchar_t letter)
{
    wchar_t dev[3];
    wchar_t buf[PATHBUF + 16];
    dev_name(letter, dev);
    if (!QueryDosDeviceW(dev, buf, ARRAYSIZE(buf))) return GetLastError();
    buf[ARRAYSIZE(buf) - 1] = 0;
    if (StrCmpNW(buf, L"\\??\\", 4) != 0) return ERROR_INVALID_DRIVE;
    if (!DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_RAW_TARGET_PATH |
                          DDD_EXACT_MATCH_ON_REMOVE, dev, buf))
        return GetLastError();
    return 0;
}

/* ============================================================
 *  Network drives
 * ============================================================ */

static void wnet_error_text(DWORD err, wchar_t *out, size_t cch)
{
    if (!out || !cch) return;
    out[0] = 0;
    if (err == ERROR_EXTENDED_ERROR) {
        DWORD code = 0;
        wchar_t prov[128];
        if (WNetGetLastErrorW(&code, out, (DWORD)cch, prov, ARRAYSIZE(prov)) == NO_ERROR &&
            out[0])
            return;
    }
    Error_Text(err, out, cch);
}

DWORD Drive_MountNet(wchar_t letter, const wchar_t *unc, HWND owner, BOOL interactive,
                     wchar_t *errText, size_t cchErr)
{
    wchar_t dev[3];
    wchar_t remote[PATHBUF];
    NETRESOURCEW nr;
    DWORD r, fl = interactive ? CONNECT_INTERACTIVE : 0;

    if (errText && cchErr) errText[0] = 0;
    if (!IS_DRIVE_LETTER(letter) || !Path_IsUnc(unc)) return ERROR_BAD_NETPATH;
    dev_name(letter, dev);
    if (FAILED(StringCchCopyW(remote, ARRAYSIZE(remote), unc))) return ERROR_BAD_NETPATH;

    ZeroMemory(&nr, sizeof(nr));
    nr.dwType = RESOURCETYPE_DISK;
    nr.lpLocalName = dev;
    nr.lpRemoteName = remote;

    r = WNetAddConnection3W(owner, &nr, NULL, NULL, fl);
    if (r == ERROR_ALREADY_ASSIGNED || r == ERROR_DEVICE_ALREADY_REMEMBERED) {
        /* Only a disconnected mapping, or one to the very same share, may be
           replaced - never somebody's live connection to another share. */
        DriveState st;
        Drive_Query(letter, &st);
        if (st.kind == DK_NETUNAVAIL ||
            (st.kind == DK_NETWORK && lstrcmpiW(st.target, remote) == 0) ||
            (st.kind == DK_NONE && r == ERROR_DEVICE_ALREADY_REMEMBERED)) {
            WNetCancelConnection2W(dev, CONNECT_UPDATE_PROFILE, TRUE);
            r = WNetAddConnection3W(owner, &nr, NULL, NULL, fl);
        }
    }
    if (r != NO_ERROR) wnet_error_text(r, errText, cchErr);
    return r;
}

DWORD Drive_UnmountNet(wchar_t letter, BOOL force)
{
    wchar_t dev[3];
    dev_name(letter, dev);
    return WNetCancelConnection2W(dev, CONNECT_UPDATE_PROFILE, force);
}

/* ============================================================
 *  Explorer label and icon
 * ============================================================ */

static void mountpoint_key(const wchar_t *unc, wchar_t *out, size_t cch)
{
    wchar_t name[PATHBUF];
    wchar_t *p;
    StringCchCopyW(name, ARRAYSIZE(name), unc);
    for (p = name; *p; p++) if (*p == L'\\') *p = L'#';
    StringCchPrintfW(out, cch, MOUNTPOINTS2 L"\\%s", name);
}

static void set_default_value(const wchar_t *key, const wchar_t *value)
{
    if (value && value[0]) {
        Reg_SetString(HKEY_CURRENT_USER, key, NULL, value, 0);
    } else {
        Reg_DeleteTree(HKEY_CURRENT_USER, key, 0);
    }
}

void Drive_SetShellInfo(wchar_t letter, const wchar_t *label, const wchar_t *icon,
                        const wchar_t *unc)
{
    wchar_t key[PATHBUF + 128];

    StringCchPrintfW(key, ARRAYSIZE(key), EXPLORER_DRIVES L"\\%c\\DefaultLabel", letter);
    set_default_value(key, label);
    StringCchPrintfW(key, ARRAYSIZE(key), EXPLORER_DRIVES L"\\%c\\DefaultIcon", letter);
    set_default_value(key, icon);

    if (unc && unc[0] && label && label[0]) {
        /* The share label is shared by every drive mapped to the share; keep
           a label the user gave it in Explorer so it can be put back. */
        wchar_t cur[LABELBUF], prev[LABELBUF];
        mountpoint_key(unc, key, ARRAYSIZE(key));
        if (Reg_GetString(HKEY_CURRENT_USER, key, L"_LabelFromReg", cur, ARRAYSIZE(cur), 0) &&
            cur[0] && lstrcmpW(cur, label) != 0 &&
            !Reg_GetString(HKEY_CURRENT_USER, key, PREV_LABEL, prev, ARRAYSIZE(prev), 0))
            Reg_SetString(HKEY_CURRENT_USER, key, PREV_LABEL, cur, 0);
        Reg_SetString(HKEY_CURRENT_USER, key, L"_LabelFromReg", label, 0);
    }

    /* Drop the per-letter key again if it ended up empty. */
    StringCchPrintfW(key, ARRAYSIZE(key), EXPLORER_DRIVES L"\\%c", letter);
    if ((!label || !label[0]) && (!icon || !icon[0]))
        RegDeleteKeyW(HKEY_CURRENT_USER, key);
}

void Drive_ClearShellInfo(wchar_t letter, const wchar_t *unc, const wchar_t *ourLabel)
{
    wchar_t key[PATHBUF + 128];

    StringCchPrintfW(key, ARRAYSIZE(key), EXPLORER_DRIVES L"\\%c", letter);
    Reg_DeleteTree(HKEY_CURRENT_USER, key, 0);

    /* The share label belongs to the share, not to us: remove it only if it
       still holds the label OpenSubst wrote. */
    if (unc && unc[0] && ourLabel && ourLabel[0]) {
        wchar_t cur[LABELBUF];
        mountpoint_key(unc, key, ARRAYSIZE(key));
        if (Reg_GetString(HKEY_CURRENT_USER, key, L"_LabelFromReg", cur, ARRAYSIZE(cur), 0) &&
            lstrcmpW(cur, ourLabel) == 0) {
            wchar_t prev[LABELBUF];
            BOOL hadPrev = Reg_GetString(HKEY_CURRENT_USER, key, PREV_LABEL, prev, ARRAYSIZE(prev), 0);
            HKEY k;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
                if (hadPrev && prev[0])
                    RegSetValueExW(k, L"_LabelFromReg", 0, REG_SZ, (const BYTE *)prev,
                                   (DWORD)((lstrlenW(prev) + 1) * sizeof(wchar_t)));
                else
                    RegDeleteValueW(k, L"_LabelFromReg");
                RegDeleteValueW(k, PREV_LABEL);
                RegCloseKey(k);
            }
        }
    }
}

void Drive_NotifyShell(wchar_t letter, BOOL added)
{
    wchar_t root[4];
    root_name(letter, root);
    SHChangeNotify(added ? SHCNE_DRIVEADD : SHCNE_DRIVEREMOVED,
                   SHCNF_PATHW | SHCNF_FLUSHNOWAIT, root, NULL);
}

void Drive_NotifyShellInfo(wchar_t letter, BOOL iconChanged)
{
    wchar_t root[4];
    root_name(letter, root);
    SHChangeNotify(SHCNE_RENAMEFOLDER, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, root, root);
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, root, NULL);
    if (iconChanged)
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, NULL, NULL);
}
