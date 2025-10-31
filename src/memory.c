/* Memory profiling functions for GNU Make.
   Copyright (C) 2023 Free Software Foundation, Inc.
   This file is part of GNU Make.

   GNU Make is free software; you can redistribute it and/or modify it under the
   terms of the GNU General Public License as published by the Free Software
   Foundation; either version 3 of the License, or (at your option) any later
   version.

   GNU Make is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
   A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along with
   this program.  If not, see <https://www.gnu.org/licenses/>.  */

#include "makeint.h"
#include "memory.h"
#include "debug.h"
#include <sys/time.h>
#include <unistd.h>
#include <stdarg.h>

/* Non-blocking debug write helper - defined in memory.c for use across the codebase */
void
debug_write (const char *format, ...)
{
  char buf[300];
  int len;
  ssize_t written;
  va_list args;
  struct timeval tv;
  time_t secs;
  int milliseconds;

  /* Get current time for timestamp */
  gettimeofday(&tv, NULL);
  secs = tv.tv_sec % 60;  /* seconds from 0 to 59 */
  milliseconds = tv.tv_usec / 1000;  /* convert microseconds to milliseconds */

  va_start (args, format);
  /* Format message with timestamp prefix in one go */
  len = snprintf(buf, sizeof(buf), "%02ld%03d ", (long)secs, milliseconds);
  if (len > 0 && len < (int)sizeof(buf) - 1)
    {
      int msg_len = vsnprintf(buf + len, sizeof(buf) - len, format, args);
      if (msg_len > 0 && (len + msg_len) < (int)sizeof(buf))
        {
          int total_len = len + msg_len;
          written = write (STDERR_FILENO, buf, total_len);
          (void)written;  /* Ignore return value for debug output */
        }
    }
  va_end (args);
}

/* Common filename extraction logic - finds the last .cpp/.cc/.c file with "/" in the path
   Returns malloc'd string (caller must free) or NULL if no filename found */
static char *
extract_filename_common (const char *text, size_t text_len, const char *caller, int pid, int parent_pid, int depth, const char *debug_prefix)
{
  char source_filename[1024];
  char tmp_filename[64];
  FILE *tmp_file;
  char *ptr, *start, *end;
  int len;
  char *strip_ptr = NULL;
  char *result = NULL;
  struct timeval tv;
  int timestamp;

  /* Get timestamp for unique temp file naming */
  gettimeofday(&tv, NULL);
  {
    time_t secs = tv.tv_sec % 86400;  /* seconds since midnight */
    int hours = secs / 3600;
    int minutes = (secs % 3600) / 60;
    int seconds = secs % 60;
    int milliseconds = tv.tv_usec / 1000;
    timestamp = hours * 10000000 + minutes * 100000 + seconds * 1000 + milliseconds; /* HHMMSSms */
  }

  /* Find ALL .cpp/.cc/.c occurrences, keep the LAST one with a "/" */
  end = NULL;
  ptr = (char *)text;
  while (*ptr && (size_t)(ptr - text) < text_len) {
    char *candidate_end = NULL;
    char *candidate_start;
    int has_slash;

    /* Check for file extensions */
    if (strncmp(ptr, ".cpp", 4) == 0)
      candidate_end = ptr + 3;
    else if (strncmp(ptr, ".cc", 3) == 0)
      candidate_end = ptr + 2;
    else if (strncmp(ptr, ".c", 2) == 0 && (ptr[2] == ' ' || ptr[2] == '\0'))
      candidate_end = ptr + 1;

    if (candidate_end) {
      /* Backtrack to previous space or start of text */
      candidate_start = ptr;
      while (candidate_start > text && candidate_start[-1] != ' ')
        candidate_start--;

      /* Check if this token contains "/" (i.e., it's a filepath) */
      has_slash = 0;
      start = candidate_start;
      while (start <= candidate_end) {
        if (*start == '/') {
          has_slash = 1;
          break;
        }
        start++;
      }

      /* Only keep candidates with "/" */
      if (has_slash)
        end = candidate_end;
    }

    ptr++;
  }

  source_filename[0] = '\0';
  if (end) {
    /* Backtrack to find start of filepath */
    start = end;
    while (start > text && !(start[-1] == ' ' || start[-1] == '"'))
      start--;

    len = (end - start) + 1;
    if (len < 1000 && len > 0) {
      memcpy(source_filename, start, len);
      source_filename[len] = '\0';

      /* Strip leading "../" sequences */
      strip_ptr = source_filename;
      while (strncmp(strip_ptr, "../", 3) == 0)
        strip_ptr += 3;

      /* Return malloc'd copy of the filename */
      result = xstrdup(strip_ptr);
    }
  }

  /* Create debug temp file of the full cmdline */
  if (text_len > 0) {
    snprintf(tmp_filename, sizeof(tmp_filename), "/tmp/make_%s_%d.%s.txt", debug_prefix, timestamp, caller);
    tmp_file = fopen(tmp_filename, "w");
    if (tmp_file) {
      /* Prepend "FOUND: %s" if strip_ptr is not NULL */
      if (strip_ptr)
        fprintf(tmp_file, "FOUND: %s\n", strip_ptr);
      if (pid)
        fprintf(tmp_file, "PID=%d PPID=%d (d:%d)\n", pid, parent_pid, depth);

      /* Write everything except the final \0, then add LF */
      fwrite(text, 1, text_len - 1, tmp_file);
      fputc('\n', tmp_file);
      fclose(tmp_file);
    }
  }

  return result;
}

/* Extract filename from process command line for memory profiling
   Returns malloc'd string (caller must free) or NULL if no filename found
   If cmdline_out is not NULL, *cmdline_out will be set to a malloc'd copy of the cmdline (caller must free)
   max_cmdline_len: maximum length for cmdline_out (0 = no limit)
   caller: "main" or "job" - used in temp file naming */
char *
extract_filename_from_cmdline (pid_t pid, pid_t parent_pid, int depth, const char *caller, char **cmdline_out, size_t max_cmdline_len)
{
  char cmdline_path[64];
  char cmdline_buf[4096];
  FILE *cmdline_file;
  size_t cmdline_len;
  int i;

  if (cmdline_out)
    *cmdline_out = NULL;

  snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%d/cmdline", (int)pid);
  cmdline_file = fopen(cmdline_path, "r");
  if (!cmdline_file) {
    /* Debug: can't open cmdline file */
    debug_write("[DEBUG] extract_filename_from_cmdline: failed to open %s for PID %d\n", cmdline_path, (int)pid);
    return NULL;
  }

  cmdline_len = fread(cmdline_buf, 1, sizeof(cmdline_buf) - 1, cmdline_file);
  fclose(cmdline_file);

  if (cmdline_len == 0) return NULL;

  /* /proc/cmdline uses \0 separators, convert to spaces for easier parsing */
  for (i = 0; i < (int)cmdline_len - 1; i++)
    if (cmdline_buf[i] == '\0') cmdline_buf[i] = ' ';
  cmdline_buf[cmdline_len - 1] = '\0';

  /* Return cmdline if requested */
  if (cmdline_out) {
    size_t result_len = strlen(cmdline_buf);
    if (max_cmdline_len > 0 && result_len > max_cmdline_len) {
      /* Truncate and add "..." */
      *cmdline_out = xmalloc(max_cmdline_len + 1);
      snprintf(*cmdline_out, max_cmdline_len + 1, "%.*s...", (int)(max_cmdline_len - 3), cmdline_buf);
    } else {
      *cmdline_out = xstrdup(cmdline_buf);
    }
  }

  /* Use common extraction logic */
  return extract_filename_common(cmdline_buf, cmdline_len, caller, (int)pid, (int)parent_pid, depth, "cmdline");
}

/* Extract filename from argv array for memory profiling (before process starts)
   Returns malloc'd string (caller must free) or NULL if no filename found
   caller: "main" or "job" - used in temp file naming */
char *
extract_filename_from_argv (const char **argv, const char *caller)
{
  char argv_buf[4096];
  int argc = 0;
  int i;
  size_t total_len = 0;

  /* Count arguments and calculate total length needed */
  while (argv[argc]) {
    total_len += strlen(argv[argc]) + 1; /* +1 for space */
    argc++;
  }

  if (argc == 0 || total_len >= sizeof(argv_buf))
    return NULL;

  /* Concatenate all arguments with spaces */
  argv_buf[0] = '\0';
  for (i = 0; i < argc; i++) {
    if (i > 0) strcat(argv_buf, " ");
    strcat(argv_buf, argv[i]);
  }

  /* Use common extraction logic */
  return extract_filename_common(argv_buf, strlen(argv_buf), caller, 0, 0, 0, "argv");
}
