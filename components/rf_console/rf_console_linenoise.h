/* linenoise.h -- VERSION 1.0
 *
 * Guerrilla line editing library against the idea that a line editing lib
 * needs to be 20,000 lines of C code.
 *
 * See linenoise.c for more information.
 *
 * ------------------------------------------------------------------------
 *
 * Copyright (c) 2010-2014, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2013, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __RF_CONSOLE_LINENOISE_H
#define __RF_CONSOLE_LINENOISE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <unistd.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct linenoiseCompletions {
  size_t len;
  char **cvec;
} linenoiseCompletions;

typedef void(linenoiseCompletionCallback)(const char *, linenoiseCompletions *);
typedef char*(linenoiseHintsCallback)(const char *, int *color, int *bold);
typedef void(linenoiseFreeHintsCallback)(void *);
void rf_linenoiseSetCompletionCallback(linenoiseCompletionCallback *);
void rf_linenoiseSetHintsCallback(linenoiseHintsCallback *);
void rf_linenoiseSetFreeHintsCallback(linenoiseFreeHintsCallback *);
void rf_linenoiseAddCompletion(linenoiseCompletions *, const char *);

int rf_linenoiseProbe(void);
char *rf_linenoise(const char *prompt);
void rf_linenoiseFree(void *ptr);
int rf_linenoiseHistoryAdd(const char *line);
int rf_linenoiseHistorySetMaxLen(int len);
int rf_linenoiseHistorySave(const char *filename);
int rf_linenoiseHistoryLoad(const char *filename);
void rf_linenoiseHistoryFree(void);
void rf_linenoiseClearScreen(void);
void rf_linenoiseSetMultiLine(int ml);
void rf_linenoiseSetDumbMode(int set);
bool rf_linenoiseIsDumbMode(void);
void rf_linenoisePrintKeyCodes(void);
void rf_linenoiseAllowEmpty(bool);
int rf_linenoiseSetMaxLineLen(size_t len);

typedef ssize_t (*linenoise_read_bytes_fn)(int, void*, size_t);
void rf_linenoiseSetReadFunction(linenoise_read_bytes_fn read_fn);
void rf_linenoiseSetReadCharacteristics(void);
typedef uint32_t (*rf_linenoise_time_fn)(void);
void rf_linenoiseSetTimeFunction(rf_linenoise_time_fn time_fn);

typedef void (*rf_linenoise_sync_fn)(void *context);
void rf_linenoiseSetSyncCallbacks(rf_linenoise_sync_fn lock_fn,
                                  rf_linenoise_sync_fn unlock_fn,
                                  void *context);
bool rf_linenoiseSuspendActiveLine(void);
void rf_linenoiseResumeActiveLine(void);

enum {
  RF_LINENOISE_MASKED_CANCELLED = -1,
  RF_LINENOISE_MASKED_TOO_LONG = -2,
};
int rf_linenoiseReadMasked(const char *prompt, char *buffer, size_t capacity);

/* Deterministic terminal width for host tests; zero restores probing. */
void rf_linenoiseSetColumnsOverride(size_t columns);

#ifdef __cplusplus
}
#endif

#endif /* __RF_CONSOLE_LINENOISE_H */
