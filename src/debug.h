/* Debugging macros and interface.
Copyright (C) 1999-2023 Free Software Foundation, Inc.
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

#define DB_NONE         (0x000)
#define DB_BASIC        (0x001)
#define DB_VERBOSE      (0x002)
#define DB_JOBS         (0x004)
#define DB_IMPLICIT     (0x008)
#define DB_PRINT        (0x010)
#define DB_WHY          (0x020)
#define DB_MAKEFILES    (0x100)
/* Memory debug levels (ma1-ma5) */
#define DB_MEM_1        (0x200)  /* Errors and important warnings */
#define DB_MEM_2        (0x400)  /* Prediction/memory checking (includes DB_MEM_1) */
#define DB_MEM_3        (0x800)  /* Memory operations (includes DB_MEM_1-2) */
#define DB_MEM_4        (0x1000) /* Verbose debug details (includes DB_MEM_1-3) */
#define DB_MEM_5        (0x2000) /* Maximum verbosity (includes DB_MEM_1-4) */

#define DB_ALL          (0x3fff)  /* Includes all flags including memory debug */

extern int db_level;

#define ISDB(_l)    ((_l)&db_level)

/* Check if memory debug level should be shown.
   MEM_DEBUG_ERROR (1) requires DB_MEM_1
   MEM_DEBUG_PREDICT (2) requires DB_MEM_2 (which includes DB_MEM_1)
   MEM_DEBUG_INFO (3) requires DB_MEM_3 (which includes DB_MEM_1-2)
   MEM_DEBUG_VERBOSE (4) requires DB_MEM_4 (which includes DB_MEM_1-3)
   MEM_DEBUG_MAX (5) requires DB_MEM_5 (which includes DB_MEM_1-4) */
#define ISDB_MEM(_level) \
  ((_level == 1 && ISDB(DB_MEM_1)) || \
   (_level == 2 && ISDB(DB_MEM_2)) || \
   (_level == 3 && ISDB(DB_MEM_3)) || \
   (_level == 4 && ISDB(DB_MEM_4)) || \
   (_level == 5 && ISDB(DB_MEM_5)))

/* When adding macros to this list be sure to update the value of
   XGETTEXT_OPTIONS in the po/Makevars file.  */
#define DBS(_l,_x)  do{ if(ISDB(_l)) {print_spaces (depth); \
									  printf _x; fflush (stdout);} }while(0)

#define DBF(_l,_x)  do{ if(ISDB(_l)) {print_spaces (depth); \
									  printf (_x, file->name); \
									  fflush (stdout);} }while(0)

#define DB(_l,_x)   do{ if(ISDB(_l)) {printf _x; fflush (stdout);} }while(0)

/* Memory debug macro - replaces debug_write() */
#define DBM(_level,_x) do{ if(ISDB_MEM(_level)) {printf _x; fflush (stdout);} }while(0)
