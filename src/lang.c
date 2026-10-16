/*
 * lang.c - built-in strings and the .lng language file loader.
 *
 * Copyright (c) 2026 Rekow IT. MIT License, see LICENSE.
 */
#include "lang.h"

static const wchar_t *const s_keys[S__COUNT] = {
#define X(k, en, de) L ## #k,
#include "strings.h"
#undef X
};

static const wchar_t *const s_en[S__COUNT] = {
#define X(k, en, de) en,
#include "strings.h"
#undef X
};

static const wchar_t *const s_de[S__COUNT] = {
#define X(k, en, de) de,
#include "strings.h"
#undef X
};

static BOOL same_format(const wchar_t *base, const wchar_t *tr);

static const wchar_t *const *s_base = s_en;
static const wchar_t *s_over[S__COUNT];
static wchar_t *s_overBuf;              /* one block holding all overrides */

const wchar_t *T(StrId id)
{
    if ((unsigned)id >= (unsigned)S__COUNT) return L"";
    return s_over[id] ? s_over[id] : s_base[id];
}

static void lang_dir(wchar_t *out, size_t cch)
{
    StringCchCopyW(out, cch, g_exeDir);
    PathAppendW(out, L"lang");
}

/* Read a text file (UTF-8 with or without BOM, or UTF-16 LE with BOM) into a
   freshly allocated, NUL-terminated wide string. */
static wchar_t *read_text_file(const wchar_t *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER size;
    BYTE *raw;
    DWORD got = 0;
    wchar_t *text = NULL;

    if (f == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(f, &size) || size.QuadPart > 4 * 1024 * 1024) {
        CloseHandle(f);
        return NULL;
    }
    raw = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size.QuadPart + 2);
    if (!raw) { CloseHandle(f); return NULL; }
    if (!ReadFile(f, raw, (DWORD)size.QuadPart, &got, NULL)) got = 0;
    CloseHandle(f);
    raw[got] = 0;
    raw[got + 1] = 0;

    if (got >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        DWORD n = (got - 2) / 2;
        text = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, ((SIZE_T)n + 1) * sizeof(wchar_t));
        if (text) {
            CopyMemory(text, raw + 2, (SIZE_T)n * sizeof(wchar_t));
            text[n] = 0;
        }
    } else {
        const char *src = (const char *)raw;
        int len = (int)got, n;
        if (len >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
            src += 3;
            len -= 3;
        }
        n = MultiByteToWideChar(CP_UTF8, 0, src, len, NULL, 0);
        text = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, ((SIZE_T)n + 1) * sizeof(wchar_t));
        if (text) {
            MultiByteToWideChar(CP_UTF8, 0, src, len, text, n);
            text[n] = 0;
        }
    }
    HeapFree(GetProcessHeap(), 0, raw);
    return text;
}

static void trim(wchar_t **s, wchar_t **e)
{
    while (*s < *e && (**s == L' ' || **s == L'\t')) (*s)++;
    while (*e > *s && ((*e)[-1] == L' ' || (*e)[-1] == L'\t' || (*e)[-1] == L'\r')) (*e)--;
}

/* Unescape \n \r \t \\ in place; returns the new end. */
static wchar_t *unescape(wchar_t *s, wchar_t *e)
{
    wchar_t *w = s;
    while (s < e) {
        if (*s == L'\\' && s + 1 < e) {
            s++;
            switch (*s) {
            case L'n': *w++ = L'\n'; break;
            case L'r': *w++ = L'\r'; break;
            case L't': *w++ = L'\t'; break;
            default:   *w++ = *s;    break;
            }
            s++;
        } else {
            *w++ = *s++;
        }
    }
    return w;
}

/* Parse "KEY=Value" lines. If 'nameOnly' is set, only the Language= value is
   extracted into 'name'. */
static BOOL parse_lng(wchar_t *text, BOOL nameOnly, wchar_t *name, size_t cchName,
                      const wchar_t **base)
{
    wchar_t *p = text;
    BOOL any = FALSE;

    while (*p) {
        wchar_t *line = p, *eol = p, *eq, *ks, *ke, *vs, *ve;
        while (*eol && *eol != L'\n') eol++;
        p = *eol ? eol + 1 : eol;

        ks = line;
        ke = eol;
        trim(&ks, &ke);
        if (ks >= ke || *ks == L';' || *ks == L'#' || *ks == L'[') continue;

        for (eq = ks; eq < ke && *eq != L'='; eq++) { }
        if (eq >= ke) continue;
        vs = eq + 1;
        ve = ke;
        ke = eq;
        trim(&ks, &ke);
        trim(&vs, &ve);
        *ke = 0;

        if (lstrcmpiW(ks, L"Language") == 0) {
            *ve = 0;
            if (name) StringCchCopyW(name, cchName, vs);
            continue;
        }
        if (lstrcmpiW(ks, L"Base") == 0) {
            if (base) {
                *ve = 0;
                *base = (lstrcmpiW(vs, L"de") == 0) ? L"de" : L"en";
            }
            continue;
        }
        if (nameOnly) continue;

        ve = unescape(vs, ve);
        *ve = 0;
        {
            int i;
            for (i = 0; i < S__COUNT; i++) {
                if (lstrcmpiW(ks, s_keys[i]) == 0) {
                    if (*vs && same_format(s_en[i], vs)) { s_over[i] = vs; any = TRUE; }
                    break;
                }
            }
        }
    }
    return any;
}

/* Conversion characters of all printf specifiers in 's', e.g. "cs" for
   "Drive %c: for %s". "%%" is not a specifier; '*' counts (it eats an
   argument). */
static void fmt_signature(const wchar_t *s, wchar_t *sig, size_t cch)
{
    size_t n = 0;
    for (; *s; s++) {
        if (*s != L'%') continue;
        s++;
        if (*s == L'%') continue;
        while (*s && wcschr(L"-+ #0123456789.*hlLwIzjt", *s)) {
            if (*s == L'*' && n + 1 < cch) sig[n++] = L'*';
            s++;
        }
        if (n + 1 < cch) sig[n++] = *s ? *s : L'!';
        if (!*s) break;
    }
    sig[n] = 0;
}

/* A translation must use exactly the same format specifiers as the built-in
   text; anything else would read the wrong arguments and could crash. */
static BOOL same_format(const wchar_t *base, const wchar_t *tr)
{
    wchar_t a[32], b[32];
    fmt_signature(base, a, ARRAYSIZE(a));
    fmt_signature(tr, b, ARRAYSIZE(b));
    return lstrcmpW(a, b) == 0;
}

static BOOL system_is_german(void)
{
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_GERMAN;
}

BOOL Lang_Select(const wchar_t *setting)
{
    wchar_t path[MAX_PATH];
    wchar_t *text;
    const wchar_t *base = NULL;

    ZeroMemory((void *)s_over, sizeof(s_over));
    if (s_overBuf) { HeapFree(GetProcessHeap(), 0, s_overBuf); s_overBuf = NULL; }

    if (!setting || !*setting) {
        s_base = system_is_german() ? s_de : s_en;
        return TRUE;
    }
    if (lstrcmpiW(setting, L"en") == 0) { s_base = s_en; return TRUE; }
    if (lstrcmpiW(setting, L"de") == 0) { s_base = s_de; return TRUE; }

    s_base = s_en;
    /* Only plain file names inside the lang folder. */
    if (wcschr(setting, L'\\') || wcschr(setting, L'/') || wcschr(setting, L':'))
        return FALSE;
    lang_dir(path, ARRAYSIZE(path));
    if (!PathAppendW(path, setting)) return FALSE;
    text = read_text_file(path);
    if (!text) return FALSE;
    if (!parse_lng(text, FALSE, NULL, 0, &base)) {
        HeapFree(GetProcessHeap(), 0, text);
        ZeroMemory((void *)s_over, sizeof(s_over));
        return FALSE;
    }
    if (base && lstrcmpiW(base, L"de") == 0) s_base = s_de;
    s_overBuf = text;   /* s_over points into it */
    return TRUE;
}

int Lang_ListFiles(LangFile *out, int max)
{
    wchar_t pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;

    lang_dir(pattern, ARRAYSIZE(pattern));
    PathAppendW(pattern, L"*.lng");
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        wchar_t path[MAX_PATH];
        wchar_t *text;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (n >= max) break;
        lang_dir(path, ARRAYSIZE(path));
        PathAppendW(path, fd.cFileName);
        StringCchCopyW(out[n].file, ARRAYSIZE(out[n].file), fd.cFileName);
        StringCchCopyW(out[n].name, ARRAYSIZE(out[n].name), fd.cFileName);
        PathRemoveExtensionW(out[n].name);
        text = read_text_file(path);
        if (text) {
            parse_lng(text, TRUE, out[n].name, ARRAYSIZE(out[n].name), NULL);
            HeapFree(GetProcessHeap(), 0, text);
        }
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static BOOL write_utf8(HANDLE f, const wchar_t *s)
{
    char buf[4096];
    DWORD put;
    int len = lstrlenW(s), pos = 0;
    while (pos < len) {
        int chunk = len - pos > 1000 ? 1000 : len - pos;
        int n = WideCharToMultiByte(CP_UTF8, 0, s + pos, chunk, buf, sizeof(buf), NULL, NULL);
        if (n <= 0 || !WriteFile(f, buf, (DWORD)n, &put, NULL)) return FALSE;
        pos += chunk;
    }
    return TRUE;
}

BOOL Lang_ExportTemplate(const wchar_t *path)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    BOOL ok = TRUE;
    int i;

    if (f == INVALID_HANDLE_VALUE) return FALSE;
    ok = write_utf8(f, L"\xFEFF; OpenSubst language file (UTF-8).\r\n"
                       L"; Copy to <OpenSubst folder>\\lang\\<Name>.lng and translate the text after '='.\r\n"
                       L"; Escapes: \\n = new line, \\r = carriage return, \\t = tab, \\\\ = backslash.\r\n"
                       L"; Missing keys fall back to the Base language (en or de).\r\n"
                       L"Language=English\r\nBase=en\r\n\r\n");
    for (i = 0; ok && i < S__COUNT; i++) {
        const wchar_t *s = s_en[i];
        wchar_t line[8192];
        size_t w = 0;
        StringCchPrintfW(line, ARRAYSIZE(line), L"%s=", s_keys[i]);
        w = (size_t)lstrlenW(line);
        for (; *s && w + 3 < ARRAYSIZE(line); s++) {
            if (*s == L'\r') { line[w++] = L'\\'; line[w++] = L'r'; }
            else if (*s == L'\n') { line[w++] = L'\\'; line[w++] = L'n'; }
            else if (*s == L'\t') { line[w++] = L'\\'; line[w++] = L't'; }
            else if (*s == L'\\') { line[w++] = L'\\'; line[w++] = L'\\'; }
            else line[w++] = *s;
        }
        line[w] = 0;
        ok = write_utf8(f, line) && write_utf8(f, L"\r\n");
    }
    CloseHandle(f);
    return ok;
}
