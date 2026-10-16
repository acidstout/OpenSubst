/*
 * lang.h - user interface languages.
 *
 * English and German are built in. Further languages are plain UTF-8 text
 * files in <exe dir>\lang\*.lng with lines of the form KEY=Text (see
 * strings.h for the keys, or export a template with /EXPORTLANG).
 *
 * Copyright (c) 2026 Rekow IT. MIT License, see LICENSE.
 */
#ifndef OPENSUBST_LANG_H
#define OPENSUBST_LANG_H

#include "common.h"

typedef enum StrId {
#define X(k, en, de) S_##k,
#include "strings.h"
#undef X
    S__COUNT
} StrId;

typedef struct LangFile {
    wchar_t file[MAX_PATH];     /* file name inside the lang folder */
    wchar_t name[64];           /* display name from "Language=" */
} LangFile;

/* Select a language: "" = follow Windows, "en", "de" or a .lng file name.
   Returns FALSE if a language file was requested but could not be read
   (the built-in language is used then). */
BOOL Lang_Select(const wchar_t *setting);

/* Translated text. Never NULL. */
const wchar_t *T(StrId id);

/* Enumerate lang\*.lng. Returns the number of entries written. */
int Lang_ListFiles(LangFile *out, int max);

/* Write all English strings as a language file template. */
BOOL Lang_ExportTemplate(const wchar_t *path);

#endif
