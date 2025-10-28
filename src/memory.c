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

/* Extract filename from process command line for memory profiling
   Returns malloc'd string (caller must free) or NULL if no filename found
   caller: "main" or "job" - used in temp file naming */
char *
extract_filename_from_cmdline (pid_t pid, const char *caller)
{
  char cmdline_path[64];
  char cmdline_buf[4096];
  char source_filename[1024];
  char tmp_filename[64];
  FILE *cmdline_file, *tmp_file;
  size_t cmdline_len;
  char *ptr, *start, *end;
  int i, len;
  char *strip_ptr = NULL;
  char *result = NULL;
  struct timeval tv;
  int timestamp;

  /* Get timestamp for unique temp file naming */
  gettimeofday(&tv, NULL);
  timestamp = (int)(tv.tv_sec % 86400); /* HHMMSS */

  snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%d/cmdline", (int)pid);
  cmdline_file = fopen(cmdline_path, "r");
  if (!cmdline_file)
    return NULL;

  cmdline_len = fread(cmdline_buf, 1, sizeof(cmdline_buf) - 1, cmdline_file);
  fclose(cmdline_file);

  if (cmdline_len == 0) {
    return NULL;
  }

  /* /proc/cmdline uses \0 separators, convert to spaces for easier parsing */
  for (i = 0; i < (int)cmdline_len - 1; i++)
    if (cmdline_buf[i] == '\0')
      cmdline_buf[i] = ' ';
  cmdline_buf[cmdline_len] = '\0';

  /* Find ALL .cpp/.cc/.c occurrences, keep the LAST one with a "/" */
  end = NULL;
  ptr = cmdline_buf;
  while (*ptr) {
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
      /* Backtrack to previous space */
      candidate_start = ptr;
      while (candidate_start > cmdline_buf && candidate_start[-1] != ' ')
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
    while (start > cmdline_buf && start[-1] != ' ')
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

  /* Create debug temp file if no filename found and process uses significant memory */
  if (!result && cmdline_len > 0) {
    snprintf(tmp_filename, sizeof(tmp_filename), "/tmp/make_cmdline_%d.%s.txt", timestamp, caller);
    tmp_file = fopen(tmp_filename, "w");
    if (tmp_file) {
      fprintf(tmp_file, "%s", cmdline_buf);
      fclose(tmp_file);
    }
  }

  return result;
}

/* Extract filename from argv array for memory profiling (before process starts)
   Returns malloc'd string (caller must free) or NULL if no filename found
   caller: "main" or "job" - used in temp file naming */
char *
extract_filename_from_argv (const char **argv, const char *caller)
{
  char source_filename[1024];
  char tmp_filename[64];
  FILE *tmp_file;
  char *ptr, *start, *end;
  int i, len;
  char *strip_ptr = NULL;
  char *result = NULL;
  struct timeval tv;
  int timestamp;
  int argc = 0;

  /* Count arguments */
  while (argv[argc]) argc++;

  /* Get timestamp for unique temp file naming */
  gettimeofday(&tv, NULL);
  timestamp = (int)(tv.tv_sec % 86400); /* HHMMSS */

  /* Find ALL .cpp/.cc/.c occurrences in argv, keep the LAST one with a "/" */
  end = NULL;
  for (i = 0; i < argc; i++) {
    if (!argv[i]) continue;

    ptr = (char *)argv[i];
    while (*ptr) {
      char *candidate_end = NULL;
      char *candidate_start;
      int has_slash;

      /* Check for file extensions */
      if (strncmp(ptr, ".cpp", 4) == 0)
        candidate_end = ptr + 3;
      else if (strncmp(ptr, ".cc", 3) == 0)
        candidate_end = ptr + 2;
      else if (strncmp(ptr, ".c", 2) == 0 && (ptr[2] == '\0' || ptr[2] == ' '))
        candidate_end = ptr + 1;

      if (candidate_end) {
        /* Backtrack to previous space or start of string */
        candidate_start = ptr;
        while (candidate_start > (char *)argv[i] && candidate_start[-1] != ' ')
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
  }

  source_filename[0] = '\0';
  if (end) {
    /* Backtrack to find start of filepath */
    start = end;
    while (start > (char *)argv[0] && start[-1] != ' ')
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

  /* Create debug temp file if no filename found */
  if (!result && argc > 0) {
    snprintf(tmp_filename, sizeof(tmp_filename), "/tmp/make_argv_%d.%s.txt", timestamp, caller);
    tmp_file = fopen(tmp_filename, "w");
    if (tmp_file) {
      for (i = 0; i < argc; i++) {
        if (argv[i]) {
          fprintf(tmp_file, "%s ", argv[i]);
        }
      }
      fclose(tmp_file);
    }
  }

  return result;
}
