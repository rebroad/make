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
/* Memory debug level stored in 3 bits (bits 9-11): 0-5 */
#define DB_MEM_LEVEL_MASK  (0xE00)  /* Bits 9-11 */
#define DB_MEM_LEVEL_SHIFT (9)
#define DB_MEM_GET_LEVEL(_l) (((_l) & DB_MEM_LEVEL_MASK) >> DB_MEM_LEVEL_SHIFT)
#define DB_MEM_SET_LEVEL(_l, _level) (((_l) & ~DB_MEM_LEVEL_MASK) | ((_level) << DB_MEM_LEVEL_SHIFT))

#define DB_ALL          (0x1fff)  /* Includes all flags (0xE00 is level, not a flag) */

extern int db_level;

#define ISDB(_l)    ((_l)&db_level)

/* Check if memory debug level should be shown.
   Memory debug level is stored in 3 bits (0-5), hierarchical:
   Level N shows messages at level <= N */
#define ISDB_MEM(_level) \
  ((_level) > 0 && (_level) <= DB_MEM_GET_LEVEL(db_level))

/* When adding macros to this list be sure to update the value of
   XGETTEXT_OPTIONS in the po/Makevars file.  */
#define DBS(_l,_x)  do{ if(ISDB(_l)) { \
  char _ts_buf[16]; \
  db_timestamp(_ts_buf, sizeof(_ts_buf)); \
  printf("%s", _ts_buf); \
  print_spaces (depth); \
  printf _x; \
  fflush (stdout); \
} }while(0)

#define DBF(_l,_x)  do{ if(ISDB(_l)) { \
  char _ts_buf[16]; \
  db_timestamp(_ts_buf, sizeof(_ts_buf)); \
  printf("%s", _ts_buf); \
  print_spaces (depth); \
  printf (_x, file->name); \
  fflush (stdout); \
} }while(0)

/* Helper function to get current timestamp as string (format: "SSSSSmmm ") */
extern void db_timestamp (char *buf, size_t bufsize);

#define DB(_l,_x)   do{ if(ISDB(_l)) { \
  char _ts_buf[16]; \
  db_timestamp(_ts_buf, sizeof(_ts_buf)); \
  printf("%s", _ts_buf); \
  printf _x; \
  fflush (stdout); \
} }while(0)

/* Memory debug macro - replaces debug_write() */
#define DBM(_level, ...) do{ if(ISDB_MEM(_level)) { \
  char _ts_buf[16]; \
  db_timestamp(_ts_buf, sizeof(_ts_buf)); \
  printf("%s", _ts_buf); \
  printf(__VA_ARGS__); \
  fflush (stdout); \
} }while(0)
