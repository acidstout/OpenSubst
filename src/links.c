/*
 * links.c - folder links (junctions and directory symbolic links).
 *
 * Copyright (c) 2026 Rekow IT. MIT License, see LICENSE.
 */
#include "links.h"
#include "drives.h"
#include "engine.h"
#include "lang.h"

#include <winioctl.h>

LinkConfig g_links[MAX_LINKS];

#define LINKS_KEY APP_REGKEY L"\\Links"

#ifndef IO_REPARSE_TAG_MOUNT_POINT
#define IO_REPARSE_TAG_MOUNT_POINT  0xA0000003L
#endif
#ifndef IO_REPARSE_TAG_SYMLINK
#define IO_REPARSE_TAG_SYMLINK      0xA000000CL
#endif
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
#define RP_MAX          (16 * 1024)     /* MAXIMUM_REPARSE_DATA_BUFFER_SIZE */
#define SYMLINK_RELATIVE 0x1

/* REPARSE_DATA_BUFFER lives in the DDK headers only. */
typedef struct RpBuffer {
    DWORD ReparseTag;
    WORD  ReparseDataLength;
    WORD  Reserved;
    union {
        struct {
            WORD  SubstituteNameOffset;
            WORD  SubstituteNameLength;
            WORD  PrintNameOffset;
            WORD  PrintNameLength;
            ULONG Flags;
            WCHAR PathBuffer[1];
        } sym;
        struct {
            WORD  SubstituteNameOffset;
            WORD  SubstituteNameLength;
            WORD  PrintNameOffset;
            WORD  PrintNameLength;
            WCHAR PathBuffer[1];
        } mp;
    } u;
} RpBuffer;

#define RP_HEADER   8   /* tag + length + reserved */

/* ============================================================
 *  Paths
 * ============================================================ */

/* Path for the file system, with "\\?\" when it is long. */
static BOOL fs_path(const wchar_t *p, wchar_t *out, size_t cch)
{
    if ((size_t)lstrlenW(p) >= MAX_PATH - 12) {
        if (Path_IsUnc(p)) return SUCCEEDED(StringCchPrintfW(out, cch, L"\\\\?\\UNC\\%s", p + 2));
        return SUCCEEDED(StringCchPrintfW(out, cch, L"\\\\?\\%s", p));
    }
    return SUCCEEDED(StringCchCopyW(out, cch, p));
}

/* Replace a session-specific drive letter (virtual or network drive) by
   what it stands for. Drive letters exist per logon session; a link that
   points through one would lead nowhere for every other user. */
static BOOL resolve_letters(const wchar_t *in, wchar_t *out, size_t cch)
{
    wchar_t cur[PATHBUF], next[PATHBUF];
    int depth;

    if (!Path_Normalize(in, cur, ARRAYSIZE(cur))) return FALSE;
    for (depth = 0; depth < 8; depth++) {
        DriveState st;
        const wchar_t *rest;
        if (Path_IsUnc(cur)) break;
        Drive_Query(cur[0], &st);
        if ((st.kind != DK_SUBST && st.kind != DK_NETWORK && st.kind != DK_NETUNAVAIL) || !st.target[0])
            break;
        rest = cur + 2;                 /* "\..." or "" after "X:" */
        if (FAILED(StringCchPrintfW(next, ARRAYSIZE(next), L"%s%s%s", st.target,
                                    (rest[0] == L'\\' || rest[0] == 0) ? L"" : L"\\", rest)))
            return FALSE;
        if (!Path_Normalize(next, cur, ARRAYSIZE(cur))) return FALSE;
    }
    if (depth >= 8) return FALSE;       /* a loop of virtual drives */
    return SUCCEEDED(StringCchCopyW(out, cch, cur));
}

BOOL Link_ResolveTarget(const wchar_t *in, wchar_t *out, size_t cch)
{
    if (!in || !Path_IsAbsoluteSpec(in)) return FALSE;
    return resolve_letters(in, out, cch);
}

BOOL Link_ResolveLocation(const wchar_t *in, wchar_t *out, size_t cch)
{
    if (!in || !Path_IsAbsoluteSpec(in)) return FALSE;
    if (!resolve_letters(in, out, cch)) return FALSE;
    if (Path_IsUnc(out)) return FALSE;              /* links live on local volumes */
    if (lstrlenW(out) <= 3) return FALSE;           /* never a drive root */
    return TRUE;
}

BOOL Link_PathsOverlap(const wchar_t *a, const wchar_t *b)
{
    int la = lstrlenW(a), lb = lstrlenW(b);
    const wchar_t *shortp = la <= lb ? a : b, *longp = la <= lb ? b : a;
    int ls = la <= lb ? la : lb;
    if (ls == 0) return FALSE;
    if (CompareStringOrdinal(shortp, ls, longp, ls, TRUE) != CSTR_EQUAL) return FALSE;
    /* "C:\a" vs "C:\ab" is no overlap; "C:\a" vs "C:\a\b" is. */
    return longp[ls] == 0 || longp[ls] == L'\\' || shortp[ls - 1] == L'\\';
}

/* ============================================================
 *  Query
 * ============================================================ */

void Link_Query(const wchar_t *link, LinkState *st)
{
    wchar_t lp[PATHBUF + 8];
    DWORD a, got = 0;
    HANDLE h;
    RpBuffer *rp;

    st->kind = LK_UNKNOWN;
    st->target[0] = 0;
    if (!fs_path(link, lp, ARRAYSIZE(lp))) return;

    a = GetFileAttributesW(lp);
    if (a == INVALID_FILE_ATTRIBUTES) {
        DWORD e = GetLastError();
        st->kind = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? LK_NONE : LK_UNKNOWN;
        return;
    }
    if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        st->kind = LK_PLAIN;
        return;
    }

    h = CreateFileW(lp, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    rp = (RpBuffer *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, RP_MAX);
    if (rp && DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, rp, RP_MAX, &got, NULL) &&
        got >= RP_HEADER + 8) {
        const WCHAR *buf = NULL;
        WORD off = 0, len = 0;
        BOOL relative = FALSE;
        if (rp->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) {
            st->kind = LK_JUNCTION;
            buf = rp->u.mp.PathBuffer;
            off = rp->u.mp.SubstituteNameOffset;
            len = rp->u.mp.SubstituteNameLength;
        } else if (rp->ReparseTag == IO_REPARSE_TAG_SYMLINK && got >= RP_HEADER + 12) {
            st->kind = LK_SYMLINK;
            buf = rp->u.sym.PathBuffer;
            off = rp->u.sym.SubstituteNameOffset;
            len = rp->u.sym.SubstituteNameLength;
            relative = (rp->u.sym.Flags & SYMLINK_RELATIVE) != 0;
        } else {
            st->kind = LK_PLAIN;    /* some other reparse point (OneDrive, dedup, ...) */
        }
        /* Bounds: the name must lie completely inside what was returned. */
        if (buf && len && (size_t)((const BYTE *)buf - (const BYTE *)rp) + off + len <= got &&
            !(off & 1) && !(len & 1)) {
            wchar_t name[PATHBUF];
            size_t n = len / sizeof(WCHAR);
            if (n < ARRAYSIZE(name)) {
                CopyMemory(name, (const BYTE *)buf + off, len);
                name[n] = 0;
                if (!relative && StrCmpNIW(name, L"\\??\\UNC\\", 8) == 0)
                    StringCchPrintfW(st->target, ARRAYSIZE(st->target), L"\\\\%s", name + 8);
                else if (!relative && StrCmpNIW(name, L"\\??\\", 4) == 0)
                    StringCchCopyW(st->target, ARRAYSIZE(st->target), name + 4);
                else
                    StringCchCopyW(st->target, ARRAYSIZE(st->target), name);
                /* Same canonical form as stored targets. */
                if (!relative) {
                    wchar_t norm[PATHBUF];
                    if (Path_Normalize(st->target, norm, ARRAYSIZE(norm)))
                        StringCchCopyW(st->target, ARRAYSIZE(st->target), norm);
                }
            }
        }
    }
    if (rp) HeapFree(GetProcessHeap(), 0, rp);
    CloseHandle(h);
}

/* ============================================================
 *  Create / remove
 * ============================================================ */

static DWORD create_junction(const wchar_t *lp, const wchar_t *target)
{
    wchar_t sub[PATHBUF + 8];
    RpBuffer *rp;
    size_t subBytes, printBytes, dataLen;
    HANDLE h;
    DWORD r = 0, ret = 0;

    if (FAILED(StringCchPrintfW(sub, ARRAYSIZE(sub), L"\\??\\%s", target))) return ERROR_FILENAME_EXCED_RANGE;
    subBytes = (size_t)lstrlenW(sub) * sizeof(WCHAR);
    printBytes = (size_t)lstrlenW(target) * sizeof(WCHAR);
    dataLen = 8 + subBytes + sizeof(WCHAR) + printBytes + sizeof(WCHAR);
    if (RP_HEADER + dataLen > RP_MAX) return ERROR_FILENAME_EXCED_RANGE;

    rp = (RpBuffer *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, RP_MAX);
    if (!rp) return ERROR_OUTOFMEMORY;
    rp->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    rp->ReparseDataLength = (WORD)dataLen;
    rp->u.mp.SubstituteNameOffset = 0;
    rp->u.mp.SubstituteNameLength = (WORD)subBytes;
    rp->u.mp.PrintNameOffset = (WORD)(subBytes + sizeof(WCHAR));
    rp->u.mp.PrintNameLength = (WORD)printBytes;
    CopyMemory(rp->u.mp.PathBuffer, sub, subBytes);
    CopyMemory((BYTE *)rp->u.mp.PathBuffer + subBytes + sizeof(WCHAR), target, printBytes);

    /* CreateDirectory fails if anything exists there: never touches an
       existing folder. */
    if (!CreateDirectoryW(lp, NULL)) {
        r = GetLastError();
        HeapFree(GetProcessHeap(), 0, rp);
        return r;
    }
    h = CreateFileW(lp, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        r = GetLastError();
    } else {
        if (!DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, rp, (DWORD)(RP_HEADER + dataLen),
                             NULL, 0, &ret, NULL))
            r = GetLastError();
        CloseHandle(h);
    }
    if (r) RemoveDirectoryW(lp);    /* the empty folder we just created */
    HeapFree(GetProcessHeap(), 0, rp);
    return r;
}

DWORD Link_Create(const wchar_t *link, const wchar_t *target)
{
    wchar_t lp[PATHBUF + 8], parent[PATHBUF];
    LinkState st;

    if (!link || !target || !link[0] || !target[0]) return ERROR_INVALID_PARAMETER;
    if (Link_PathsOverlap(link, target)) return ERROR_INVALID_PARAMETER;
    Link_Query(link, &st);
    if (st.kind != LK_NONE) return ERROR_ALREADY_EXISTS;

    StringCchCopyW(parent, ARRAYSIZE(parent), link);
    PathRemoveFileSpecW(parent);
    if (!Path_IsDirectory(parent)) return ERROR_PATH_NOT_FOUND;
    if (!fs_path(link, lp, ARRAYSIZE(lp))) return ERROR_FILENAME_EXCED_RANGE;

    if (!Path_IsUnc(target)) return create_junction(lp, target);

    /* Network target: a directory symbolic link. Without Developer Mode or
       the privilege this fails with ERROR_PRIVILEGE_NOT_HELD - by design no
       elevation is attempted. */
    if (CreateSymbolicLinkW(lp, target, SYMBOLIC_LINK_FLAG_DIRECTORY |
                                        SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
        return 0;
    {
        DWORD e = GetLastError();
        /* Windows before 10 1703 rejects the unknown flag. */
        if (e == ERROR_INVALID_PARAMETER &&
            CreateSymbolicLinkW(lp, target, SYMBOLIC_LINK_FLAG_DIRECTORY))
            return 0;
        if (e == ERROR_INVALID_PARAMETER) e = GetLastError();
        return e;
    }
}

DWORD Link_Remove(const wchar_t *link)
{
    wchar_t lp[PATHBUF + 8];
    LinkState st;

    Link_Query(link, &st);
    if (st.kind == LK_NONE) return 0;
    /* Only the link object - RemoveDirectory on a junction or directory
       symlink deletes the link, never anything in the target. A real folder
       is refused outright. */
    if (st.kind != LK_JUNCTION && st.kind != LK_SYMLINK) return ERROR_DIRECTORY;
    if (!fs_path(link, lp, ARRAYSIZE(lp))) return ERROR_FILENAME_EXCED_RANGE;
    if (!RemoveDirectoryW(lp)) return GetLastError();
    return 0;
}

/* ============================================================
 *  Storage
 * ============================================================ */

static void slot_key(int i, wchar_t *out, size_t cch)
{
    StringCchPrintfW(out, cch, LINKS_KEY L"\\%02d", i);
}

static void reg_read_str(HKEY k, const wchar_t *name, wchar_t *out, DWORD cch)
{
    DWORD cb = cch * (DWORD)sizeof(wchar_t), type = 0;
    out[0] = 0;
    if (RegQueryValueExW(k, name, NULL, &type, (BYTE *)out, &cb) != ERROR_SUCCESS || type != REG_SZ) {
        out[0] = 0;
        return;
    }
    out[cch - 1] = 0;
    if (cb / sizeof(wchar_t) < cch) out[cb / sizeof(wchar_t)] = 0;
}

void Links_LoadAll(void)
{
    int i;
    for (i = 0; i < MAX_LINKS; i++) {
        LinkConfig *c = &g_links[i];
        wchar_t key[96], raw[PATHBUF];
        HKEY k;
        DWORD fl = DF_ENABLED, cb = sizeof(fl), type = 0;

        ZeroMemory(c, sizeof(*c));
        slot_key(i, key, ARRAYSIZE(key));
        if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) continue;
        reg_read_str(k, L"Link", raw, ARRAYSIZE(raw));
        /* User-writable input: validate exactly like typed input. */
        if (!Link_ResolveLocation(raw, c->link, ARRAYSIZE(c->link))) c->link[0] = 0;
        reg_read_str(k, L"Target", raw, ARRAYSIZE(raw));
        if (!Link_ResolveTarget(raw, c->target, ARRAYSIZE(c->target))) c->target[0] = 0;
        if (RegQueryValueExW(k, L"Flags", NULL, &type, (BYTE *)&fl, &cb) != ERROR_SUCCESS || type != REG_DWORD)
            fl = DF_ENABLED;
        RegCloseKey(k);
        c->flags = fl & DF_ENABLED;
        c->used = c->link[0] && c->target[0] && !Link_PathsOverlap(c->link, c->target);
        if (!c->used) ZeroMemory(c, sizeof(*c));
    }
}

void Links_Save(int i)
{
    wchar_t key[96];
    HKEY k;
    const LinkConfig *c;
    if (i < 0 || i >= MAX_LINKS) return;
    c = &g_links[i];
    if (!c->used) { Links_Delete(i); return; }
    slot_key(i, key, ARRAYSIZE(key));
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExW(k, L"Link", 0, REG_SZ, (const BYTE *)c->link,
                   (DWORD)((lstrlenW(c->link) + 1) * sizeof(wchar_t)));
    RegSetValueExW(k, L"Target", 0, REG_SZ, (const BYTE *)c->target,
                   (DWORD)((lstrlenW(c->target) + 1) * sizeof(wchar_t)));
    RegSetValueExW(k, L"Flags", 0, REG_DWORD, (const BYTE *)&c->flags, sizeof(c->flags));
    RegCloseKey(k);
}

void Links_Delete(int i)
{
    wchar_t key[96];
    if (i < 0 || i >= MAX_LINKS) return;
    slot_key(i, key, ARRAYSIZE(key));
    RegDeleteTreeW(HKEY_CURRENT_USER, key);
    RegDeleteKeyW(HKEY_CURRENT_USER, key);
}

int Links_Find(const wchar_t *link)
{
    int i;
    for (i = 0; i < MAX_LINKS; i++)
        if (g_links[i].used && lstrcmpiW(g_links[i].link, link) == 0) return i;
    return -1;
}

int Links_FreeSlot(void)
{
    int i;
    for (i = 0; i < MAX_LINKS; i++) if (!g_links[i].used) return i;
    return -1;
}

/* ============================================================
 *  Apply
 * ============================================================ */

BOOL Link_VolumeSupported(const wchar_t *path, wchar_t *desc, size_t cch)
{
    wchar_t root[PATHBUF], fs[MAX_PATH + 1];
    DWORD fl = 0;
    if (!GetVolumePathNameW(path, root, ARRAYSIZE(root))) return TRUE;
    StringCchCopyW(desc, cch, root);
    if (!GetVolumeInformationW(root, NULL, 0, NULL, NULL, &fl, fs, ARRAYSIZE(fs))) return TRUE;
    if (fs[0]) StringCchPrintfW(desc, cch, L"%s (%s)", root, fs);
    return (fl & FILE_SUPPORTS_REPARSE_POINTS) != 0;
}

static void report(HWND owner, StrId fmt, const wchar_t *path, DWORD err)
{
    wchar_t head[PATHBUF + 256], msg[PATHBUF + 1024], etext[512], line[640], parent[PATHBUF], vol[PATHBUF];
    StringCchPrintfW(head, ARRAYSIZE(head), T(fmt), path);
    StringCchCopyW(parent, ARRAYSIZE(parent), path);
    PathRemoveFileSpecW(parent);
    if (err == ERROR_INVALID_FUNCTION && !Link_VolumeSupported(parent, vol, ARRAYSIZE(vol))) {
        StringCchPrintfW(line, ARRAYSIZE(line), T(S_E_LK_NOREPARSE), vol);
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"%s\r\n\r\n%s", head, line);
    } else if (err == ERROR_PRIVILEGE_NOT_HELD) {
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"%s\r\n\r\n%s", head, T(S_E_LK_SYMPRIV));
    } else {
        Error_Text(err, etext, ARRAYSIZE(etext));
        StringCchPrintfW(line, ARRAYSIZE(line), T(S_E_ERRCODE), err, etext);
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"%s\r\n\r\n%s", head, line);
    }
    App_Message(owner, msg, TRUE);
}

DWORD Links_Apply(int i, HWND owner, BOOL interactive)
{
    LinkConfig *c;
    LinkState st;
    DWORD r;

    if (i < 0 || i >= MAX_LINKS || !g_links[i].used) return ERROR_INVALID_PARAMETER;
    c = &g_links[i];
    Link_Query(c->link, &st);
    if ((st.kind == LK_JUNCTION || st.kind == LK_SYMLINK) && lstrcmpiW(st.target, c->target) == 0)
        return 0;   /* already there */
    if (st.kind != LK_NONE) {
        /* Something else is at the link path: never replace it. */
        if (interactive) {
            wchar_t msg[PATHBUF + 512];
            StringCchPrintfW(msg, ARRAYSIZE(msg), T(S_E_LK_EXISTS), c->link);
            App_Message(owner, msg, TRUE);
        }
        return ERROR_ALREADY_EXISTS;
    }
    r = Link_Create(c->link, c->target);
    if (r && interactive) report(owner, S_E_LK_CREATE, c->link, r);
    return r;
}

DWORD Links_Unapply(int i, HWND owner, BOOL interactive)
{
    LinkConfig *c;
    LinkState st;
    DWORD r;

    if (i < 0 || i >= MAX_LINKS || !g_links[i].used) return ERROR_INVALID_PARAMETER;
    c = &g_links[i];
    Link_Query(c->link, &st);
    if (st.kind == LK_NONE) return 0;
    /* Only our own link: a link somebody pointed elsewhere, or a real
       folder, stays untouched. */
    if ((st.kind != LK_JUNCTION && st.kind != LK_SYMLINK) || lstrcmpiW(st.target, c->target) != 0)
        return 0;
    r = Link_Remove(c->link);
    if (r && interactive) report(owner, S_E_LK_REMOVE, c->link, r);
    return r;
}

void Links_RestoreAll(void)
{
    int i;
    for (i = 0; i < MAX_LINKS; i++)
        if (g_links[i].used && (g_links[i].flags & DF_ENABLED)) Links_Apply(i, NULL, FALSE);
}

static int input_error(HWND owner, StrId fmt, const wchar_t *arg, int field)
{
    wchar_t msg[PATHBUF + 512];
    if (arg) StringCchPrintfW(msg, ARRAYSIZE(msg), T(fmt), arg);
    else StringCchCopyW(msg, ARRAYSIZE(msg), T(fmt));
    App_Message(owner, msg, TRUE);
    return field;
}

int Links_CheckInput(int self, const wchar_t *link, const wchar_t *target,
                     LinkConfig *out, HWND owner)
{
    wchar_t parent[PATHBUF];
    LinkState st;
    int dup;

    ZeroMemory(out, sizeof(*out));
    if (self < 0 && Links_FreeSlot() < 0) return input_error(owner, S_E_LK_FULL, NULL, 1);
    if (!Link_ResolveLocation(link, out->link, ARRAYSIZE(out->link)))
        return input_error(owner, S_E_LK_PATH, NULL, 1);
    if (!Link_ResolveTarget(target, out->target, ARRAYSIZE(out->target))) {
        wchar_t msg[PATHBUF + 64];
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"%s%s", T(S_E_INVALIDPATH), target);
        App_Message(owner, msg, TRUE);
        return 2;
    }
    if (!Path_IsDirectoryTimeout(out->target, DIR_TIMEOUT_USER))
        return input_error(owner, S_E_LK_TARGET, out->target, 2);
    if (Link_PathsOverlap(out->link, out->target)) return input_error(owner, S_E_LK_NESTED, NULL, 1);
    dup = Links_Find(out->link);
    if (dup >= 0 && dup != self) return input_error(owner, S_E_LK_DUP, out->link, 1);

    StringCchCopyW(parent, ARRAYSIZE(parent), out->link);
    PathRemoveFileSpecW(parent);
    if (!Path_IsDirectoryTimeout(parent, DIR_TIMEOUT_USER))
        return input_error(owner, S_E_LK_NOPARENT, parent, 1);
    {
        wchar_t vol[PATHBUF];
        if (!Link_VolumeSupported(parent, vol, ARRAYSIZE(vol)))
            return input_error(owner, S_E_LK_NOREPARSE, vol, 1);
    }

    /* The path must be free - or already be a link to this target, or the
       edited link itself (it is replaced). */
    Link_Query(out->link, &st);
    if (st.kind != LK_NONE) {
        BOOL isLink = st.kind == LK_JUNCTION || st.kind == LK_SYMLINK;
        BOOL same = isLink && lstrcmpiW(st.target, out->target) == 0;
        BOOL own = isLink && self >= 0 && self < MAX_LINKS && g_links[self].used &&
                   lstrcmpiW(g_links[self].link, out->link) == 0 &&
                   lstrcmpiW(g_links[self].target, st.target) == 0;
        if (!same && !own) return input_error(owner, S_E_LK_EXISTS, out->link, 1);
    }
    out->used = TRUE;
    out->flags = DF_ENABLED;
    return 0;
}

DWORD Links_Set(int i, const LinkConfig *nc, HWND owner, BOOL interactive, BOOL create)
{
    LinkConfig old;
    DWORD r;

    if (i < 0 || i >= MAX_LINKS || !nc || !nc->link[0] || !nc->target[0]) return ERROR_INVALID_PARAMETER;
    old = g_links[i];
    if (create && old.used &&
        (lstrcmpiW(old.link, nc->link) != 0 || lstrcmpiW(old.target, nc->target) != 0)) {
        r = Links_Unapply(i, owner, interactive);
        if (r) return r;
    }
    g_links[i] = *nc;
    g_links[i].used = TRUE;
    g_links[i].flags = DF_ENABLED;
    if (!create) return 0;
    r = Links_Apply(i, owner, interactive);
    if (r) {
        g_links[i] = old;
        /* Put the edited link back where it was. */
        if (old.used && (old.flags & DF_ENABLED)) Links_Apply(i, NULL, FALSE);
    }
    return r;
}
