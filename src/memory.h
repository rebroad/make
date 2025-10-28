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

#ifndef MEMORY_H
#define MEMORY_H

/* Extract filename from process command line for memory profiling
   Returns malloc'd string (caller must free) or NULL if no filename found
   caller: "main" or "job" - used in temp file naming */
char *extract_filename_from_cmdline (pid_t pid, int depth, const char *caller);

/* Extract filename from argv array for memory profiling (before process starts)
   Returns malloc'd string (caller must free) or NULL if no filename found
   caller: "main" or "job" - used in temp file naming */
char *extract_filename_from_argv (const char **argv, const char *caller);

#endif /* MEMORY_H */
