/* Argument parsing and main program of GNU Make.
Copyright (C) 1988-2023 Free Software Foundation, Inc.
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
#include "os.h"
#include "filedef.h"
#include "dep.h"
#include "variable.h"
#include "job.h"
#include "commands.h"
#include "rule.h"
#include "debug.h"
#include "getopt.h"
#include "shuffle.h"
#include "memory.h"

#include <assert.h>
#ifdef HAVE_SYS_MMAN_H
# include <sys/mman.h>
#endif
#ifdef HAVE_PTHREAD_H
# include <pthread.h>
#endif
#ifdef HAVE_SYS_IOCTL_H
# include <sys/ioctl.h>
#endif
#include <termios.h>
#include <unistd.h>
#ifdef HAVE_DIRENT_H
# include <dirent.h>
#endif
#include <stdarg.h>

/* Set to 1 to enable verbose memory monitor debugging (timing, iteration markers, etc.) */
#define DEBUG_MEMORY_MONITOR 0

/* Memory monitoring for auto-adjust-jobs feature */
#define MAX_TRACKED_DESCENDANTS 100    /* Max concurrent compilations to track */
#ifdef _AMIGA
# include <dos/dos.h>
# include <proto/dos.h>
#endif
#ifdef WINDOWS32
# include <windows.h>
# include <io.h>
# include "pathstuff.h"
# include "sub_proc.h"
# include "w32err.h"
#endif
#ifdef __EMX__
# include <sys/types.h>
# include <sys/wait.h>
#endif
#ifdef HAVE_FCNTL_H
# include <fcntl.h>
#endif

#ifdef _AMIGA
int __stack = 20000; /* Make sure we have 20K of stack space */
#endif
#ifdef VMS
int vms_use_mcr_command = 0;
int vms_always_use_cmd_file = 0;
int vms_gnv_shell = 0;
int vms_legacy_behavior = 0;
int vms_comma_separator = 0;
int vms_unix_simulation = 0;
int vms_report_unix_paths = 0;

/* Evaluates if a VMS environment option is set, only look at first character */
static int
get_vms_env_flag (const char *name, int default_value)
{
char * value;
char x;

  value = getenv (name);
  if (value == NULL)
    return default_value;

  x = toupper (value[0]);
  switch (x)
    {
    case '1':
    case 'T':
    case 'E':
      return 1;
      break;
    case '0':
    case 'F':
    case 'D':
      return 0;
    }
}
#endif

#if defined HAVE_WAITPID || defined HAVE_WAIT3
# define HAVE_WAIT_NOHANG
#endif

#ifndef HAVE_UNISTD_H
int chdir ();
#endif
#ifndef STDC_HEADERS
# ifndef sun                    /* Sun has an incorrect decl in a header.  */
void exit (int) NORETURN;
# endif
double atof ();
#endif

static void clean_jobserver (int status);
static void print_data_base (void);
static void print_version (void);
static void print_compiled_features (const char *precede);
static void decode_switches (int argc, const char **argv,
                             enum variable_origin origin);
static void decode_env_switches (const char *envar, size_t len,
                                 enum variable_origin origin);
static char *quote_for_env (char *out, const char *in);
static void initialize_global_hash_tables (void);


/* True if C is a switch value that corresponds to a short option.  */

#define short_option(c) ((c) <= CHAR_MAX)

/* The structure used to hold the list of strings given
   in command switches of a type that takes strlist arguments.  */

struct stringlist
  {
    const char **list;  /* Nil-terminated list of strings.  */
    unsigned int idx;   /* Index into above.  */
    unsigned int max;   /* Number of pointers allocated.  */
  };


/* The recognized command switches.  */

/* Nonzero means do extra verification (that may slow things down).  */

int verify_flag;

/* Nonzero means do not print commands to be executed (-s).  */

static int silent_flag;
static const int default_silent_flag = 0;
static enum variable_origin silent_origin = o_default;

/* Nonzero means either -s was given, or .SILENT-with-no-deps was seen.  */

int run_silent = 0;

/* Nonzero means just touch the files
   that would appear to need remaking (-t)  */

int touch_flag;

/* Nonzero means just print what commands would need to be executed,
   don't actually execute them (-n).  */

int just_print_flag;

/* Print debugging info (--debug).  */

static struct stringlist *db_flags = 0;
static int debug_flag = 0;

int db_level = 0;

/* Synchronize output (--output-sync).  */

char *output_sync_option = 0;

/* Environment variables override makefile definitions.  */

int env_overrides = 0;

/* Nonzero means ignore status codes returned by commands
   executed to remake files.  Just treat them all as successful (-i).  */

int ignore_errors_flag = 0;

/* Nonzero means don't remake anything, just print the data base
   that results from reading the makefile (-p).  */

int print_data_base_flag = 0;

/* Nonzero means don't remake anything; just return a nonzero status
   if the specified targets are not up to date (-q).  */

int question_flag = 0;

/* Nonzero means do not use any of the builtin rules (-r) / variables (-R).  */

int no_builtin_rules_flag = 0;
int no_builtin_variables_flag = 0;

/* Nonzero means keep going even if remaking some file fails (-k).  */

int keep_going_flag;
static const int default_keep_going_flag = 0;
static enum variable_origin keep_going_origin = o_default;

/* Nonzero means check symlink mtimes.  */

int check_symlink_flag = 0;

static int print_directory_flag = -1;
static const int default_print_directory_flag = -1;
static enum variable_origin print_directory_origin = o_default;

/* Nonzero means print version information.  */

int print_version_flag = 0;

/* List of makefiles given with -f switches.  */

static struct stringlist *makefiles = 0;

/* Size of the stack when we started.  */

#ifdef SET_STACK_SIZE
struct rlimit stack_limit;
#endif


/* Number of job slots for parallelism.  */

unsigned int job_slots;

#define INVALID_JOB_SLOTS (-1)
static unsigned int master_job_slots = 0;
static int arg_job_slots = INVALID_JOB_SLOTS;

static const int default_job_slots = INVALID_JOB_SLOTS;

/* Value of job_slots that means no limit.  */

static const int inf_jobs = 0;

/* Authorization for the jobserver.  */

char *jobserver_auth = NULL;

/* Style for the jobserver.  */

static char *jobserver_style = NULL;

/* Shuffle mode for goals and prerequisites.  */

static char *shuffle_mode = NULL;

/* Handle for the mutex to synchronize output of our children under -O.  */

static char *sync_mutex = NULL;

/* Maximum load average at which multiple jobs will be run.
   Negative values mean unlimited, while zero means limit to
   zero load (which could be useful to start infinite jobs remotely
   but one at a time locally).  */
double max_load_average = -1.0;
double default_load_average = -1.0;

/* Memory-aware job adjustment */
int memory_aware_flag = -1;  /* -1 = not set, will check env */

/* Memory debug level (0 = no debug output, higher = more verbose) */
int memory_debug_level = MEM_DEBUG_NONE;
static const int default_memory_debug_level = MEM_DEBUG_NONE;

/* Memory profiling variables */
struct file_memory_profile *memory_profiles = NULL;
unsigned int memory_profile_count = 0;
unsigned int memory_profiles_capacity = 0;
static int disable_memory_display = 0;  /* Disable memory status display */
#ifdef HAVE_PTHREAD_H
static int status_line_shown = 0;
static int spinner_state = 0;
static pthread_t memory_monitor_thread;
static time_t monitor_start_time = 0;
#endif
static volatile int monitor_thread_running = 0;
static struct timeval make_start_time = {0, 0};

/* Initialize memory monitoring settings from environment */
static void
init_memory_monitoring_env (void)
{
  char *env;

  /* Check if enabled via environment (default: ON) */
  if (memory_aware_flag == -1)
    {
      env = getenv ("MAKE_MEMORY_AWARE");
      if (env)
        memory_aware_flag = (strcmp (env, "0") != 0 && strcmp (env, "no") != 0 && strcmp (env, "false") != 0);
      else
        memory_aware_flag = 1;  /* Default: ON */
    }
}

/* List of directories given with -C switches.  */

static struct stringlist *directories = 0;

/* List of include directories given with -I switches.  */

static struct stringlist *include_dirs = 0;

/* List of files given with -o switches.  */

static struct stringlist *old_files = 0;

/* List of files given with -W switches.  */

static struct stringlist *new_files = 0;

/* List of strings to be eval'd.  */
static struct stringlist *eval_strings = 0;

/* If nonzero, we should just print usage and exit.  */

static int print_usage_flag = 0;

/* If nonzero, we should print a warning message
   for each reference to an undefined variable.  */

int warn_undefined_variables_flag;

/* If nonzero, always build all targets, regardless of whether
   they appear out of date or not.  */

static int always_make_set = 0;
int always_make_flag = 0;

/* If nonzero, we're in the "try to rebuild makefiles" phase.  */

int rebuilding_makefiles = 0;

/* Remember the original value of the SHELL variable, from the environment.  */

struct variable shell_var;

/* This character introduces a command: it's the first char on the line.  */

char cmd_prefix = '\t';

/* Whether or not .NOTINTERMEDIATE with no prerequisites was given.  */
unsigned int no_intermediates;

/* Count the number of commands we've invoked, that might change something in
   the filesystem.  Start with 1 so calloc'd memory never matches.  */

unsigned long command_count = 1;

/* Remember the location of the name of the batch file from stdin.  */

static int stdin_offset = -1;


/* The usage output.  We write it this way to make life easier for the
   translators, especially those trying to translate to right-to-left
   languages like Hebrew.  */

static const char *const usage[] =
  {
    N_("Options:\n"),
    N_("\
  -b, -m                      Ignored for compatibility.\n"),
    N_("\
  -B, --always-make           Unconditionally make all targets.\n"),
    N_("\
  -C DIRECTORY, --directory=DIRECTORY\n\
                              Change to DIRECTORY before doing anything.\n"),
    N_("\
  -d                          Print lots of debugging information.\n"),
    N_("\
  --debug[=FLAGS]             Print various types of debugging information.\n"),
    N_("\
  -e, --environment-overrides\n\
                              Environment variables override makefiles.\n"),
    N_("\
  -E STRING, --eval=STRING    Evaluate STRING as a makefile statement.\n"),
    N_("\
  -f FILE, --file=FILE, --makefile=FILE\n\
                              Read FILE as a makefile.\n"),
    N_("\
  -h, --help                  Print this message and exit.\n"),
    N_("\
  -i, --ignore-errors         Ignore errors from recipes.\n"),
    N_("\
  -I DIRECTORY, --include-dir=DIRECTORY\n\
                              Search DIRECTORY for included makefiles.\n"),
    N_("\
  -j [N], --jobs[=N]          Allow N jobs at once; infinite jobs with no arg.\n"),
    N_("\
  --jobserver-style=STYLE     Select the style of jobserver to use.\n"),
    N_("\
  -k, --keep-going            Keep going when some targets can't be made.\n"),
    N_("\
  -l [N], --load-average[=N], --max-load[=N]\n\
                              Don't start multiple jobs unless load is below N.\n"),
    N_("\
  -L, --check-symlink-times   Use the latest mtime between symlinks and target.\n"),
    N_("\
  -n, --just-print, --dry-run, --recon\n\
                              Don't actually run any recipe; just print them.\n"),
    N_("\
  -o FILE, --old-file=FILE, --assume-old=FILE\n\
                              Consider FILE to be very old and don't remake it.\n"),
    N_("\
  -O[TYPE], --output-sync[=TYPE]\n\
                              Synchronize output of parallel jobs by TYPE.\n"),
    N_("\
  -p, --print-data-base       Print make's internal database.\n"),
    N_("\
  -q, --question              Run no recipe; exit status says if up to date.\n"),
    N_("\
  -r, --no-builtin-rules      Disable the built-in implicit rules.\n"),
    N_("\
  -R, --no-builtin-variables  Disable the built-in variable settings.\n"),
    N_("\
  --shuffle[={SEED|random|reverse|none}]\n\
                              Perform shuffle of prerequisites and goals.\n"),
    N_("\
  -s, --silent, --quiet       Don't echo recipes.\n"),
    N_("\
  --no-silent                 Echo recipes (disable --silent mode).\n"),
    N_("\
  -S, --no-keep-going, --stop\n\
                              Turns off -k.\n"),
    N_("\
  -t, --touch                 Touch targets instead of remaking them.\n"),
    N_("\
  --trace                     Print tracing information.\n"),
    N_("\
  -v, --version               Print the version number of make and exit.\n"),
    N_("\
  -w, --print-directory       Print the current directory.\n"),
    N_("\
  --no-print-directory        Turn off -w, even if it was turned on implicitly.\n"),
    N_("\
  -W FILE, --what-if=FILE, --new-file=FILE, --assume-new=FILE\n\
                              Consider FILE to be infinitely new.\n"),
    N_("\
  --warn-undefined-variables  Warn when an undefined variable is referenced.\n"),
    N_("\
  --nomem                     Disable memory status display.\n"),
    NULL
  };

/* Nonzero if the "--trace" option was given.  */

static int trace_flag = 0;

/* The structure that describes an accepted command switch.  */

struct command_switch
  {
    int c;                        /* The switch character.  */

    enum                          /* Type of the value.  */
      {
        flag,                     /* Turn int flag on.  */
        flag_off,                 /* Turn int flag off.  */
        string,                   /* One string per invocation.  */
        strlist,                  /* One string per switch.  */
        filename,                 /* A string containing a file name.  */
        positive_int,             /* A positive integer.  */
        floating,                 /* A floating-point number (double).  */
        ignore                    /* Ignored.  */
      } type;

    void *value_ptr;              /* Pointer to the value-holding variable.  */

    unsigned int env:1;           /* Can come from MAKEFLAGS.  */
    unsigned int toenv:1;         /* Should be put in MAKEFLAGS.  */
    unsigned int no_makefile:1;   /* Don't propagate when remaking makefiles. */
    unsigned int specified:1;     /* Set if the switch was specified somewhere.
                                     Allows switches that are ON by default to
                                     appear in MAKEFLAGS when set explicitly. */

    const void *noarg_value;      /* Pointer to value used if no arg given.  */
    const void *default_value;    /* Pointer to default value.  */

    const char *long_name;        /* Long option name.  */
    enum variable_origin *origin; /* Origin of the value.  */
  };

/* The table of command switches.
   Order matters here: this is the order MAKEFLAGS will be constructed.
   So be sure all simple flags (single char, no argument) come first.  */

#define TEMP_STDIN_OPT (CHAR_MAX+10)

static struct command_switch switches[] =
  {
    { 'b', ignore, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 'B', flag, &always_make_set, 1, 1, 0, 0, 0, 0, "always-make", 0 },
    { 'd', flag, &debug_flag, 1, 1, 0, 0, 0, 0, 0, 0 },
    { 'e', flag, &env_overrides, 1, 1, 0, 0, 0, 0, "environment-overrides", 0 },
    { 'E', strlist, &eval_strings, 1, 0, 0, 0, 0, 0, "eval", 0 },
    { 'h', flag, &print_usage_flag, 0, 0, 0, 0, 0, 0, "help", 0 },
    { 'i', flag, &ignore_errors_flag, 1, 1, 0, 0, 0, 0, "ignore-errors", 0 },
    { 'k', flag, &keep_going_flag, 1, 1, 0, 0, 0, &default_keep_going_flag,
      "keep-going", &keep_going_origin },
    { 'L', flag, &check_symlink_flag, 1, 1, 0, 0, 0, 0, "check-symlink-times", 0 },
    { 'm', ignore, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 'n', flag, &just_print_flag, 1, 1, 1, 0, 0, 0, "just-print", 0 },
    { 'p', flag, &print_data_base_flag, 1, 1, 0, 0, 0, 0, "print-data-base", 0 },
    { 'q', flag, &question_flag, 1, 1, 1, 0, 0, 0, "question", 0 },
    { 'r', flag, &no_builtin_rules_flag, 1, 1, 0, 0, 0, 0, "no-builtin-rules", 0 },
    { 'R', flag, &no_builtin_variables_flag, 1, 1, 0, 0, 0, 0,
      "no-builtin-variables", 0 },
    { 's', flag, &silent_flag, 1, 1, 0, 0, 0, &default_silent_flag, "silent",
      &silent_origin },
    { 'S', flag_off, &keep_going_flag, 1, 1, 0, 0, 0, &default_keep_going_flag,
      "no-keep-going", &keep_going_origin },
    { 't', flag, &touch_flag, 1, 1, 1, 0, 0, 0, "touch", 0 },
    { 'v', flag, &print_version_flag, 1, 0, 0, 0, 0, 0, "version", 0 },
    { 'w', flag, &print_directory_flag, 1, 1, 0, 0, 0,
      &default_print_directory_flag, "print-directory", &print_directory_origin },

    /* These options take arguments.  */
    { 'C', filename, &directories, 0, 0, 0, 0, 0, 0, "directory", 0 },
    { 'f', filename, &makefiles, 0, 0, 0, 0, 0, 0, "file", 0 },
    { 'I', filename, &include_dirs, 1, 1, 0, 0, 0, 0,
      "include-dir", 0 },
    { 'j', positive_int, &arg_job_slots, 1, 1, 0, 0, &inf_jobs, &default_job_slots,
      "jobs", 0 },
    { 'l', floating, &max_load_average, 1, 1, 0, 0, &default_load_average,
      &default_load_average, "load-average", 0 },
    { 'o', filename, &old_files, 0, 0, 0, 0, 0, 0, "old-file", 0 },
    { 'O', string, &output_sync_option, 1, 1, 0, 0, "target", 0, "output-sync", 0 },
    { 'W', filename, &new_files, 0, 0, 0, 0, 0, 0, "what-if", 0 },

    /* These are long-style options.  */
    { CHAR_MAX+1, strlist, &db_flags, 1, 1, 0, 0, "basic", 0, "debug", 0 },
    { CHAR_MAX+2, string, &jobserver_auth, 1, 1, 0, 0, 0, 0, JOBSERVER_AUTH_OPT, 0 },
    { CHAR_MAX+3, flag, &trace_flag, 1, 1, 0, 0, 0, 0, "trace", 0 },
    { CHAR_MAX+4, flag_off, &print_directory_flag, 1, 1, 0, 0, 0,
      &default_print_directory_flag, "no-print-directory", &print_directory_origin },
    { CHAR_MAX+5, flag, &warn_undefined_variables_flag, 1, 1, 0, 0, 0, 0,
      "warn-undefined-variables", 0 },
    { CHAR_MAX+7, string, &sync_mutex, 1, 1, 0, 0, 0, 0, "sync-mutex", 0 },
    { CHAR_MAX+8, flag_off, &silent_flag, 1, 1, 0, 0, 0, &default_silent_flag,
      "no-silent", &silent_origin },
    { CHAR_MAX+9, string, &jobserver_auth, 1, 0, 0, 0, 0, 0, "jobserver-fds", 0 },
    /* There is special-case handling for this in decode_switches() as well.  */
    { TEMP_STDIN_OPT, filename, &makefiles, 0, 0, 0, 0, 0, 0, "temp-stdin", 0 },
    { CHAR_MAX+11, string, &shuffle_mode, 1, 1, 0, 0, "random", 0, "shuffle", 0 },
    { CHAR_MAX+12, string, &jobserver_style, 1, 0, 0, 0, 0, 0, "jobserver-style", 0 },
    { CHAR_MAX+13, flag, &memory_aware_flag, 1, 1, 0, 0, 0, 0, "memory-aware", 0 },
    { CHAR_MAX+14, flag_off, &memory_aware_flag, 1, 1, 0, 0, 0, 0, "no-memory-aware", 0 },
    { CHAR_MAX+15, flag, &disable_memory_display, 1, 1, 0, 0, 0, 0, "nomem", 0 },
    { CHAR_MAX+16, positive_int, &memory_debug_level, 1, 1, 0, 0, 0, &default_memory_debug_level, "memdebug", 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
  };

/* Secondary long names for options.  */

static struct option long_option_aliases[] =
  {
    { "quiet",          no_argument,            0, 's' },
    { "stop",           no_argument,            0, 'S' },
    { "new-file",       required_argument,      0, 'W' },
    { "assume-new",     required_argument,      0, 'W' },
    { "assume-old",     required_argument,      0, 'o' },
    { "max-load",       optional_argument,      0, 'l' },
    { "dry-run",        no_argument,            0, 'n' },
    { "recon",          no_argument,            0, 'n' },
    { "makefile",       required_argument,      0, 'f' },
  };

/* List of goal targets.  */

static struct goaldep *goals, *lastgoal;

/* List of variables which were defined on the command line
   (or, equivalently, in MAKEFLAGS).  */

struct command_variable
  {
    struct command_variable *next;
    struct variable *variable;
  };
static struct command_variable *command_variables;

/* The name we were invoked with.  */

const char *program;

/* Our current directory before processing any -C options.  */

char *directory_before_chdir;

/* Our current directory after processing all -C options.  */

char *starting_directory;

/* Value of the MAKELEVEL variable at startup (or 0).  */

unsigned int makelevel;

/* Pointer to the value of the .DEFAULT_GOAL special variable.
   The value will be the name of the goal to remake if the command line
   does not override it.  It can be set by the makefile, or else it's
   the first target defined in the makefile whose name does not start
   with '.'.  */

struct variable * default_goal_var;

/* Pointer to structure for the file .DEFAULT
   whose commands are used for any file that has none of its own.
   This is zero if the makefiles do not define .DEFAULT.  */

struct file *default_file;

/* Nonzero if we have seen the magic '.POSIX' target.
   This turns on pedantic compliance with POSIX.2.  */

int posix_pedantic;

/* Nonzero if we have seen the '.SECONDEXPANSION' target.
   This turns on secondary expansion of prerequisites.  */

int second_expansion;

/* Nonzero if we have seen the '.ONESHELL' target.
   This causes the entire recipe to be handed to SHELL
   as a single string, potentially containing newlines.  */

int one_shell;

/* One of OUTPUT_SYNC_* if the "--output-sync" option was given.  This
   attempts to synchronize the output of parallel jobs such that the results
   of each job stay together.  */

int output_sync = OUTPUT_SYNC_NONE;

/* Nonzero if we have seen the '.NOTPARALLEL' target.
   This turns off parallel builds for this invocation of make.  */

int not_parallel;

/* Nonzero if some rule detected clock skew; we keep track so (a) we only
   print one warning about it during the run, and (b) we can print a final
   warning at the end of the run. */

int clock_skew_detected;

/* Map of possible stop characters for searching strings.  */
#ifndef UCHAR_MAX
# define UCHAR_MAX 255
#endif
unsigned short stopchar_map[UCHAR_MAX + 1] = {0};

/* If output-sync is enabled we'll collect all the output generated due to
   options, while reading makefiles, etc.  */

struct output make_sync;


/* Mask of signals that are being caught with fatal_error_signal.  */

#if defined(POSIX)
sigset_t fatal_signal_set;
#elif defined(HAVE_SIGSETMASK)
int fatal_signal_mask;
#endif

#if !HAVE_DECL_BSD_SIGNAL && !defined bsd_signal
# if !defined HAVE_SIGACTION
#  define bsd_signal signal
# else
typedef void (*bsd_signal_ret_t) (int);

static bsd_signal_ret_t
bsd_signal (int sig, bsd_signal_ret_t func)
{
  struct sigaction act, oact;
  act.sa_handler = func;
  act.sa_flags = SA_RESTART;
  sigemptyset (&act.sa_mask);
  sigaddset (&act.sa_mask, sig);
  if (sigaction (sig, &act, &oact) != 0)
    return SIG_ERR;
  return oact.sa_handler;
}
# endif
#endif

static void
initialize_global_hash_tables (void)
{
  init_hash_global_variable_set ();
  strcache_init ();
  init_hash_files ();
  hash_init_directories ();
  hash_init_function_table ();
}

/* This character map locate stop chars when parsing GNU makefiles.
   Each element is true if we should stop parsing on that character.  */

static void
initialize_stopchar_map (void)
{
  int i;

  stopchar_map[(int)'\0'] = MAP_NUL;
  stopchar_map[(int)'#'] = MAP_COMMENT;
  stopchar_map[(int)';'] = MAP_SEMI;
  stopchar_map[(int)'='] = MAP_EQUALS;
  stopchar_map[(int)':'] = MAP_COLON;
  stopchar_map[(int)'|'] = MAP_PIPE;
  stopchar_map[(int)'.'] = MAP_DOT | MAP_USERFUNC;
  stopchar_map[(int)','] = MAP_COMMA;
  stopchar_map[(int)'('] = MAP_VARSEP;
  stopchar_map[(int)'{'] = MAP_VARSEP;
  stopchar_map[(int)'}'] = MAP_VARSEP;
  stopchar_map[(int)')'] = MAP_VARSEP;
  stopchar_map[(int)'$'] = MAP_VARIABLE;

  stopchar_map[(int)'-'] = MAP_USERFUNC;
  stopchar_map[(int)'_'] = MAP_USERFUNC;

  stopchar_map[(int)' '] = MAP_BLANK;
  stopchar_map[(int)'\t'] = MAP_BLANK;

  stopchar_map[(int)'/'] = MAP_DIRSEP;
#if defined(VMS)
  stopchar_map[(int)':'] |= MAP_DIRSEP;
  stopchar_map[(int)']'] |= MAP_DIRSEP;
  stopchar_map[(int)'>'] |= MAP_DIRSEP;
#elif defined(HAVE_DOS_PATHS)
  stopchar_map[(int)'\\'] |= MAP_DIRSEP;
#endif

  for (i = 1; i <= UCHAR_MAX; ++i)
    {
      if (isspace (i) && NONE_SET (stopchar_map[i], MAP_BLANK))
        /* Don't mark blank characters as newline characters.  */
        stopchar_map[i] |= MAP_NEWLINE;
      else if (isalnum (i))
        stopchar_map[i] |= MAP_USERFUNC;
    }
}

/* This code is stolen from gnulib.
   If/when we abandon the requirement to work with K&R compilers, we can
   remove this (and perhaps other parts of GNU Make!) and migrate to using
   gnulib directly.

   This is called only through atexit(), which means die() has already been
   invoked.  So, call exit() here directly.  Apparently that works...?
*/

/* Close standard output, exiting with status 'exit_failure' on failure.
   If a program writes *anything* to stdout, that program should close
   stdout and make sure that it succeeds before exiting.  Otherwise,
   suppose that you go to the extreme of checking the return status
   of every function that does an explicit write to stdout.  The last
   printf can succeed in writing to the internal stream buffer, and yet
   the fclose(stdout) could still fail (due e.g., to a disk full error)
   when it tries to write out that buffered data.  Thus, you would be
   left with an incomplete output file and the offending program would
   exit successfully.  Even calling fflush is not always sufficient,
   since some file systems (NFS and CODA) buffer written/flushed data
   until an actual close call.

   Besides, it's wasteful to check the return value from every call
   that writes to stdout -- just let the internal stream state record
   the failure.  That's what the ferror test is checking below.

   It's important to detect such failures and exit nonzero because many
   tools (most notably 'make' and other build-management systems) depend
   on being able to detect failure in other tools via their exit status.  */

static void
close_stdout (void)
{
  int prev_fail = ferror (stdout);
  int fclose_fail = fclose (stdout);

  if (prev_fail || fclose_fail)
    {
      if (fclose_fail)
        perror_with_name (_("write error: stdout"), "");
      else
        O (error, NILF, _("write error: stdout"));
      exit (MAKE_TROUBLE);
    }
}

static const char *
expand_command_line_file (const char *name)
{
  const char *cp;
  char *expanded = 0;

  if (name[0] == '\0')
    O (fatal, NILF, _("empty string invalid as file name"));

  if (name[0] == '~')
    {
      expanded = tilde_expand (name);
      if (expanded && expanded[0] != '\0')
        name = expanded;
    }

  /* This is also done in parse_file_seq, so this is redundant
     for names read from makefiles.  It is here for names passed
     on the command line.  */
  while (name[0] == '.' && name[1] == '/')
    {
      name += 2;
      while (name[0] == '/')
        /* Skip following slashes: ".//foo" is "foo", not "/foo".  */
        ++name;
    }

  if (name[0] == '\0')
    {
      /* Nothing else but one or more "./", maybe plus slashes!  */
      name = "./";
    }

  cp = strcache_add (name);

  free (expanded);

  return cp;
}

/* Toggle -d on receipt of SIGUSR1.  */

#ifdef SIGUSR1
static void
debug_signal_handler (int sig UNUSED)
{
  db_level = db_level ? DB_NONE : DB_BASIC;
}
#endif

/* Shared memory structure for inter-process communication */
#define MAX_RESERVATIONS 64  /* Max concurrent sub-makes to track reservations for */

struct pid_reservation {
  pid_t pid;
  volatile unsigned long reserved_mb;
};

struct shared_memory_data {
  volatile unsigned int reservation_count;  /* Number of active reservation slots */
  struct pid_reservation reservations[MAX_RESERVATIONS];  /* Per-PID reservation tracking */
  volatile unsigned long unused_peaks_mb;  /* Sum of current memory usage of all running compilations */
  volatile unsigned long total_reserved_mb;  /* Sum of all reserved memory */
#if defined(HAVE_SYS_MMAN_H) && defined(HAVE_SHM_OPEN) && defined(HAVE_PTHREAD_H)
  pthread_mutex_t reserved_count_mutex;
  pthread_mutex_t total_reserved_mb_mutex;
#endif
} __attribute__((aligned(8)));
#if defined(HAVE_SYS_MMAN_H) && defined(HAVE_SHM_OPEN) && defined(HAVE_PTHREAD_H)
static struct shared_memory_data *shared_data = NULL;
static int shared_memory_fd = -1;
static const char *SHARED_MEMORY_NAME = "/make_memory_shared";
#endif

/* Global dirty flag for memory profiles */
static int memory_profiles_dirty = 0;

/* Main-make-only monitoring data (not shared between processes) */
static struct {
  unsigned int compile_count;
  struct {
    pid_t pid;
    unsigned long peak_mb;
    unsigned long old_peak_mb; // Not made redundant by reserved_mb as unused_peaks calculated per job
    unsigned long current_mb;
    int profile_idx;        /* Index into memory_profiles array, -1 if not found */
  } descendants[MAX_TRACKED_DESCENDANTS];
} main_monitoring_data = {0};

void
grow_memory_profiles (void)
{
  unsigned int new_capacity;
  struct file_memory_profile *new_profiles;

  if (memory_profiles_capacity == 0) {
    /* First allocation: start with 1000 entries */
    new_capacity = 1000;
  } else {
    /* Double the capacity */
    new_capacity = memory_profiles_capacity * 2;
  }

  if (memory_profiles == NULL) {
    /* First allocation */
    new_profiles = (struct file_memory_profile *)xmalloc (new_capacity * sizeof(struct file_memory_profile));
  } else {
    /* Reallocation */
    new_profiles = (struct file_memory_profile *)xrealloc (memory_profiles,
                                                            new_capacity * sizeof(struct file_memory_profile));
  }

  if (!new_profiles) {
    debug_write(MEM_DEBUG_ERROR, "[MEMORY] ERROR: Failed to grow memory_profiles array from %u to %u\n",
                memory_profiles_capacity, new_capacity);
    return;
  }

  /* Initialize new entries */
  memset(&new_profiles[memory_profiles_capacity], 0,
         (new_capacity - memory_profiles_capacity) * sizeof(struct file_memory_profile));

  memory_profiles = new_profiles;
  memory_profiles_capacity = new_capacity;

  debug_write(MEM_DEBUG_MAX, "[MEMORY] Grew memory_profiles array to %u entries (PID=%d PPID=%d makelevel=%u)\n",
              new_capacity, (int)getpid(), (int)getppid(), makelevel);
}

/* Record memory usage for a file using profile index (main make only) */
void
record_file_memory_usage_by_index (int profile_idx, unsigned long memory_mb, int final)
{
  time_t now;
  unsigned long prev_peak_mb;

  /* Only update if we have a valid profile index */
  if (profile_idx < 0 || (unsigned int)profile_idx >= memory_profile_count) return;
  if (memory_profiles == NULL) {
    debug_write(MEM_DEBUG_ERROR, "[MEMORY] ERROR: record_file_memory_usage_by_index called but memory_profiles is NULL (profile_idx=%d, count=%u)\n",
                profile_idx, memory_profile_count);
    return;
  }

  prev_peak_mb = memory_profiles[profile_idx].peak_memory_mb;

  if (memory_mb <= prev_peak_mb && !final) return;

  now = time (NULL);
  fflush (stderr);

  /* If final=1 and the new peak is lower, reduce by 33% of the difference */
  if (final && memory_mb < prev_peak_mb) {
    memory_mb = prev_peak_mb - (prev_peak_mb - memory_mb) / 3;
    debug_write(MEM_DEBUG_VERBOSE, "[MEMORY] Reducing peak by 33%% of difference (peak: %luMB -> %luMB final: %d file: %s)\n",
                prev_peak_mb, memory_mb, final,
                memory_profiles[profile_idx].filename ? memory_profiles[profile_idx].filename : "unknown");
  } else {
    debug_write(MEM_DEBUG_VERBOSE, "[MEMORY] Marking memory_profiles_dirty (peak: %luMB -> %luMB final: %d file: %s)\n",
                prev_peak_mb, memory_mb, final,
                memory_profiles[profile_idx].filename ? memory_profiles[profile_idx].filename : "unknown");
  }

  memory_profiles[profile_idx].peak_memory_mb = memory_mb;
  memory_profiles[profile_idx].last_used = now;
  memory_profiles_dirty = 1;
}

/* Forward declarations */
static int init_shared_memory (void);

/* Initialize shared memory for inter-process communication */
static int
init_shared_memory (void)
{
#if defined(HAVE_SYS_MMAN_H) && defined(HAVE_SHM_OPEN) && defined(HAVE_PTHREAD_H)
  int created = 0;
  struct stat st;

  /* Create or open shared memory object */
  shared_memory_fd = shm_open (SHARED_MEMORY_NAME, O_CREAT | O_RDWR, 0666);
  if (shared_memory_fd == -1)
    {
      perror ("shm_open");
      return -1;
    }

  /* Check if we need to set the size (first process) */
  if (fstat (shared_memory_fd, &st) == -1 || st.st_size == 0)
    {
      if (ftruncate (shared_memory_fd, sizeof (struct shared_memory_data)) == -1)
        {
          perror ("ftruncate");
          close (shared_memory_fd);
          return -1;
        }
      created = 1;
    }

  /* Map shared memory */
  shared_data = mmap (NULL, sizeof (struct shared_memory_data),
                     PROT_READ | PROT_WRITE, MAP_SHARED, shared_memory_fd, 0);
  if (shared_data == MAP_FAILED)
    {
      perror ("mmap");
      close (shared_memory_fd);
      return -1;
    }

  /* Initialize or reset all shared memory data at top-level make */
  if (makelevel == 0)
    {
      pthread_mutexattr_t mutex_attr;
      /* Fully zero/clear most shared memory except the mutex itself */
      memset(shared_data, 0, sizeof(struct shared_memory_data));

      if (created)
        {
          /* Initialize mutex on newly created shmem */
          pthread_mutexattr_init (&mutex_attr);
          pthread_mutexattr_setpshared (&mutex_attr, PTHREAD_PROCESS_SHARED);
          pthread_mutex_init (&shared_data->reserved_count_mutex, &mutex_attr);
          pthread_mutex_init (&shared_data->total_reserved_mb_mutex, &mutex_attr);
          pthread_mutexattr_destroy (&mutex_attr);
          debug_write(MEM_DEBUG_INFO, "[DEBUG] Created NEW shared memory: all fields zeroed (PID=%d, makelevel=%u)\n", getpid(), makelevel);
        }
      else
        {
          /* The mutex should already be constructed if reusing, do not re-init it */
          debug_write(MEM_DEBUG_INFO, "[DEBUG] Top-level make: fully zeroed shared memory to prevent stale data (PID=%d, makelevel=%u)\n", getpid(), makelevel);
        }
    }

  return 0;
#else
  return -1; /* Not available on this platform */
#endif
}

/* Cleanup shared memory */
static void
cleanup_shared_memory (void)
{
#if defined(HAVE_SYS_MMAN_H) && defined(HAVE_SHM_OPEN) && defined(HAVE_PTHREAD_H)
  /* Safety check: only run in main make process */
  if (makelevel > 0)
    {
      debug_write(MEM_DEBUG_ERROR, "[MEMORY] WARNING: cleanup_shared_memory() called in sub-make (makelevel=%u), ignoring\n", makelevel);
      fflush (stderr);
      return;
    }

  debug_write(MEM_DEBUG_VERBOSE, "[DEBUG] cleanup_shared_memory() called (PID=%d, makelevel=%u)\n", getpid(), makelevel);

  if (shared_data != NULL && shared_data != MAP_FAILED)
    {
      munmap (shared_data, sizeof (struct shared_memory_data));
      shared_data = NULL;
    }

  if (shared_memory_fd != -1)
    {
      close (shared_memory_fd);
      shared_memory_fd = -1;
    }

  /* Remove the shared memory object from the system */
  /* Only try to unlink if we actually created shared memory */
  if (shared_memory_fd != -1) {
    if (shm_unlink (SHARED_MEMORY_NAME) == -1) {
      /* Only report error if it's not "No such file" (ENOENT) */
      if (errno != ENOENT) {
        perror ("shm_unlink");
      } else {
        debug_write(MEM_DEBUG_VERBOSE, "[DEBUG] Shared memory object %s not found (already cleaned up)\n", SHARED_MEMORY_NAME);
      }
    } else {
      debug_write(MEM_DEBUG_VERBOSE, "[DEBUG] Successfully removed shared memory object: %s\n", SHARED_MEMORY_NAME);
    }
  } else {
    debug_write(MEM_DEBUG_VERBOSE, "[DEBUG] No shared memory to clean up (never created)\n");
  }
#endif
}

/* Save memory profiles to cache file (main make only) */
void
save_memory_profiles (void)
{
  FILE *f;
  unsigned int i;

  /* Safety check: only run in main make process */
  if (makelevel > 0)
    {
      debug_write(MEM_DEBUG_ERROR, "[MEMORY] WARNING: save_memory_profiles() called in sub-make (makelevel=%u), ignoring\n", makelevel);
      fflush (stderr);
      return;
    }

  /* debug_write("[MEMORY] save_memory_profiles() called, profile_count=%u\n", memory_profile_count); */
  /* fflush (stderr); */

  /* Use atomic file replacement: write to temp file, then rename */
  f = fopen (".make_memory_cache.tmp", "w");
  if (!f)
    {
      debug_write(MEM_DEBUG_ERROR, "[MEMORY] ERROR: Failed to open .make_memory_cache for writing\n");
      fflush (stderr);
      return;
    }

  if (memory_profiles == NULL) {
    debug_write(MEM_DEBUG_ERROR, "[MEMORY] ERROR: save_memory_profiles called but memory_profiles is NULL (count=%u)\n",
                memory_profile_count);
    fclose (f);
    return;
  }

  for (i = 0; i < memory_profile_count; i++) {
    /* Skip entries with peak_memory_mb=0 to avoid saving useless data */
    if (memory_profiles[i].peak_memory_mb == 0) continue;

    fprintf (f, "%lu %ld %s\n",
            memory_profiles[i].peak_memory_mb,
            (long)memory_profiles[i].last_used,
            memory_profiles[i].filename);
  }

  fclose (f);

  /* Atomic file replacement: rename temp file (automatically replaces existing file) */
  if (rename (".make_memory_cache.tmp", ".make_memory_cache") == -1) {
    perror ("rename .make_memory_cache.tmp");
    debug_write(MEM_DEBUG_ERROR, "[MEMORY] ERROR: Failed to rename temp file to cache file\n");
  }

  /* debug_write("[MEMORY] Saved %u profiles to .make_memory_cache\n", memory_profile_count); */
  /* fflush (stderr); */
}

/* Get imminent memory usage (called from job.c before forking) */
unsigned long
get_imminent_memory_mb (void)
{
#if defined(HAVE_SYS_MMAN_H) && defined(HAVE_SHM_OPEN) && defined(HAVE_PTHREAD_H)
  unsigned long reserved_mb = 0;
  unsigned long unused_peaks_mb = 0;

  /* Initialize shared memory if not already done (only if memory monitoring is enabled) */
  if (memory_aware_flag && shared_data == NULL)
    {
      if (init_shared_memory () != 0)
        return 0; /* Fallback to no imminent memory if shared memory fails */
    }

  /* Thread-safe access to shared memory values */
  if (shared_data)
    {
      //pthread_mutex_lock (&shared_data->imminent_mutex);
      reserved_mb = shared_data->total_reserved_mb;
      unused_peaks_mb = shared_data->unused_peaks_mb;
      //pthread_mutex_unlock (&shared_data->imminent_mutex);
    }

  return reserved_mb + unused_peaks_mb;
#else
  return 0; /* Not available on this platform */
#endif
}

unsigned long
get_memory_stats (unsigned int *percent)
{
#ifdef __linux__
  FILE *meminfo;
  unsigned long total_kb = 0, avail_kb = 0;
  char line[256];

  if (percent)
    *percent = 0;

  meminfo = fopen ("/proc/meminfo", "r");
  if (!meminfo)
    return 0;

  while (fgets (line, sizeof (line), meminfo))
    {
      if (sscanf (line, "MemTotal: %lu", &total_kb) == 1)
        continue;
      if (sscanf (line, "MemAvailable: %lu", &avail_kb) == 1)
        break;
    }

  fclose (meminfo);

  if (avail_kb > 0)
    {
      if (percent && total_kb > 0)
        *percent = (unsigned int)(100 - (avail_kb * 100 / total_kb));
      return avail_kb / 1024;
    }
#endif
  return 0;
}

/* Reserve or release memory for a process
   mb: memory amount in MB (positive value to reserve, 0 or negative to release)
   If mb <= 0, sets reserved_mb to 0.
   If mb < 0, returns 1 (true) if the negative value cancelled out the existing value.
   The new value simply overwrites the old value for this PID's reservation slot.
   Uses the provided PID (child PID for jobs, make PID for sub-makes) to track reservations */
int
reserve_memory_mb (pid_t pid, long mb, const char *filepath)
{
#if defined(HAVE_SYS_MMAN_H) && defined(HAVE_SHM_OPEN) && defined(HAVE_PTHREAD_H)
  unsigned long old_value;
  unsigned long old_total_reserved;
  unsigned long new_total_reserved;
  struct pid_reservation *res = NULL;
  int i, count;
  int empty_slot = -1;

  if (memory_aware_flag && shared_data == NULL) {
      if (init_shared_memory () != 0)
        return 0; /* Fallback if shared memory fails */
  }

  if (!shared_data) {
    debug_write(MEM_DEBUG_ERROR, "[DEBUG] Shared memory not initialized, cannot reserve/release memory\n");
    return 0;
  }

  //pthread_mutex_lock (&shared_data->imminent_mutex);

  /* Find or create reservation entry for this PID */
  count = shared_data->reservation_count;

  /* First, look for existing entry for this PID (only check up to actual count) */
  for (i = 0; i < count && i < MAX_RESERVATIONS; i++)
    if (shared_data->reservations[i].pid == pid) {
      res = &shared_data->reservations[i];
      break;
    }

  /* If not found, need to find or create a slot */
  if (!res) {
    if (mb == 0) return 0; // Nothing to do - already released
    pthread_mutex_lock (&shared_data->reserved_count_mutex);
    count = shared_data->reservation_count;

    /* Look for empty slot */
    for (i = 0; i < MAX_RESERVATIONS; i++)
      if (shared_data->reservations[i].pid == 0) {
        empty_slot = i;
        break;
      }

    /* No existing entry, use empty slot if available */
    if (empty_slot >= 0) {
      res = &shared_data->reservations[empty_slot];
      res->pid = pid;
      res->reserved_mb = 0;
      /* Update count if we're adding at the end */
      if (empty_slot >= count) shared_data->reservation_count = empty_slot + 1;
    } else {
      /* No empty slot available - report error */
      pthread_mutex_unlock (&shared_data->reserved_count_mutex);
      debug_write(MEM_DEBUG_ERROR, "[MEMORY] ERROR: No available reservation slots (MAX_RESERVATIONS=%d exceeded). Cannot track memory for PID=%d\n",
                  MAX_RESERVATIONS, (int)pid);
      return 0;
    }
    pthread_mutex_unlock (&shared_data->reserved_count_mutex);
  }

  old_value = res->reserved_mb;

  /* If mb <= 0, zero out the existing value */
  if (mb <= 0) {
    pthread_mutex_lock (&shared_data->total_reserved_mb_mutex);
    old_total_reserved = shared_data->total_reserved_mb;
    shared_data->total_reserved_mb -= old_value;
    new_total_reserved = shared_data->total_reserved_mb;
    pthread_mutex_unlock (&shared_data->total_reserved_mb_mutex);
    res->reserved_mb = 0;
    /* If releasing (mb <= 0), clear the PID to mark the slot as free for reuse */
    if (res->pid == pid) {
      res->pid = 0;
      debug_write(MEM_DEBUG_VERBOSE, "[MEMORY] Freed reservation slot for PID=%d (slot can be reused), total_reserved_mb: %luMB -> %luMB (-%luMB)\n",
            (int)pid, old_total_reserved, new_total_reserved, old_value);
    }
    /* Return 1 if negative value cancelled out the existing value */
    return (mb == 0 || (mb < 0 && old_value == (unsigned long)(-mb))) ? 1 : 0;
  }

  /* Positive value: overwrite with new value */
  res->reserved_mb = (unsigned long)mb;
  pthread_mutex_lock (&shared_data->total_reserved_mb_mutex);
  old_total_reserved = shared_data->total_reserved_mb;
  if ((unsigned long)mb >= old_value) {
    shared_data->total_reserved_mb += ((unsigned long)mb - old_value);
  } else {
    /* Avoid underflow if old_value > new mb */
    if (shared_data->total_reserved_mb < (old_value - (unsigned long)mb))
      shared_data->total_reserved_mb = 0;
    else
      shared_data->total_reserved_mb -= (old_value - (unsigned long)mb);
  }
  new_total_reserved = shared_data->total_reserved_mb;
  pthread_mutex_unlock (&shared_data->total_reserved_mb_mutex);

  //pthread_mutex_unlock (&shared_data->imminent_mutex);
  debug_write(MEM_DEBUG_INFO, "[MEMORY] Reserved memory[%d]: %luMB -> %luMB for %s (PID=%d, makelevel=%u), total_reserved_mb: %luMB -> %luMB\n",
       i, old_value, (unsigned long)mb, filepath ? filepath : "?", (int)pid, makelevel, old_total_reserved, new_total_reserved);
  return 0;
#else
  return 0;
#endif
}

/* Forward declaration for non-blocking debug output */
#ifdef HAVE_PTHREAD_H
void debug_write (int log_level, const char *format, ...);
static void write_monitor_debug_file (const char *function_name, int saved_errno);
static void reset_terminal_state (void);
static void terminal_cleanup_atexit (void);

/* Cached terminal width - set once at monitor start, NEVER query ioctl() from thread! */
static int cached_term_width = 0;

/* Monitor thread's private non-blocking stderr fd (dup of STDERR_FILENO) */
static int monitor_stderr_fd = -1;
#endif

#ifdef HAVE_PTHREAD_H
static void
display_memory_status (unsigned int mem_percent, unsigned long free_mb, int force, unsigned int total_jobs, unsigned long make_usage_mb, unsigned long imminent_mb)
{
  static struct timeval last_display = {0, 0};
  struct timeval now;
  long elapsed_ms;
  const char *green = "\033[1;32m";
  const char *purple = "\033[1;35m";
  const char *gray = "\033[0;90m";
  const char *white = "\033[1;37m";
  const char *reset = "\033[0m";
  const char *spinners[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
  const char *spinner;
  char bar[256];
  int bar_len = 20;
  size_t pos = 0;
  size_t i;
  char status[512];
  int term_width;
  int visible_len = 50;  /* Adjusted for "X jobs" display */
  int col_pos;
  static int write_count = 0;
  static int skip_count = 0;
  unsigned long total_mb;
  int make_filled, imminent_filled;
  int other_filled, free_filled;
  char output_buf[1024];
  int output_len;
  ssize_t written;
  char debug_msg[128];
  int debug_len;
  int total_used_filled;

  if (!memory_aware_flag || disable_memory_display)
    return;

  gettimeofday(&now, NULL);

  /* Update status line every 300ms for smooth spinner */
  if (!force) {
    elapsed_ms = (now.tv_sec - last_display.tv_sec) * 1000 +
                 (now.tv_usec - last_display.tv_usec) / 1000;
    if (elapsed_ms < 300) {
      skip_count++;
      /* Debug if we're skipping a LOT */
      if (skip_count % 100 == 0) {
        debug_len = snprintf(debug_msg, sizeof(debug_msg), "[SKIP%d:elapsed=%ldms]", skip_count, elapsed_ms);
        written = write(STDERR_FILENO, debug_msg, debug_len);
        (void)written;
      }
      return;
    }
    skip_count = 0;  /* Reset when we do display */
  }

  last_display = now;

  /* Spinner to show we're alive */
  spinner = spinners[spinner_state % 10];
  spinner_state++;

  /* Get total memory */
  total_mb = free_mb / (100 - mem_percent) * 100;

  /* Build multi-color memory bar (20 chars) */
  /* Bar shows: [make memory in purple][other used in green][imminent in yellow][free in gray] */
  make_filled = (make_usage_mb * bar_len) / total_mb;
  imminent_filled = (imminent_mb * bar_len) / total_mb;

  /* Calculate other_filled and ensure it's not negative */
  total_used_filled = (mem_percent * bar_len) / 100;
  other_filled = total_used_filled - make_filled;
  if (other_filled < 0) other_filled = 0;

  free_filled = bar_len - make_filled - other_filled - imminent_filled;
  if (free_filled < 0) free_filled = 0;

  /* Purple: make-tracked memory */
  if (make_filled > 0) {
    pos += snprintf(bar + pos, sizeof(bar) - pos, "%s", purple);
    for (i = 0; i < (size_t)make_filled && pos < sizeof(bar) - 10; i++)
      pos += snprintf(bar + pos, sizeof(bar) - pos, "█");
  }

  /* Green: other used memory */
  if (other_filled > 0) {
    pos += snprintf(bar + pos, sizeof(bar) - pos, "%s", green);
    for (i = 0; i < (size_t)other_filled && pos < sizeof(bar) - 10; i++)
      pos += snprintf(bar + pos, sizeof(bar) - pos, "█");
  }

  /* Yellow: imminent memory */
  if (imminent_filled > 0) {
    pos += snprintf(bar + pos, sizeof(bar) - pos, "\033[1;33m");
    for (i = 0; i < (size_t)imminent_filled && pos < sizeof(bar) - 10; i++)
      pos += snprintf(bar + pos, sizeof(bar) - pos, "░");
  }

  /* Gray: free memory */
  if (free_filled > 0) {
    pos += snprintf(bar + pos, sizeof(bar) - pos, "%s", gray);
    for (i = 0; i < (size_t)free_filled && pos < sizeof(bar) - 10; i++)
      pos += snprintf(bar + pos, sizeof(bar) - pos, "░");
  }

  /* Reset color */
  snprintf(bar + pos, sizeof(bar) - pos, "%s", reset);

  /* Build the status string - show total job count */
  /* Use the total_jobs count we already calculated (no need to scan /proc again) */

  snprintf(status, sizeof(status), "%s%s %s%u%%%s %s(%luMB)%s %s%u jobs%s",
            spinner, bar, white, mem_percent, reset, gray, free_mb, reset,
            gray, total_jobs, reset);

  /* Use cached terminal width - NEVER ioctl() from thread (it blocks!) */
  term_width = cached_term_width;

  /* Calculate position (right-aligned with 2 char margin) */
  /* Note: actual visible length is less than total due to ANSI codes */
  col_pos = term_width - visible_len;
  if (col_pos < 1)
    col_pos = 1;

  /* Move up one line, save cursor, move to right side, display, restore, move down */
  /* This makes the status appear on the line ABOVE the current compilation message */
  /* Use write() for unbuffered, lock-free output - bypasses stdio locking! */
  /* CRITICAL: Only use cursor save/restore if stderr AND stdout are TTYs - avoid corruption when piped */
  if (isatty(STDERR_FILENO) && isatty(STDOUT_FILENO)) {
    output_len = snprintf(output_buf, sizeof(output_buf), "\033[A\033[s\033[%dG%s\033[u\033[B", col_pos, status);
  } else {
    /* Not a TTY or being piped - just use simple newline to avoid corrupting piped output */
    output_len = snprintf(output_buf, sizeof(output_buf), "%s\n", status);
  }

  if (output_len > 0 && output_len < (int)sizeof(output_buf)) {
    write_count++;

    /* Debug EVERY write for now to see gaps */
#if DEBUG_MEMORY_MONITOR
    debug_len = snprintf(debug_msg, sizeof(debug_msg), "[W%d]", write_count);
    written = write(monitor_stderr_fd >= 0 ? monitor_stderr_fd : STDERR_FILENO, debug_msg, debug_len);
    (void)written;
#endif

    /* write() to monitor's private non-blocking fd - never blocks! */
    written = write(monitor_stderr_fd >= 0 ? monitor_stderr_fd : STDERR_FILENO, output_buf, output_len);
    if (written < 0 && (errno == EPIPE || errno == EBADF)) {
      int saved_errno = errno;
      write_monitor_debug_file ("display_memory_status (broken pipe detected)", saved_errno);
      /* Pipe broken (e.g., less exited) - reset terminal and stop monitoring */
      reset_terminal_state ();
      status_line_shown = 0;
      monitor_thread_running = 0;  /* Stop the monitor thread */
      write_monitor_debug_file ("display_memory_status (monitor stopped)", saved_errno);
      return;
    }
    status_line_shown = 1;
  }
}
#endif // HAVE_PTHREAD_H

/* Helper function to find descendants of a child process by scanning only processes with this as parent */
static unsigned long find_child_descendants(pid_t parent_pid, int depth, int parent_idx, unsigned int *total_jobs,
                                            unsigned long *unused_peaks_mb)
{
  DIR *proc_dir;
  struct dirent *entry;
  char stat_path[512];
  FILE *stat_file;
  char line[512];
  unsigned long total_rss_kb = 0;
  unsigned int i;
  char *cmdline;  /* Cmdline for current PID being processed */
  int term_width = cached_term_width > 0 ? cached_term_width : 80;
  size_t max_cmdline_len = term_width > 100 ? (size_t)(term_width - 100) : 20;  /* Leave room for message prefix, min 20 */

  //debug_write("[EXTRA] run find_child_descendants parent_pid=%d (depth=%d, parent_idx=%d)\n", (int)parent_pid, depth, parent_idx);

  proc_dir = opendir("/proc");
  if (!proc_dir) {
    debug_write(MEM_DEBUG_ERROR, "[ERROR] Failed to open /proc directory\n");
    return 0;
  }

  while ((entry = readdir(proc_dir)) != NULL) {
    int new_descendant = 0;
    int descendant_idx = -1;
    int found_ppidx = parent_idx;
    int send_idx;
    char *strip_ptr = NULL;
    unsigned long rss_kb = 0;
    unsigned long profile_peak_mb = 0;
    pid_t pid, check_pid = 0;
    int profile_idx = -1;
    unsigned long child_rss_kb = 0;
    unsigned int child_jobs = 0;
    int found_ppid = 0, found_vmrss = 0;

    cmdline = NULL;  /* Reset cmdline for each PID */

    /* Skip non-numeric entries */
    if (!isdigit(entry->d_name[0])) continue;

    pid = atoi(entry->d_name);
    if (pid <= 0) continue;

    /* Read this process's status to check if it's a descendant */
    snprintf(stat_path, sizeof(stat_path), "/proc/%d/status", (int)pid);
    stat_file = fopen(stat_path, "r");
    if (!stat_file) continue;

    /* Track whether we've successfully parsed each field */
    while (fgets(line, sizeof(line), stat_file)) {
      if (!found_ppid && sscanf(line, "PPid: %d", &check_pid) == 1) {
        if (check_pid != parent_pid) break;
        found_ppid = 1;
      }
      else if (!found_vmrss && sscanf(line, "VmRSS: %lu kB", &rss_kb) == 1) {
        found_vmrss = 1;
      }

      /* Break when we've successfully parsed both fields */
      if (found_ppid && found_vmrss) break;
    }
    fclose(stat_file);

    /* Check if this process is a direct descendant of our parent */
    if (check_pid != parent_pid) continue;

    /* Found a descendant! Track its memory */

    total_rss_kb += rss_kb; // Add this descendant's RSS to the total
    // Do we already know about this descendant?
    for (i = 0; i < main_monitoring_data.compile_count; i++) {
      if (main_monitoring_data.descendants[i].pid == parent_pid) {
        found_ppidx = main_monitoring_data.descendants[i].profile_idx;
        /*debug_write("[DEBUG] PID=%d PPID=%d (d:%d) Found parent descendant[%d] ppidx=%d found_ppidx=%d (file: %s)\n", (int)pid, (int)parent_pid, depth, i,
                    parent_idx, found_ppidx, found_ppidx >= 0 ? memory_profiles[found_ppidx].filename : "unknown");*/
      }
      if (main_monitoring_data.descendants[i].pid == pid) {
        descendant_idx = i;
        profile_idx = main_monitoring_data.descendants[i].profile_idx;
        if (parent_idx != found_ppidx)
          debug_write(MEM_DEBUG_MAX, "[DEBUG] Found existing descendant[%d] ppidx=%d fppidx=%d PID=%d PPID=%d (d:%d): old_peak=%luMB, rss=%luMB current_mb=%luMB peak=%luMB (file: %s)\n",
                    i, parent_idx, found_ppidx, (int)pid, (int)parent_pid, depth, main_monitoring_data.descendants[i].old_peak_mb, rss_kb / 1024,
                    main_monitoring_data.descendants[i].current_mb, main_monitoring_data.descendants[i].peak_mb,
                    profile_idx >= 0 ? memory_profiles[profile_idx].filename : "");
      }
      if (descendant_idx >= 0 && found_ppidx >= 0) break;
    }
    if (descendant_idx < 0 && parent_idx != found_ppidx) {
      debug_write(MEM_DEBUG_INFO, "[DEBUG] PID=%d PPID=%d (d:%d) Parent index mismatch: parent_idx=%d != found_ppidx=%d\n", (int)pid, (int)parent_pid, depth, parent_idx, found_ppidx);
      parent_idx = found_ppidx; // No idea why we're having to do this!! (done after debugging above so we capture the oddity)
    }

    if (descendant_idx < 0 && parent_idx < 0) { // We'll need to track this new descendant
      if (main_monitoring_data.compile_count >= MAX_TRACKED_DESCENDANTS) {
        debug_write(MEM_DEBUG_ERROR, "[DEBUG] Max tracked descendants reached, skipping descendant PID %d\n", (int)pid);
        continue;
      }

      // Extract cmdline to see if we need to track it
      strip_ptr = extract_filename_from_cmdline(pid, parent_pid, depth, "main", &cmdline, max_cmdline_len);

      if (strip_ptr) {
        /* Look up memory profile for this filename */
        if (memory_profiles != NULL) {
          for (i = 0; i < memory_profile_count; i++) {
            if (memory_profiles[i].filename && strcmp(memory_profiles[i].filename, strip_ptr) == 0) {
              profile_peak_mb = memory_profiles[i].peak_memory_mb;
              profile_idx = i;  /* Remember the profile index */
              //debug_write("[DEBUG] PID=%d (d:%d) Found memory profile for %s: %uMB\n", (int)pid, depth, strip_ptr, profile_peak_mb);
              break;
            }
          }
        } else if (memory_profile_count > 0) {
          /* Error: we have profiles but array not allocated */
          debug_write(MEM_DEBUG_ERROR, "[MEMORY] ERROR: memory_profiles is NULL but memory_profile_count=%u (PID=%d)\n",
                      memory_profile_count, (int)getpid());
        }
        if (profile_idx < 0) {
          /* Grow array if needed */
          if (memory_profile_count >= memory_profiles_capacity) {
            grow_memory_profiles();
            /* If growth failed, capacity will still be 0 or less than count */
            if (memory_profile_count >= memory_profiles_capacity) {
              static int memory_profile_error_count = 0;
              if (memory_profile_error_count < 10) {
                debug_write(MEM_DEBUG_ERROR, "[DEBUG] PID=%d (d:%d) Failed to grow memory_profiles, cannot create profile for '%s'\n", (int)pid, depth, strip_ptr);
                memory_profile_error_count++;
              }
            }
          }

          if (memory_profile_count < memory_profiles_capacity) {
            memory_profiles[memory_profile_count].filename = xstrdup(strip_ptr);
            memory_profiles[memory_profile_count].peak_memory_mb = rss_kb / 1024;
            memory_profiles[memory_profile_count].last_used = time(NULL);
            profile_idx = memory_profile_count;  /* Set the profile index for this new profile */
            memory_profile_count++;
            debug_write(MEM_DEBUG_VERBOSE, "[MEMORY] Added new profile %s: %luMB, profile_count=%u\n",
                       strip_ptr, 0, memory_profile_count);
            fflush(stderr);
          }
        }
      } // if strip_ptr
    } // if new descendant and parent not tracked

    if (descendant_idx < 0) {
      if (parent_idx < 0) {
        // Add new entry for this compilation process
        if (main_monitoring_data.compile_count < MAX_TRACKED_DESCENDANTS) {
          int idx = main_monitoring_data.compile_count;
          descendant_idx = idx;
          main_monitoring_data.descendants[idx].pid = pid;
          main_monitoring_data.descendants[idx].current_mb = rss_kb / 1024;
          main_monitoring_data.descendants[idx].old_peak_mb = profile_peak_mb;
          main_monitoring_data.descendants[idx].profile_idx = profile_idx;
          main_monitoring_data.compile_count++;
          new_descendant = 1;

          if (strip_ptr)
            debug_write(MEM_DEBUG_VERBOSE, "[DEBUG] New descendant[%d] PID=%d PPID=%d (d:%d) pidx=%d ppidx=%d old_peak=%luMB rss=%luMB (file: %s)\n",
                        idx, (int)pid, (int)parent_pid, depth, profile_idx, parent_idx, profile_peak_mb, rss_kb / 1024, strip_ptr);
          else
            debug_write(MEM_DEBUG_VERBOSE, "[DEBUG] New descendant[%d] PID=%d PPID=%d (d:%d) pidx=%d ppidx=%d rss=%luMB (cmd: %s)\n",
                        idx, (int)pid, (int)parent_pid, depth, profile_idx, parent_idx, rss_kb / 1024, cmdline ? cmdline : "");
        } else debug_write(MEM_DEBUG_ERROR, "[DEBUG] Max tracked descendants reached, skipping descendant PID %d\n", (int)pid);
      } // TODO - we could "else" track related descendants (parent_idx >= 0) or other PIDs (profile_idx < 0) via another descendants-like struct (for debugging)

      if (profile_peak_mb > 0 && reserve_memory_mb(pid, -profile_peak_mb, strip_ptr))
          debug_write(MEM_DEBUG_VERBOSE, "[MEMORY] Released %luMB reservation for PID=%d (main make discovered descendant, using old_peak_mb)\n",
                profile_peak_mb, parent_pid);

      /* Free the filename and cmdline if we extracted them */
      if (strip_ptr) free(strip_ptr);
      if (cmdline) free(cmdline);
      strip_ptr = cmdline = NULL;
    } // descendant_idx < 0

    if (profile_idx >=0) send_idx = profile_idx;
    else send_idx = parent_idx;
    /* Recursively find descendants of this descendant */
    child_rss_kb = find_child_descendants(pid, depth + 1, send_idx, &child_jobs, unused_peaks_mb);
    total_rss_kb += child_rss_kb;
    if (total_jobs) *total_jobs += child_jobs;
    if (unused_peaks_mb) *unused_peaks_mb += (main_monitoring_data.descendants[descendant_idx].old_peak_mb > main_monitoring_data.descendants[descendant_idx].current_mb)
                          ? main_monitoring_data.descendants[descendant_idx].old_peak_mb - main_monitoring_data.descendants[descendant_idx].current_mb
                          : 0;

    if (descendant_idx >= 0 && profile_idx >= 0) {
      unsigned long new_current_mb = (rss_kb + child_rss_kb) / 1024;
      if (total_jobs) (*total_jobs)++; // Increment the total job count
      // Existing descendant - update memory tracking
      if (new_current_mb > main_monitoring_data.descendants[descendant_idx].current_mb || new_descendant) {
        debug_write(MEM_DEBUG_VERBOSE, "[DEBUG] Memory increase[%d] PID=%d PPID=%d (d:%d) %luMB -> %luMB (rss=%luMB child_rss=%luMB) child_jobs=%u (file: %s)\n",
                  descendant_idx, (int)pid, (int)parent_pid, depth, main_monitoring_data.descendants[descendant_idx].current_mb, new_current_mb,
                  rss_kb / 1024, child_rss_kb / 1024, child_jobs, profile_idx >= 0 ? memory_profiles[profile_idx].filename : "unknown");
        main_monitoring_data.descendants[descendant_idx].current_mb = new_current_mb;
        if (new_current_mb > main_monitoring_data.descendants[descendant_idx].peak_mb) {
          main_monitoring_data.descendants[descendant_idx].peak_mb = new_current_mb;
          record_file_memory_usage_by_index(profile_idx, new_current_mb, 0);
        }
      }
    }
  } // while reading /proc/PID/status

  closedir(proc_dir);

  return total_rss_kb;
}

/* Memory monitoring thread - runs independently! */
#ifdef HAVE_PTHREAD_H
static void *
memory_monitor_thread_func (void *arg)
{
  unsigned int mem_percent;
  unsigned long free_mb;

  //debug_write("[DEBUG] Memory monitor thread started (PID=%d)\n", (int)getpid());
#if DEBUG_MEMORY_MONITOR
  static time_t last_debug = 0;
  time_t now;
#endif

  (void)arg;
  monitor_start_time = time (NULL);

  /* Cache terminal width ONCE before setting non-blocking (ioctl can block!) */
  /* Get terminal width safely by saving/restoring terminal state */
#ifdef HAVE_SYS_IOCTL_H
  {
    struct winsize w;
    struct termios saved_termios;

    /* Save current terminal state if stderr is a TTY */
    int saved_termios_ok = isatty(STDERR_FILENO) && (tcgetattr(STDERR_FILENO, &saved_termios) == 0);

    /* Get terminal window size */
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
      cached_term_width = w.ws_col;
    }

    /* Restore terminal state if we saved it */
    if (saved_termios_ok) {
      tcsetattr(STDERR_FILENO, TCSANOW, &saved_termios);
    }

    /* If we couldn't get terminal width, disable memory display */
    if (!cached_term_width) {
      disable_memory_display = 1;
      debug_write(MEM_DEBUG_INFO, "[MONITOR] Could not obtain terminal width, disabling memory display\n");
    }
  }
#else
  /* No ioctl support, disable memory display */
  disable_memory_display = 1;
  debug_write(MEM_DEBUG_INFO, "[MONITOR] No ioctl support, disabling memory display\n");
#endif

  /* Use dup() to create private fd for monitor thread
   * NOTE: We do NOT set O_NONBLOCK because that flag is shared between the original
   * and dup'd fd (they point to the same file description), which would break echo/printf
   * in child processes with "write error". Instead, we rely on write() being fast. */
  monitor_stderr_fd = dup(STDERR_FILENO);
  if (monitor_stderr_fd >= 0) {
    debug_write(MEM_DEBUG_INFO, "[MONITOR] Using private fd=%d (dup of stderr=%d), term_width=%d, isatty(stderr)=%d, isatty(stdout)=%d\n",
                monitor_stderr_fd, STDERR_FILENO, cached_term_width, isatty(STDERR_FILENO), isatty(STDOUT_FILENO));
  } else {
    debug_write(MEM_DEBUG_INFO, "[ERROR] Failed to dup() stderr, monitor will use STDERR_FILENO\n");
  }

  while (monitor_thread_running) {
    unsigned long total_unused_peaks_mb = 0;
    unsigned long total_reserved_mb = 0;
    unsigned long total_imminent_mb = 0;
    unsigned int total_make_mem = 0;
    unsigned int total_jobs = 0;
    static unsigned int last_total_make_mem = 0;
    static unsigned int last_total_jobs = 0;
    static time_t last_save_time = 0;

    /* Sleep 100ms between each check for accurate process memory tracking */
    usleep(100000);

    free_mb = get_memory_stats(&mem_percent); // TODO get total_mem instead of percent

    if (mem_percent == 0) {
      debug_write(MEM_DEBUG_ERROR, "[ERROR] Could not determine memory usage!\n");
      return NULL;
    }

    /* Update peak memory by finding descendants starting from our PID */
    total_make_mem = find_child_descendants(getpid(), 0, -1, &total_jobs, &total_unused_peaks_mb) / 1024;
    if (total_make_mem != last_total_make_mem || total_jobs != last_total_jobs) {
      debug_write(MEM_DEBUG_VERBOSE, "[DEBUG] Total jobs found: %u, total make memory: %uMB\n", total_jobs, total_make_mem);
      last_total_make_mem = total_make_mem;
      last_total_jobs = total_jobs;
    }

    /* Check for exited descendants and calculate total current usage in one loop */
    for (unsigned int i = 0; i < main_monitoring_data.compile_count; i++) {
      char stat_path[512];
      FILE *stat_file;

      /* Check if this PID still exists */
      snprintf(stat_path, sizeof(stat_path), "/proc/%d/status", (int)main_monitoring_data.descendants[i].pid);
      stat_file = fopen(stat_path, "r");
      if (!stat_file) {
        /* Process exited - record final memory and release reservation */
        int profile_idx = main_monitoring_data.descendants[i].profile_idx;
        if (profile_idx >= 0 && (main_monitoring_data.descendants[i].peak_mb > 0 || main_monitoring_data.descendants[i].old_peak_mb > 0)) {
          debug_write(MEM_DEBUG_INFO, "[MEMORY] PID=%d Compilation exited, final peak for %s: %luMB -> %luMB\n",
                      (int)main_monitoring_data.descendants[i].pid, memory_profiles[profile_idx].filename,
                      main_monitoring_data.descendants[i].old_peak_mb, main_monitoring_data.descendants[i].peak_mb);

          /* Record final memory usage for disk operations using direct profile update */
          record_file_memory_usage_by_index(profile_idx, main_monitoring_data.descendants[i].peak_mb, 1);  /* final=1 */
        }

        /* Remove this entry by shifting remaining entries */
        if (i < main_monitoring_data.compile_count - 1)
          memmove(&main_monitoring_data.descendants[i], &main_monitoring_data.descendants[i + 1],
                  (main_monitoring_data.compile_count - i - 1) * sizeof(main_monitoring_data.descendants[0]));
        main_monitoring_data.compile_count--;
        i--;  /* Check this index again since we shifted */
      } else fclose(stat_file);
    }

#if defined(HAVE_SYS_MMAN_H) && defined(HAVE_SHM_OPEN) && defined(HAVE_PTHREAD_H)
    if (shared_data) {
      int count;
      unsigned int i;
      //pthread_mutex_lock (&shared_data->imminent_mutex);
      shared_data->unused_peaks_mb = total_unused_peaks_mb;
      count = shared_data->reservation_count;
      for (i = 0; i < (unsigned int)count; i++) {
        unsigned long res_mb = shared_data->reservations[i].reserved_mb;
        total_reserved_mb += res_mb;
        if (res_mb > 0) {
          debug_write(MEM_DEBUG_VERBOSE, "[DEBUG_SUM] reservation[%u]: PID=%d reserved_mb=%lu (total now=%lu)\n",
                      i, (int)shared_data->reservations[i].pid, res_mb, total_reserved_mb);
        }
      }
      /* Check if total_reserved_mb matches shared_data->total_reserved_mb, warn if not */
      if (shared_data->total_reserved_mb != total_reserved_mb) {
        debug_write(MEM_DEBUG_ERROR, "[MEMORY] WARNING: Calculated total_reserved_mb=%lu does not match shared_data->total_reserved_mb=%lu\n",
                    total_reserved_mb, shared_data->total_reserved_mb);
      }
      /* Update shared memory with total current usage (calculated in the loop above) */
      //shared_data->total_reserved_mb = total_reserved_mb;
      //pthread_mutex_unlock (&shared_data->imminent_mutex);
    }
#endif

    /* Save memory profiles if they've been updated (check shared dirty flag)
     * Rate limit to once every 10 seconds */
    if (memory_profiles_dirty) {
      time_t current_time = time(NULL);
      if (current_time - last_save_time >= 10) {
        debug_write(MEM_DEBUG_VERBOSE, "[MEMORY] Dirty flag detected, saving profiles...\n");
        save_memory_profiles();
        last_save_time = current_time;
        memory_profiles_dirty = 0;
        /* debug_write ("[MEMORY] Profiles saved, dirty flag cleared\n"); */
      }
    }

    // Update status display
    total_imminent_mb = total_reserved_mb + total_unused_peaks_mb;
    display_memory_status (mem_percent, free_mb, 0, total_jobs, total_make_mem, total_imminent_mb);

  } /* while (monitor_thread_running) */

  /* If we exit the loop, show why */
#if DEBUG_MEMORY_MONITOR
  debug_write ("[THREAD_EXIT] Loop exited, monitor_thread_running=%d\n", monitor_thread_running);
#endif

  /* Close our private stderr fd */
  if (monitor_stderr_fd >= 0) {
    close (monitor_stderr_fd);
    monitor_stderr_fd = -1;
  }

  return NULL;
}
#endif

/* Start the memory monitoring thread */
#ifdef HAVE_PTHREAD_H
static void
start_memory_monitor (void)
{
  static int already_started = 0;

  if (!memory_aware_flag)
    return;

  /* Only top-level make should monitor (not recursive makes) */
  if (makelevel > 0)
    return;

  /* Only start once */
  if (already_started)
    return;
  already_started = 1;

  monitor_thread_running = 1;

  /* Load existing memory profiles from cache */
  load_memory_profiles ();

  /* Register terminal cleanup function for atexit() - runs when process exits */
  atexit(terminal_cleanup_atexit);

  //debug_write("[DEBUG] Starting memory monitor thread (PID=%d)\n", (int)getpid());

#ifdef HAVE_PTHREAD_H
  if (pthread_create (&memory_monitor_thread, NULL, memory_monitor_thread_func, NULL) != 0)
    {
      O (error, NILF, _("Failed to create memory monitor thread"));
      memory_aware_flag = 0;
    }
#else
  O (error, NILF, _("Memory monitoring requires pthread support"));
  memory_aware_flag = 0;
#endif
}
#endif

/* Reset terminal state - tries multiple methods to ensure it works */
static void
reset_terminal_state (void)
{
  const char reset_seq[] = "\r\033[K\n";  /* Reset cursor, clear line, newline - simple and reliable */
  ssize_t written;
  int tty_fd = -1;

  /* Try the monitor's stderr fd first if available */
  if (monitor_stderr_fd >= 0) {
    written = write(monitor_stderr_fd, reset_seq, sizeof(reset_seq) - 1);
    if (written >= 0) {
      write_monitor_debug_file ("reset_terminal_state (monitor_stderr_fd success)", 0);
      return;
    }
    write_monitor_debug_file ("reset_terminal_state (monitor_stderr_fd failed)", errno);
  }

  /* Fallback: try /dev/tty directly */
  tty_fd = open("/dev/tty", O_WRONLY);
  if (tty_fd >= 0) {
    written = write(tty_fd, reset_seq, sizeof(reset_seq) - 1);
    close(tty_fd);
    if (written >= 0) {
      write_monitor_debug_file ("reset_terminal_state (/dev/tty success)", 0);
      return;
    }
    write_monitor_debug_file ("reset_terminal_state (/dev/tty failed)", errno);
  } else {
    write_monitor_debug_file ("reset_terminal_state (/dev/tty open failed)", errno);
  }

  /* Last resort: try stderr */
  written = write(STDERR_FILENO, reset_seq, sizeof(reset_seq) - 1);
  if (written >= 0) {
    write_monitor_debug_file ("reset_terminal_state (STDERR_FILENO success)", 0);
  } else {
    write_monitor_debug_file ("reset_terminal_state (STDERR_FILENO failed)", errno);
  }
}

/* Terminal cleanup function for atexit() - runs when process exits */
static void
terminal_cleanup_atexit (void)
{
  /* Only try to reset if we were showing status lines AND both stdout/stderr are TTYs */
  if (status_line_shown && isatty(STDERR_FILENO) && isatty(STDOUT_FILENO)) {
    write_monitor_debug_file ("terminal_cleanup_atexit", errno);
    reset_terminal_state();
  }
}

/* Write debug info to file (works even when stderr is broken) */
static void
write_monitor_debug_file (const char *function_name, int saved_errno)
{
  char debug_filename[64];
  FILE *debug_file;

  snprintf(debug_filename, sizeof(debug_filename), "/tmp/make_monitor_debug_%d.txt", (int)getpid());
  debug_file = fopen (debug_filename, "a");
  if (debug_file) {
      time_t now = time(NULL);
      struct tm *tm_info = localtime(&now);
      char buf[16];
      if (tm_info) strftime(buf, sizeof(buf), "%H:%M:%S", tm_info);
      else strncpy(buf, "??:??:??", sizeof(buf));
      fprintf (debug_file, "[%s] %s called: PID=%d (PPID=%d), makelevel=%u, errno=%d (%s), status_line_shown=%d, monitor_thread_running=%d, isatty(stderr)=%d, isatty(stdout)=%d\n",
               buf, function_name, (int)getpid(), (int)getppid(), makelevel,
               saved_errno, saved_errno ? strerror(saved_errno) : "0",
               status_line_shown, monitor_thread_running, isatty(STDERR_FILENO), isatty(STDOUT_FILENO));
    fclose (debug_file);
  }
}

/* Stop the memory monitoring thread */
#ifdef HAVE_PTHREAD_H
void
stop_memory_monitor (int immediate)
{
  int saved_errno = errno;  /* Save errno at entry */

  if (!memory_aware_flag || !monitor_thread_running) return;

  write_monitor_debug_file(immediate ? "stop_memory_monitor_immediate (entry)" : "stop_memory_monitor", saved_errno);

  if (immediate)
    debug_write(MEM_DEBUG_INFO, "[STOP_MONITOR_IMMEDIATE] Signal stop (pid=%d)\n", (int)getpid());
  else
    debug_write(MEM_DEBUG_INFO, "[STOP_MONITOR] Stopping monitor thread (makelevel=%u, pid=%d)\n", makelevel, (int)getpid());

  monitor_thread_running = 0;
  if (!immediate)
    pthread_join (memory_monitor_thread, NULL);

  saved_errno = errno;  /* Capture errno after join or flag set */

  /* Always reset terminal state - ANSI sequences may have left cursor in wrong position */
  /* But only if both stdout/stderr are TTYs - don't try to reset if we're piped */
  if (isatty(STDERR_FILENO) && isatty(STDOUT_FILENO))
    reset_terminal_state ();
  status_line_shown = 0;

  write_monitor_debug_file(immediate ? "stop_memory_monitor_immediate (exit)" : "stop_memory_monitor (exit)", saved_errno);

  /* Give thread a moment to see the flag and exit (only for immediate/signal handlers) */
  if (immediate) usleep (10000);  /* 10ms */
}

/* Minimal wrapper for atexit() - required because atexit needs void (*)(void) signature */
static void
stop_memory_monitor_atexit (void)
{
  stop_memory_monitor(0);
}
#else
/* Stub functions for non-POSIX systems */
static void
start_memory_monitor (void)
{
  /* Memory monitoring not available on this platform */
  memory_aware_flag = 0;
}

void
stop_memory_monitor (int immediate)
{
  /* No-op on non-POSIX systems */
  (void)immediate; /* Unused parameter */
}
#endif

static void
decode_debug_flags (void)
{
  const char **pp;

  if (debug_flag)
    db_level = DB_ALL;

  if (trace_flag)
    db_level |= DB_PRINT | DB_WHY;

  if (db_flags)
    for (pp=db_flags->list; *pp; ++pp)
      {
        const char *p = *pp;

        while (1)
          {
            switch (tolower (p[0]))
              {
              case 'a':
                db_level |= DB_ALL;
                break;
              case 'b':
                db_level |= DB_BASIC;
                break;
              case 'i':
                db_level |= DB_BASIC | DB_IMPLICIT;
                break;
              case 'j':
                db_level |= DB_JOBS;
                break;
              case 'm':
                db_level |= DB_BASIC | DB_MAKEFILES;
                break;
              case 'n':
                db_level = 0;
                break;
              case 'p':
                db_level |= DB_PRINT;
                break;
              case 'v':
                db_level |= DB_BASIC | DB_VERBOSE;
                break;
              case 'w':
                db_level |= DB_WHY;
                break;
              default:
                OS (fatal, NILF,
                    _("unknown debug level specification '%s'"), p);
              }

            while (*(++p) != '\0')
              if (*p == ',' || *p == ' ')
                {
                  ++p;
                  break;
                }

            if (*p == '\0')
              break;
          }
      }

  if (db_level)
    verify_flag = 1;

  if (! db_level)
    debug_flag = 0;
}

static void
decode_output_sync_flags (void)
{
#ifdef NO_OUTPUT_SYNC
  output_sync = OUTPUT_SYNC_NONE;
#else
  if (output_sync_option)
    {
      if (streq (output_sync_option, "none"))
        output_sync = OUTPUT_SYNC_NONE;
      else if (streq (output_sync_option, "line"))
        output_sync = OUTPUT_SYNC_LINE;
      else if (streq (output_sync_option, "target"))
        output_sync = OUTPUT_SYNC_TARGET;
      else if (streq (output_sync_option, "recurse"))
        output_sync = OUTPUT_SYNC_RECURSE;
      else
        OS (fatal, NILF,
            _("unknown output-sync type '%s'"), output_sync_option);
    }

  if (sync_mutex)
    osync_parse_mutex (sync_mutex);
#endif
}

/* Print a nice usage method and exit.  */

static void NORETURN
print_usage (int bad)
{
  const char *const *cpp;
  FILE *usageto;

  if (print_version_flag)
    {
      print_version ();
      fputs ("\n", stdout);
    }

  usageto = bad ? stderr : stdout;

  fprintf (usageto, _("Usage: %s [options] [target] ...\n"), program);

  for (cpp = usage; *cpp; ++cpp)
    fputs (_(*cpp), usageto);

  if (!remote_description || *remote_description == '\0')
    fprintf (usageto, _("\nThis program built for %s\n"), make_host);
  else
    fprintf (usageto, _("\nThis program built for %s (%s)\n"),
             make_host, remote_description);

  fprintf (usageto, _("Report bugs to <bug-make@gnu.org>\n"));

  die (bad ? MAKE_FAILURE : MAKE_SUCCESS);
}

#ifdef WINDOWS32

/*
 * HANDLE runtime exceptions by avoiding a requestor on the GUI. Capture
 * exception and print it to stderr instead.
 *
 * If ! DB_VERBOSE, just print a simple message and exit.
 * If DB_VERBOSE, print a more verbose message.
 * If compiled for DEBUG, let exception pass through to GUI so that
 *   debuggers can attach.
 */
LONG WINAPI
handle_runtime_exceptions (struct _EXCEPTION_POINTERS *exinfo)
{
  PEXCEPTION_RECORD exrec = exinfo->ExceptionRecord;
  LPSTR cmdline = GetCommandLine ();
  LPSTR prg = strtok (cmdline, " ");
  CHAR errmsg[1024];
#ifdef USE_EVENT_LOG
  HANDLE hEventSource;
  LPTSTR lpszStrings[1];
#endif

  if (! ISDB (DB_VERBOSE))
    {
      sprintf (errmsg,
               _("%s: Interrupt/Exception caught (code = 0x%lx, addr = 0x%p)\n"),
               prg, exrec->ExceptionCode, exrec->ExceptionAddress);
      fprintf (stderr, errmsg);
      exit (255);
    }

  sprintf (errmsg,
           _("\nUnhandled exception filter called from program %s\nExceptionCode = %lx\nExceptionFlags = %lx\nExceptionAddress = 0x%p\n"),
           prg, exrec->ExceptionCode, exrec->ExceptionFlags,
           exrec->ExceptionAddress);

  if (exrec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
      && exrec->NumberParameters >= 2)
    sprintf (&errmsg[strlen(errmsg)],
             (exrec->ExceptionInformation[0]
              ? _("Access violation: write operation at address 0x%p\n")
              : _("Access violation: read operation at address 0x%p\n")),
             (PVOID)exrec->ExceptionInformation[1]);

  /* turn this on if we want to put stuff in the event log too */
#ifdef USE_EVENT_LOG
  hEventSource = RegisterEventSource (NULL, "GNU Make");
  lpszStrings[0] = errmsg;

  if (hEventSource != NULL)
    {
      ReportEvent (hEventSource,         /* handle of event source */
                   EVENTLOG_ERROR_TYPE,  /* event type */
                   0,                    /* event category */
                   0,                    /* event ID */
                   NULL,                 /* current user's SID */
                   1,                    /* strings in lpszStrings */
                   0,                    /* no bytes of raw data */
                   lpszStrings,          /* array of error strings */
                   NULL);                /* no raw data */

      (VOID) DeregisterEventSource (hEventSource);
    }
#endif

  /* Write the error to stderr too */
  fprintf (stderr, errmsg);

#ifdef DEBUG
  return EXCEPTION_CONTINUE_SEARCH;
#else
  exit (255);
  return (255); /* not reached */
#endif
}

/*
 * On W32 systems we don't have the luxury of a /bin directory that
 * is mapped globally to every drive mounted to the system. Since make could
 * be invoked from any drive, and we don't want to propagate /bin/sh
 * to every single drive. Allow ourselves a chance to search for
 * a value for default shell here (if the default path does not exist).
 */

int
find_and_set_default_shell (const char *token)
{
  int sh_found = 0;
  char *atoken = 0;
  const char *search_token;
  const char *tokend;
  extern const char *default_shell;

  if (!token)
    search_token = default_shell;
  else
    search_token = atoken = xstrdup (token);

  /* If the user explicitly requests the DOS cmd shell, obey that request.
     However, make sure that's what they really want by requiring the value
     of SHELL either equal, or have a final path element of, "cmd" or
     "cmd.exe" case-insensitive.  */
  tokend = search_token + strlen (search_token) - 3;
  if (((tokend == search_token
        || (tokend > search_token && ISDIRSEP (tokend[-1])))
       && !strcasecmp (tokend, "cmd"))
      || ((tokend - 4 == search_token
           || (tokend - 4 > search_token && ISDIRSEP (tokend[-5])))
          && !strcasecmp (tokend - 4, "cmd.exe")))
    {
      batch_mode_shell = 1;
      unixy_shell = 0;
      default_shell = xstrdup (w32ify (search_token, 0));
      DB (DB_VERBOSE, (_("find_and_set_shell() setting default_shell = %s\n"),
                       default_shell));
      sh_found = 1;
    }
  else if (!no_default_sh_exe
           && (token == NULL || !strcmp (search_token, default_shell)))
    {
      /* no new information, path already set or known */
      sh_found = 1;
    }
  else if (_access (search_token, 0) == 0)
    {
      /* search token path was found */
      default_shell = xstrdup (w32ify (search_token, 0));
      DB (DB_VERBOSE, (_("find_and_set_shell() setting default_shell = %s\n"),
                       default_shell));
      sh_found = 1;
    }
  else
    {
      char *p;
      struct variable *v = lookup_variable (STRING_SIZE_TUPLE ("PATH"));

      /* Search Path for shell */
      if (v && v->value)
        {
          char *ep;

          p  = v->value;
          ep = strchr (p, PATH_SEPARATOR_CHAR);

          while (ep && *ep)
            {
              PATH_VAR (sh_path);

              *ep = '\0';

              snprintf (sh_path, GET_PATH_MAX, "%s/%s", p, search_token);
              if (_access (sh_path, 0) == 0)
                {
                  default_shell = xstrdup (w32ify (sh_path, 0));
                  sh_found = 1;
                  *ep = PATH_SEPARATOR_CHAR;

                  /* terminate loop */
                  p += strlen (p);
                }
              else
                {
                  *ep = PATH_SEPARATOR_CHAR;
                  p = ++ep;
                }

              ep = strchr (p, PATH_SEPARATOR_CHAR);
            }

          /* be sure to check last element of Path */
          if (p && *p)
            {
              PATH_VAR (sh_path);
              snprintf (sh_path, GET_PATH_MAX, "%s/%s", p, search_token);
              if (_access (sh_path, 0) == 0)
                {
                  default_shell = xstrdup (w32ify (sh_path, 0));
                  sh_found = 1;
                }
            }

          if (sh_found)
            DB (DB_VERBOSE,
                (_("find_and_set_shell() path search set default_shell = %s\n"),
                 default_shell));
        }
    }

  /* naive test */
  if (!unixy_shell && sh_found
      && (strstr (default_shell, "sh") || strstr (default_shell, "SH")))
    {
      unixy_shell = 1;
      batch_mode_shell = 0;
    }

#ifdef BATCH_MODE_ONLY_SHELL
  batch_mode_shell = 1;
#endif

  free (atoken);

  return (sh_found);
}
#endif  /* WINDOWS32 */

#ifdef __MSDOS__
static void
msdos_return_to_initial_directory (void)
{
  if (directory_before_chdir)
    chdir (directory_before_chdir);
}
#endif  /* __MSDOS__ */

static void
reset_jobserver (void)
{
  jobserver_clear ();
  free (jobserver_auth);
  jobserver_auth = NULL;
}

void
temp_stdin_unlink ()
{
  /* This function is called from a signal handler.  Keep async-signal-safe.
     If there is a temp file from reading from stdin, get rid of it.  */
  if (stdin_offset >= 0)
    {
      const char *nm = makefiles->list[stdin_offset];
      int r = 0;

      stdin_offset = -1;
      EINTRLOOP(r, unlink (nm));
      if (r < 0 && errno != ENOENT && !handling_fatal_signal)
        perror_with_name (_("unlink (temporary file): "), nm);
    }
}

#ifdef MK_OS_ZOS
extern char **environ;
#endif

#if defined(_AMIGA) || defined(MK_OS_ZOS)
int
main (int argc, char **argv)
#else
int
main (int argc, char **argv, char **envp)
#endif
{
  int makefile_status = MAKE_SUCCESS;
  struct goaldep *read_files;
  PATH_VAR (current_directory);
  unsigned int restarts = 0;
  unsigned int syncing = 0;
  int argv_slots;  /* The jobslot info we got from our parent process.  */



  /* Memory profiles will be loaded lazily when needed for PREDICT decisions */
#ifdef WINDOWS32
  const char *unix_path = NULL;
  const char *windows32_path = NULL;

  SetUnhandledExceptionFilter (handle_runtime_exceptions);

  /* start off assuming we have no shell */
  unixy_shell = 0;
  no_default_sh_exe = 1;
#endif

  initialize_variable_output ();

  /* Record start time for duration reporting */
  gettimeofday(&make_start_time, NULL);

  /* Useful for attaching debuggers, etc.  */
  SPIN ("main-entry");

#ifdef HAVE_ATEXIT
  if (ANY_SET (check_io_state (), IO_STDOUT_OK))
    atexit (close_stdout);
  /* Only top-level make should stop the monitor thread */
  if (makelevel == 0)
    atexit (stop_memory_monitor_atexit);
#endif

  output_init (&make_sync);

  initialize_stopchar_map();

#ifdef SET_STACK_SIZE
 /* Get rid of any avoidable limit on stack size.  */
  {
    struct rlimit rlim;

    /* Set the stack limit huge so that alloca does not fail.  */
    if (getrlimit (RLIMIT_STACK, &rlim) == 0
        && rlim.rlim_cur > 0 && rlim.rlim_cur < rlim.rlim_max)
      {
        stack_limit = rlim;
        rlim.rlim_cur = rlim.rlim_max;
        setrlimit (RLIMIT_STACK, &rlim);
      }
    else
      stack_limit.rlim_cur = 0;
  }
#endif

  /* Needed for OS/2 */
  initialize_main (&argc, &argv);

#ifdef MAKE_MAINTAINER_MODE
  /* In maintainer mode we always enable verification.  */
  verify_flag = 1;
#endif

#if defined (__MSDOS__) && !defined (_POSIX_SOURCE)
  /* Request the most powerful version of 'system', to
     make up for the dumb default shell.  */
  __system_flags = (__system_redirect
                    | __system_use_shell
                    | __system_allow_multiple_cmds
                    | __system_allow_long_cmds
                    | __system_handle_null_commands
                    | __system_emulate_chdir);

#endif

  /* Set up gettext/internationalization support.  */
  setlocale (LC_ALL, "");
  /* The cast to void shuts up compiler warnings on systems that
     disable NLS.  */
  (void)bindtextdomain (PACKAGE, LOCALEDIR);
  (void)textdomain (PACKAGE);

#ifdef  POSIX
  sigemptyset (&fatal_signal_set);
#define ADD_SIG(sig)    sigaddset (&fatal_signal_set, sig)
#else
#ifdef  HAVE_SIGSETMASK
  fatal_signal_mask = 0;
#define ADD_SIG(sig)    fatal_signal_mask |= sigmask (sig)
#else
#define ADD_SIG(sig)    (void)sig
#endif
#endif

#define FATAL_SIG(sig)                                                        \
  if (bsd_signal (sig, fatal_error_signal) == SIG_IGN)                        \
    bsd_signal (sig, SIG_IGN);                                                \
  else                                                                        \
    ADD_SIG (sig);

#ifdef SIGHUP
  FATAL_SIG (SIGHUP);
#endif
#ifdef SIGQUIT
  FATAL_SIG (SIGQUIT);
#endif
#ifdef SIGPIPE
  FATAL_SIG (SIGPIPE);
#endif
  FATAL_SIG (SIGINT);
  FATAL_SIG (SIGTERM);

#ifdef __MSDOS__
  /* Windows 9X delivers FP exceptions in child programs to their
     parent!  We don't want Make to die when a child divides by zero,
     so we work around that lossage by catching SIGFPE.  */
  FATAL_SIG (SIGFPE);
#endif

#ifdef  SIGDANGER
  FATAL_SIG (SIGDANGER);
#endif
#ifdef SIGXCPU
  FATAL_SIG (SIGXCPU);
#endif
#ifdef SIGXFSZ
  FATAL_SIG (SIGXFSZ);
#endif

#undef  FATAL_SIG

  /* Do not ignore the child-death signal.  This must be done before
     any children could possibly be created; otherwise, the wait
     functions won't work on systems with the SVR4 ECHILD brain
     damage, if our invoker is ignoring this signal.  */

#ifdef HAVE_WAIT_NOHANG
# if defined SIGCHLD
  (void) bsd_signal (SIGCHLD, SIG_DFL);
# endif
# if defined SIGCLD && SIGCLD != SIGCHLD
  (void) bsd_signal (SIGCLD, SIG_DFL);
# endif
#endif

  output_init (NULL);

  /* Figure out where this program lives.  */

  if (argv[0] == 0)
    argv[0] = (char *)"";
  if (argv[0][0] == '\0')
    program = "make";
  else
    {
#if defined(HAVE_DOS_PATHS)
      const char* start = argv[0];

      /* Skip an initial drive specifier if present.  */
      if (isalpha ((unsigned char)start[0]) && start[1] == ':')
        start += 2;

      if (start[0] == '\0')
        program = "make";
      else
        {
          program = start + strlen (start);
          while (program > start && ! ISDIRSEP (program[-1]))
            --program;

          /* Remove the .exe extension if present.  */
          {
            size_t len = strlen (program);
            if (len > 4 && streq (&program[len - 4], ".exe"))
              program = xstrndup (program, len - 4);
          }
        }
#elif defined(VMS)
      set_program_name (argv[0]);
      program = program_name;
      {
        const char *shell;
        char pwdbuf[256];
        char *pwd;
        shell = getenv ("SHELL");
        if (shell != NULL)
          vms_gnv_shell = 1;

        /* Need to know if CRTL set to report UNIX paths.  Use getcwd as
           it works on all versions of VMS. */
        pwd = getcwd(pwdbuf, 256);
        if (pwd[0] == '/')
          vms_report_unix_paths = 1;

        vms_use_mcr_command = get_vms_env_flag ("GNV$MAKE_USE_MCR", 0);

        vms_always_use_cmd_file = get_vms_env_flag ("GNV$MAKE_USE_CMD_FILE", 0);

        /* Legacy behavior is on VMS is older behavior that needed to be
           changed to be compatible with standard make behavior.
           For now only completely disable when running under a Bash shell.
           TODO: Update VMS built in recipes and macros to not need this
           behavior, at which time the default may change. */
        vms_legacy_behavior = get_vms_env_flag ("GNV$MAKE_OLD_VMS",
                                                !vms_gnv_shell);

        /* VMS was changed to use a comma separator in the past, but that is
           incompatible with built in functions that expect space separated
           lists.  Allow this to be selectively turned off. */
        vms_comma_separator = get_vms_env_flag ("GNV$MAKE_COMMA",
                                                vms_legacy_behavior);

        /* Some Posix shell syntax options are incompatible with VMS syntax.
           VMS requires double quotes for strings and escapes quotes
           differently.  When this option is active, VMS will try
           to simulate Posix shell simulations instead of using
           VMS DCL behavior. */
        vms_unix_simulation = get_vms_env_flag ("GNV$MAKE_SHELL_SIM",
                                                !vms_legacy_behavior);

      }
      if (need_vms_symbol () && !vms_use_mcr_command)
        create_foreign_command (program_name, argv[0]);
#else
      program = strrchr (argv[0], '/');
      if (program == 0)
        program = argv[0];
      else
        ++program;
#endif
    }

  initialize_global_hash_tables ();

  /* Ensure the temp directory is set up: we don't want the first time we use
     it to be in a forked process.  */
  get_tmpdir ();

  /* Figure out where we are.  */

#ifdef WINDOWS32
  if (getcwd_fs (current_directory, GET_PATH_MAX) == 0)
#else
  if (getcwd (current_directory, GET_PATH_MAX) == 0)
#endif
    {
#ifdef  HAVE_GETCWD
      perror_with_name ("getcwd", "");
#else
      OS (error, NILF, "getwd: %s", current_directory);
#endif
      current_directory[0] = '\0';
      directory_before_chdir = 0;
    }
  else
    directory_before_chdir = xstrdup (current_directory);

#ifdef  __MSDOS__
  /* Make sure we will return to the initial directory, come what may.  */
  atexit (msdos_return_to_initial_directory);
#endif

  /* Initialize the special variables.  */
  define_variable_cname (".VARIABLES", "", o_default, 0)->special = 1;
  /* define_variable_cname (".TARGETS", "", o_default, 0)->special = 1; */
  define_variable_cname (".RECIPEPREFIX", "", o_default, 0)->special = 1;
  define_variable_cname (".SHELLFLAGS", "-c", o_default, 0);
  define_variable_cname (".LOADED", "", o_default, 0);

  /* Set up .FEATURES
     Use a separate variable because define_variable_cname() is a macro and
     some compilers (MSVC) don't like conditionals in macros.  */
  {
    const char *features = "target-specific order-only second-expansion"
                           " else-if shortest-stem undefine oneshell nocomment"
                           " grouped-target extra-prereqs notintermediate"
                           " shell-export"
#ifndef NO_ARCHIVES
                           " archives"
#endif
#ifdef MAKE_JOBSERVER
                           " jobserver"
# if JOBSERVER_USE_FIFO
                           " jobserver-fifo"
# endif
#endif
#ifndef NO_OUTPUT_SYNC
                           " output-sync"
#endif
#ifdef MAKE_SYMLINKS
                           " check-symlink"
#endif
#ifdef HAVE_GUILE
                           " guile"
#endif
#ifdef MAKE_LOAD
                           " load"
#endif
#ifdef HAVE_DOS_PATHS
                           " dospaths"
#endif
#ifdef MAKE_MAINTAINER_MODE
                           " maintainer"
#endif
                           ;

    define_variable_cname (".FEATURES", features, o_default, 0);
  }

  /* Configure GNU Guile support */
  guile_gmake_setup (NILF);

  /* Read in variables from the environment.  It is important that this be
     done before $(MAKE) is figured out so its definitions will not be
     from the environment.  */

#ifdef MK_OS_ZOS
  char **envp = environ;
#endif

#ifndef _AMIGA
  {
    unsigned int i;

    for (i = 0; envp[i] != 0; ++i)
      {
        struct variable *v;
        const char *ep = envp[i];
        /* By default, export all variables culled from the environment.  */
        enum variable_export export = v_export;
        size_t len;

        while (! STOP_SET (*ep, MAP_EQUALS|MAP_NUL))
          ++ep;

        /* If there's no equals sign it's a malformed environment.  Ignore.  */
        if (*ep == '\0')
          continue;

#ifdef WINDOWS32
        if (!unix_path && strneq (envp[i], "PATH=", 5))
          unix_path = ep+1;
        else if (!strnicmp (envp[i], "Path=", 5))
          {
            if (!windows32_path)
              windows32_path = ep+1;
            /* PATH gets defined after the loop exits.  */
            continue;
          }
#endif

        /* Length of the variable name, and skip the '='.  */
        len = ep++ - envp[i];

        /* If this is MAKE_RESTARTS, check to see if the "already printed
           the enter statement" flag is set.  */
        if (len == 13 && memcmp (envp[i], "MAKE_RESTARTS", CSTRLEN ("MAKE_RESTARTS")) == 0)
          {
            if (*ep == '-')
              {
                OUTPUT_TRACED ();
                ++ep;
              }
            restarts = make_toui (ep, NULL);
            export = v_noexport;
          }

        v = define_variable (envp[i], len, ep, o_env, 1);

        /* POSIX says the value of SHELL set in the makefile won't change the
           value of SHELL given to subprocesses.  */
        if (streq (v->name, "SHELL"))
          {
#ifndef __MSDOS__
            export = v_noexport;
#endif
            shell_var.name = xstrdup ("SHELL");
            shell_var.length = 5;
            shell_var.value = xstrdup (ep);
          }

        v->export = export;
      }
  }
#ifdef WINDOWS32
  /* If we didn't find a correctly spelled PATH we define PATH as
   * either the first misspelled value or an empty string
   */
  if (!unix_path)
    define_variable_cname ("PATH", windows32_path ? windows32_path : "",
                           o_env, 1)->export = v_export;
#endif
#else /* For Amiga, read the ENV: device, ignoring all dirs */
  {
    BPTR env, file, old;
    char buffer[1024];
    int len;
    __aligned struct FileInfoBlock fib;

    env = Lock ("ENV:", ACCESS_READ);
    if (env)
      {
        old = CurrentDir (DupLock (env));
        Examine (env, &fib);

        while (ExNext (env, &fib))
          {
            if (fib.fib_DirEntryType < 0) /* File */
              {
                /* Define an empty variable. It will be filled in
                   variable_lookup(). Makes startup quite a bit faster. */
                define_variable (fib.fib_FileName,
                                 strlen (fib.fib_FileName),
                                 "", o_env, 1)->export = v_export;
              }
          }
        UnLock (env);
        UnLock (CurrentDir (old));
      }
  }
#endif

  /* Decode the switches.  */
  if (lookup_variable (STRING_SIZE_TUPLE (GNUMAKEFLAGS_NAME)))
    {
      decode_env_switches (STRING_SIZE_TUPLE (GNUMAKEFLAGS_NAME), o_command);

      /* Clear GNUMAKEFLAGS to avoid duplication.  */
      define_variable_cname (GNUMAKEFLAGS_NAME, "", o_env, 0);
    }

   /* Set MAKEFLAGS's origin to command line: in submakes MAKEFLAGS will carry
      command line switches.  This causes env variable MAKEFLAGS to beat
      makefile modifications to MAKEFLAGS.  */
  decode_env_switches (STRING_SIZE_TUPLE (MAKEFLAGS_NAME), o_command);

#if 0
  /* People write things like:
        MFLAGS="CC=gcc -pipe" "CFLAGS=-g"
     and we set the -p, -i and -e switches.  Doesn't seem quite right.  */
  decode_env_switches (STRING_SIZE_TUPLE ("MFLAGS"));
#endif

  /* In output sync mode we need to sync any output generated by reading the
     makefiles, such as in $(info ...) or stderr from $(shell ...) etc.  */

  syncing = make_sync.syncout = (output_sync == OUTPUT_SYNC_LINE
                                 || output_sync == OUTPUT_SYNC_TARGET);
  OUTPUT_SET (&make_sync);

  /* Parse the command line options.  Remember the job slots set this way.  */
  {
    int env_slots = arg_job_slots;
    arg_job_slots = INVALID_JOB_SLOTS;

    decode_switches (argc, (const char **)argv, o_command);
    argv_slots = arg_job_slots;

    if (arg_job_slots == INVALID_JOB_SLOTS)
      arg_job_slots = env_slots;
  }

  if (print_usage_flag)
    print_usage (0);

  /* Print version information, and exit.  */
  if (print_version_flag)
    {
      print_version ();
      die (MAKE_SUCCESS);
    }

  /* Now that we know we'll be running, force stdout to be line-buffered.  */
#ifdef HAVE_SETVBUF
  setvbuf (stdout, 0, _IOLBF, BUFSIZ);
#elif HAVE_SETLINEBUF
  setlinebuf (stdout);
#endif

  /* Handle shuffle mode argument.  */
  if (shuffle_mode)
    {
      const char *effective_mode;
      shuffle_set_mode (shuffle_mode);

      /* Write fixed seed back to argument list to propagate mode and
         fixed seed to child $(MAKE) runs.  */
      free (shuffle_mode);
      effective_mode = shuffle_get_mode ();
      if (effective_mode)
        shuffle_mode = xstrdup (effective_mode);
      else
        shuffle_mode = NULL;
    }

  /* Set a variable specifying whether stdout/stdin is hooked to a TTY.  */
#ifdef HAVE_ISATTY
  if (isatty (fileno (stdout)))
    if (! lookup_variable (STRING_SIZE_TUPLE ("MAKE_TERMOUT")))
      {
        const char *tty = TTYNAME (fileno (stdout));
        define_variable_cname ("MAKE_TERMOUT", tty ? tty : DEFAULT_TTYNAME,
                               o_default, 0)->export = v_export;
      }
  if (isatty (fileno (stderr)))
    if (! lookup_variable (STRING_SIZE_TUPLE ("MAKE_TERMERR")))
      {
        const char *tty = TTYNAME (fileno (stderr));
        define_variable_cname ("MAKE_TERMERR", tty ? tty : DEFAULT_TTYNAME,
                               o_default, 0)->export = v_export;
      }
#endif

  /* Reset in case the switches changed our minds.  */
  syncing = (output_sync == OUTPUT_SYNC_LINE
             || output_sync == OUTPUT_SYNC_TARGET);

  if (make_sync.syncout && ! syncing)
    output_close (&make_sync);

  make_sync.syncout = syncing;
  OUTPUT_SET (&make_sync);

  /* Figure out the level of recursion.  */
  {
    struct variable *v = lookup_variable (STRING_SIZE_TUPLE (MAKELEVEL_NAME));
    if (v && v->value[0] != '\0' && v->value[0] != '-')
      makelevel = make_toui (v->value, NULL);
    else
      makelevel = 0;
  }

  /* Define MAKE_TOP_LEVEL_CWD as a make variable for child processes (only for top-level make) */
  if (makelevel == 0)
    {
      char *top_cwd = getcwd(NULL, 0);
      define_variable_global("MAKE_TOP_LEVEL_CWD", sizeof("MAKE_TOP_LEVEL_CWD") - 1,
                              top_cwd, o_env, 0, NILF);
      debug_write(MEM_DEBUG_VERBOSE, "[DEBUG] Defined MAKE_TOP_LEVEL_CWD=%s as make variable (PID=%d, makelevel=%u)\n", top_cwd, getpid(), makelevel);
    }

  /* Initialize shared memory for inter-process communication (only if memory monitoring is enabled) */
  if (memory_aware_flag && init_shared_memory () != 0)
    {
      fprintf (stderr, "Warning: Failed to initialize shared memory for memory monitoring\n");
    }

  /* Set always_make_flag if -B was given and we've not restarted already.  */
  always_make_flag = always_make_set && (restarts == 0);

  /* If -R was given, set -r too (doesn't make sense otherwise!)  */
  if (no_builtin_variables_flag)
    no_builtin_rules_flag = 1;

  if (ISDB (DB_BASIC))
    {
      print_version ();

      /* Flush stdout so the user doesn't have to wait to see the
         version information while make thinks about things.  */
      fflush (stdout);
    }

#ifndef VMS
  /* Set the "MAKE_COMMAND" variable to the name we were invoked with.
     (If it is a relative pathname with a slash, prepend our directory name
     so the result will run the same program regardless of the current dir.
     If it is a name with no slash, we can only hope that PATH did not
     find it in the current directory.)  */
#ifdef WINDOWS32
  /*
   * Convert from backslashes to forward slashes for
   * programs like sh which don't like them. Shouldn't
   * matter if the path is one way or the other for
   * CreateProcess().
   */
  if (strpbrk (argv[0], "/:\\") || strstr (argv[0], "..")
      || strneq (argv[0], "//", 2))
    argv[0] = xstrdup (w32ify (argv[0], 1));
#else /* WINDOWS32 */
#if defined (__MSDOS__) || defined (__EMX__)
  if (strchr (argv[0], '\\'))
    {
      char *p;

      argv[0] = xstrdup (argv[0]);
      for (p = argv[0]; *p; p++)
        if (*p == '\\')
          *p = '/';
    }
  /* If argv[0] is not in absolute form, prepend the current
     directory.  This can happen when Make is invoked by another DJGPP
     program that uses a non-absolute name.  */
  if (current_directory[0] != '\0'
      && argv[0] != 0
      && (argv[0][0] != '/' && (argv[0][0] == '\0' || argv[0][1] != ':'))
# ifdef __EMX__
      /* do not prepend cwd if argv[0] contains no '/', e.g. "make" */
      && (strchr (argv[0], '/') != 0 || strchr (argv[0], '\\') != 0)
# endif
      )
    argv[0] = xstrdup (concat (3, current_directory, "/", argv[0]));
#else  /* !__MSDOS__ */
  if (current_directory[0] != '\0'
      && argv[0] != 0 && argv[0][0] != '/' && strchr (argv[0], '/') != 0
#ifdef HAVE_DOS_PATHS
      && (argv[0][0] != '\\' && (!argv[0][0] || argv[0][1] != ':'))
      && strchr (argv[0], '\\') != 0
#endif
      )
    argv[0] = xstrdup (concat (3, current_directory, "/", argv[0]));
#endif /* !__MSDOS__ */
#endif /* WINDOWS32 */
#endif

  /* We may move, but until we do, here we are.  */
  starting_directory = current_directory;

  /* If there were -C flags, move ourselves about.  */
  if (directories != 0)
    {
      unsigned int i;
      for (i = 0; directories->list[i] != 0; ++i)
        {
          const char *dir = directories->list[i];
#ifdef WINDOWS32
          /* WINDOWS32 chdir() doesn't work if the directory has a trailing '/'
             But allow -C/ just in case someone wants that.  */
          {
            char *p = (char *)dir + strlen (dir) - 1;
            while (p > dir && ISDIRSEP (p[0]))
              --p;
            p[1] = '\0';
          }
#endif
          if (chdir (dir) < 0)
            pfatal_with_name (dir);
        }
    }

#ifdef WINDOWS32
  /*
   * THIS BLOCK OF CODE MUST COME AFTER chdir() CALL ABOVE IN ORDER
   * TO NOT CONFUSE THE DEPENDENCY CHECKING CODE IN implicit.c.
   *
   * The functions in dir.c can incorrectly cache information for "."
   * before we have changed directory and this can cause file
   * lookups to fail because the current directory (.) was pointing
   * at the wrong place when it was first evaluated.
   */
  no_default_sh_exe = !find_and_set_default_shell (NULL);
#endif /* WINDOWS32 */

  /* If we chdir'ed, figure out where we are now.  */
  if (directories)
    {
#ifdef WINDOWS32
      if (getcwd_fs (current_directory, GET_PATH_MAX) == 0)
#else
      if (getcwd (current_directory, GET_PATH_MAX) == 0)
#endif
        {
#ifdef  HAVE_GETCWD
          perror_with_name ("getcwd", "");
#else
          OS (error, NILF, "getwd: %s", current_directory);
#endif
          starting_directory = 0;
        }
      else
        starting_directory = current_directory;
    }

  define_variable_cname ("CURDIR", current_directory, o_file, 0);

  /* Construct the list of include directories to search.
     This will check for existence so it must be done after chdir.  */
  construct_include_path (include_dirs ? include_dirs->list : NULL);

  /* Validate the arg_job_slots configuration before we define MAKEFLAGS so
     users get an accurate value in their makefiles.
     At this point arg_job_slots is the argv setting, if there is one, else
     the MAKEFLAGS env setting, if there is one.  */

  if (jobserver_auth)
    {
      /* We're a child in an existing jobserver group.  */
      if (argv_slots == INVALID_JOB_SLOTS)
        {
          /* There's no -j option on the command line: check authorization.  */
          if (jobserver_parse_auth (jobserver_auth))
            /* Success!  Use the jobserver.  */
            goto job_setup_complete;

          /* Oops: we have jobserver-auth but it's invalid :(.  */
          O (error, NILF, _("warning: jobserver unavailable: using -j1.  Add '+' to parent make rule."));
          arg_job_slots = 1;
        }

      /* The user provided a -j setting on the command line so use it: we're
         the master make of a new jobserver group.  */
      else if (!restarts)
        ON (error, NILF,
            _("warning: -j%d forced in submake: resetting jobserver mode."),
            argv_slots);

      /* We can't use our parent's jobserver, so reset.  */
      reset_jobserver ();
    }

 job_setup_complete:

  /* Initialize memory monitoring from environment variables */
  init_memory_monitoring_env ();

  /* Start memory monitoring thread if memory-aware mode is enabled (top-level only) */
  if (memory_aware_flag && makelevel == 0) start_memory_monitor ();

  /* The extra indirection through $(MAKE_COMMAND) is done
     for hysterical raisins.  */

#ifdef VMS
  if (vms_use_mcr_command)
    define_variable_cname ("MAKE_COMMAND", vms_command (argv[0]), o_default, 0);
  else
    define_variable_cname ("MAKE_COMMAND", program, o_default, 0);
#else
  define_variable_cname ("MAKE_COMMAND", argv[0], o_default, 0);
#endif
  define_variable_cname ("MAKE", "$(MAKE_COMMAND)", o_default, 1);

  if (command_variables != 0)
    {
      struct command_variable *cv;
      struct variable *v;
      size_t len = 0;
      char *value, *p;

      /* Figure out how much space will be taken up by the command-line
         variable definitions.  */
      for (cv = command_variables; cv != 0; cv = cv->next)
        {
          v = cv->variable;
          len += 2 * strlen (v->name);
          if (! v->recursive)
            ++len;
          ++len;
          len += 2 * strlen (v->value);
          ++len;
        }

      /* Now allocate a buffer big enough and fill it.  */
      p = value = alloca (len);
      for (cv = command_variables; cv != 0; cv = cv->next)
        {
          v = cv->variable;
          p = quote_for_env (p, v->name);
          if (! v->recursive)
            *p++ = ':';
          *p++ = '=';
          p = quote_for_env (p, v->value);
          *p++ = ' ';
        }
      p[-1] = '\0';             /* Kill the final space and terminate.  */

      /* Define an unchangeable variable with a name that no POSIX.2
         makefile could validly use for its own variable.  */
      define_variable_cname ("-*-command-variables-*-", value, o_automatic, 0);

      /* Define the variable; this will not override any user definition.
         Normally a reference to this variable is written into the value of
         MAKEFLAGS, allowing the user to override this value to affect the
         exported value of MAKEFLAGS.  In POSIX-pedantic mode, we cannot
         allow the user's setting of MAKEOVERRIDES to affect MAKEFLAGS, so
         a reference to this hidden variable is written instead. */
      define_variable_cname ("MAKEOVERRIDES", "${-*-command-variables-*-}",
                             o_default, 1);
#ifdef VMS
      vms_export_dcl_symbol ("MAKEOVERRIDES", "${-*-command-variables-*-}");
#endif
    }

  /* Read any stdin makefiles into temporary files.  */

  if (makefiles != 0)
    {
      unsigned int i;
      for (i = 0; i < makefiles->idx; ++i)
        if (makefiles->list[i][0] == '-' && makefiles->list[i][1] == '\0')
          {
            /* This makefile is standard input.  Since we may re-exec
               and thus re-read the makefiles, we read standard input
               into a temporary file and read from that.  */
            FILE *outfile;
            char *newnm;

            if (stdin_offset >= 0)
              O (fatal, NILF,
                 _("Makefile from standard input specified twice"));

            outfile = get_tmpfile (&newnm);
            if (!outfile)
              O (fatal, NILF,
                 _("cannot store makefile from stdin to a temporary file"));

            while (!feof (stdin) && ! ferror (stdin))
              {
                char buf[2048];
                size_t n = fread (buf, 1, sizeof (buf), stdin);
                if (n > 0 && fwrite (buf, 1, n, outfile) != n)
                  OSS (fatal, NILF,
                       _("fwrite: temporary file %s: %s"), newnm, strerror (errno));
              }
            fclose (outfile);

            /* Replace the name that read_all_makefiles will see with the name
               of the temporary file.  */
            makefiles->list[i] = strcache_add (newnm);
            stdin_offset = i;

            free (newnm);
          }
    }

  /* Make sure the temporary file is never considered updated.  */
  if (stdin_offset >= 0)
    {
      struct file *f = enter_file (makefiles->list[stdin_offset]);
      f->updated = 1;
      f->update_status = us_success;
      f->command_state = cs_finished;
      /* Can't be intermediate, or it'll be removed before make re-exec.  */
      f->intermediate = 0;
      f->dontcare = 0;
      /* Avoid re-exec due to stdin temp file timestamps.  */
      f->last_mtime = f->mtime_before_update = f_mtime (f, 0);
    }

#ifndef __EMX__ /* Don't use a SIGCHLD handler for OS/2 */
#if !defined(HAVE_WAIT_NOHANG) || defined(MAKE_JOBSERVER)
  /* Set up to handle children dying.  This must be done before
     reading in the makefiles so that 'shell' function calls will work.

     If we don't have a hanging wait we have to fall back to old, broken
     functionality here and rely on the signal handler and counting
     children.

     If we're using the jobs pipe we need a signal handler so that SIGCHLD is
     not ignored; we need it to interrupt the read(2) of the jobserver pipe if
     we're waiting for a token.

     If none of these are true, we don't need a signal handler at all.  */
  {
# if defined SIGCHLD
    bsd_signal (SIGCHLD, child_handler);
# endif
# if defined SIGCLD && SIGCLD != SIGCHLD
    bsd_signal (SIGCLD, child_handler);
# endif
  }

#if defined(HAVE_PSELECT) && !defined(MK_OS_ZOS)
  /* If we have pselect() then we need to block SIGCHLD so it's deferred.  */
  {
    sigset_t block;
    sigemptyset (&block);
    sigaddset (&block, SIGCHLD);
    if (sigprocmask (SIG_SETMASK, &block, NULL) < 0)
      pfatal_with_name ("sigprocmask(SIG_SETMASK, SIGCHLD)");
  }
#endif

#endif
#endif

  /* Let the user send us SIGUSR1 to toggle the -d flag during the run.  */
#ifdef SIGUSR1
  bsd_signal (SIGUSR1, debug_signal_handler);
#endif

  /* Define the initial list of suffixes for old-style rules.  */
  set_default_suffixes ();

  /* Define some internal and special variables.  */
  define_automatic_variables ();

  /* Set up the MAKEFLAGS and MFLAGS variables for makefiles to see.
     Initialize it to be exported but allow the makefile to reset it.  */
  define_makeflags (0)->export = v_export;

  /* Define the default variables.  */
  define_default_variables ();

  default_file = enter_file (strcache_add (".DEFAULT"));

  default_goal_var = define_variable_cname (".DEFAULT_GOAL", "", o_file, 0);

  /* Evaluate all strings provided with --eval.
     Also set up the $(-*-eval-flags-*-) variable.  */

  if (eval_strings)
    {
      char *p, *endp, *value;
      unsigned int i;
      size_t len = (CSTRLEN ("--eval=") + 1) * eval_strings->idx;

      for (i = 0; i < eval_strings->idx; ++i)
        {
          p = xstrdup (eval_strings->list[i]);
          len += 2 * strlen (p);
          eval_buffer (p, NULL);
          free (p);
        }

      p = endp = value = alloca (len);
      for (i = 0; i < eval_strings->idx; ++i)
        {
          p = stpcpy (p, "--eval=");
          p = quote_for_env (p, eval_strings->list[i]);
          endp = p++;
          *endp = ' ';
        }
      *endp = '\0';

      define_variable_cname ("-*-eval-flags-*-", value, o_automatic, 0);
    }

  {
    int old_builtin_rules_flag = no_builtin_rules_flag;
    int old_builtin_variables_flag = no_builtin_variables_flag;
    int old_arg_job_slots = arg_job_slots;

    /* Read all the makefiles.  */
    read_files = read_all_makefiles (makefiles == 0 ? 0 : makefiles->list);

    arg_job_slots = INVALID_JOB_SLOTS;

    /* Decode switches again, for variables set by the makefile.  */
    decode_env_switches (STRING_SIZE_TUPLE (GNUMAKEFLAGS_NAME), o_env);

    /* Clear GNUMAKEFLAGS to avoid duplication.  */
    define_variable_cname (GNUMAKEFLAGS_NAME, "", o_override, 0);

    decode_env_switches (STRING_SIZE_TUPLE (MAKEFLAGS_NAME), o_env);
#if 0
    decode_env_switches (STRING_SIZE_TUPLE ("MFLAGS"));
#endif

    /* If -j is not set in the makefile, or it was set on the command line,
       reset to use the previous value.  */
    if (arg_job_slots == INVALID_JOB_SLOTS || argv_slots != INVALID_JOB_SLOTS)
      arg_job_slots = old_arg_job_slots;

    else if (jobserver_auth && arg_job_slots != old_arg_job_slots)
      {
        /* Makefile MAKEFLAGS set -j, but we already have a jobserver.
           Make us the master of a new jobserver group.  */
        if (!restarts)
          ON (error, NILF,
              _("warning: -j%d forced in makefile: resetting jobserver mode."),
              arg_job_slots);

        /* We can't use our parent's jobserver, so reset.  */
        reset_jobserver ();
      }

    /* Reset in case the switches changed our mind.  */
    syncing = (output_sync == OUTPUT_SYNC_LINE
               || output_sync == OUTPUT_SYNC_TARGET);

    if (make_sync.syncout && ! syncing)
      output_close (&make_sync);

    make_sync.syncout = syncing;
    OUTPUT_SET (&make_sync);

    /* If -R was given, set -r too (doesn't make sense otherwise!)  */
    if (no_builtin_variables_flag)
      no_builtin_rules_flag = 1;

    /* If we've disabled builtin rules, get rid of them.  */
    if (no_builtin_rules_flag && ! old_builtin_rules_flag)
      {
        if (suffix_file->builtin)
          {
            free_dep_chain (suffix_file->deps);
            suffix_file->deps = 0;
          }
        define_variable_cname ("SUFFIXES", "", o_default, 0);
      }

    /* If we've disabled builtin variables, get rid of them.  */
    if (no_builtin_variables_flag && ! old_builtin_variables_flag)
      undefine_default_variables ();
  }

#ifdef WINDOWS32
  /* look one last time after reading all Makefiles */
  if (no_default_sh_exe)
    no_default_sh_exe = !find_and_set_default_shell (NULL);
#endif /* WINDOWS32 */

#if defined (__MSDOS__) || defined (__EMX__) || defined (VMS)
  /* We need to know what kind of shell we will be using.  */
  {
    extern int _is_unixy_shell (const char *_path);
    struct variable *shv = lookup_variable (STRING_SIZE_TUPLE ("SHELL"));
    extern int unixy_shell;
    extern const char *default_shell;

    if (shv && *shv->value)
      {
        char *shell_path = recursively_expand (shv);

        if (shell_path && _is_unixy_shell (shell_path))
          unixy_shell = 1;
        else
          unixy_shell = 0;
        if (shell_path)
          default_shell = shell_path;
      }
  }
#endif /* __MSDOS__ || __EMX__ */

  /* Final jobserver configuration.

     If we have jobserver_auth then we are a client in an existing jobserver
     group, that's already been verified OK above.  If we don't have
     jobserver_auth and jobserver is enabled, then start a new jobserver.

     arg_job_slots = INVALID_JOB_SLOTS if we don't want -j in MAKEFLAGS

     arg_job_slots = # of jobs of parallelism

     job_slots = 0 for no limits on jobs, or when limiting via jobserver.

     job_slots = 1 for standard non-parallel mode.

     job_slots >1 for old-style parallelism without jobservers.  */

  if (jobserver_auth)
    job_slots = 0;
  else if (arg_job_slots == INVALID_JOB_SLOTS)
    job_slots = 1;
  else
    job_slots = arg_job_slots;

#if defined (__MSDOS__) || defined (__EMX__) || defined (VMS)
  if (job_slots != 1
# ifdef __EMX__
      && _osmode != OS2_MODE /* turn off -j if we are in DOS mode */
# endif
      )
    {
      O (error, NILF,
         _("Parallel jobs (-j) are not supported on this platform."));
      O (error, NILF, _("Resetting to single job (-j1) mode."));
      arg_job_slots = INVALID_JOB_SLOTS;
      job_slots = 1;
    }
#endif

  /* If we have >1 slot at this point, then we're a top-level make.
     Set up the jobserver.

     Every make assumes that it always has one job it can run.  For the
     submakes it's the token they were given by their parent.  For the top
     make, we just subtract one from the number the user wants.  */

  if (job_slots > 1 && jobserver_setup (job_slots - 1, jobserver_style))
    {
      /* Fill in the jobserver_auth for our children.  */
      jobserver_auth = jobserver_get_auth ();

      if (jobserver_auth)
        {
          /* We're using the jobserver so set job_slots to 0.  */
          master_job_slots = job_slots;
          job_slots = 0;
          /* Debug: top-level make jobserver setup */
          DB (DB_JOBS, (_("[MAIN] makelevel=%u PID=%d PPID=%d: Top-level make created jobserver with master_job_slots=%u (jobserver_auth=%s)\n"),
                        makelevel, (int)getpid(), (int)getppid(), master_job_slots, jobserver_auth));
        }
    }

  /* If we're not using parallel jobs, then we don't need output sync.
     This is so people can enable output sync in GNUMAKEFLAGS or similar, but
     not have it take effect unless parallel builds are enabled.  */
  if (syncing && job_slots == 1)
    {
      OUTPUT_UNSET ();
      output_close (&make_sync);
      syncing = 0;
      output_sync = OUTPUT_SYNC_NONE;
    }

  if (syncing)
    {
      /* If there is no mutex we're the base: create one.  Else parse it.  */
      if (!sync_mutex)
        {
          osync_setup ();
          sync_mutex = osync_get_mutex ();
        }
      else if (!osync_parse_mutex (sync_mutex))
        {
          /* Parsing failed; continue without output sync.  */
          osync_clear ();
          free (sync_mutex);
          sync_mutex = NULL;
          syncing = 0;
        }
    }

  if (jobserver_auth)
    DB (DB_VERBOSE|DB_JOBS, (_("[JOBSERVER] makelevel=%u PID=%d PPID=%d: Using jobserver controller %s\n"),
                              makelevel, (int)getpid(), (int)getppid(), jobserver_auth));
  if (sync_mutex)
    DB (DB_VERBOSE, (_("Using output-sync mutex %s\n"), sync_mutex));

#ifndef MAKE_SYMLINKS
  if (check_symlink_flag)
    {
      O (error, NILF, _("Symbolic links not supported: disabling -L."));
      check_symlink_flag = 0;
    }
#endif

  /* Set up MAKEFLAGS and MFLAGS again, so they will be right.  */

  define_makeflags (0);

  /* Make each 'struct goaldep' point at the 'struct file' for the file
     depended on.  Also do magic for special targets.  */

  snap_deps ();

  /* Define the file rules for the built-in suffix rules.  These will later
     be converted into pattern rules.  */

  install_default_suffix_rules ();

  /* Convert old-style suffix rules to pattern rules.  It is important to
     do this before installing the built-in pattern rules below, so that
     makefile-specified suffix rules take precedence over built-in pattern
     rules.  */

  convert_to_pattern ();

  /* Install the default implicit pattern rules.
     This used to be done before reading the makefiles.
     But in that case, built-in pattern rules were in the chain
     before user-defined ones, so they matched first.  */

  install_default_implicit_rules ();

  /* Compute implicit rule limits and do magic for pattern rules.  */

  snap_implicit_rules ();

  /* Construct the listings of directories in VPATH lists.  */

  build_vpath_lists ();

  /* Mark files given with -o flags as very old and as having been updated
     already, and files given with -W flags as brand new (time-stamp as far
     as possible into the future).  If restarts is set we'll do -W later.  */

  if (old_files != 0)
    {
      const char **p;
      for (p = old_files->list; *p != 0; ++p)
        {
          struct file *f = enter_file (*p);
          f->last_mtime = f->mtime_before_update = OLD_MTIME;
          f->updated = 1;
          f->update_status = us_success;
          f->command_state = cs_finished;
        }
    }

  if (!restarts && new_files != 0)
    {
      const char **p;
      for (p = new_files->list; *p != 0; ++p)
        {
          struct file *f = enter_file (*p);
          f->last_mtime = f->mtime_before_update = NEW_MTIME;
        }
    }

  /* Initialize the remote job module.  */
  remote_setup ();

  /* Dump any output we've collected.  */

  OUTPUT_UNSET ();
  output_close (&make_sync);

  if (shuffle_mode)
    DB (DB_BASIC, (_("Enabled shuffle mode: %s\n"), shuffle_mode));

  if (read_files)
    {
      /* Update any makefiles if necessary.  */

      FILE_TIMESTAMP *makefile_mtimes;
      struct goaldep *skipped_makefiles = NULL;
      const char **nargv = (const char **) argv;
      int any_failed = 0;
      enum update_status status;

      DB (DB_BASIC, (_("Updating makefiles....\n")));

      /* Count the makefiles, and reverse the order so that we attempt to
         rebuild them in the order they were read.  */
      {
        unsigned int num_mkfiles = 0;
        struct goaldep *d = read_files;
        read_files = NULL;
        while (d != NULL)
          {
            struct goaldep *t = d;
            d = d->next;
            t->next = read_files;
            read_files = t;
            ++num_mkfiles;
          }

        makefile_mtimes = alloca (num_mkfiles * sizeof (FILE_TIMESTAMP));
      }

      /* Remove any makefiles we don't want to try to update.  Record the
         current modtimes of the others so we can compare them later.  */
      {
        struct goaldep *d = read_files;
        struct goaldep *last = NULL;
        unsigned int mm_idx = 0;

        while (d != 0)
          {
            int skip = 0;
            struct file *f = d->file;

            /* Check for makefiles that are either phony or a :: target with
               commands, but no dependencies.  These will always be remade,
               which will cause an infinite restart loop, so don't try to
               remake it (this will only happen if your makefiles are written
               exceptionally stupidly; but if you work for Athena, that's how
               you write your makefiles.)  */

            if (f->phony)
              skip = 1;
            else
              for (f = f->double_colon; f != NULL; f = f->prev)
                if (f->deps == NULL && f->cmds != NULL)
                  {
                    skip = 1;
                    break;
                  }

            if (!skip)
              {
                makefile_mtimes[mm_idx++] = file_mtime_no_search (d->file);
                last = d;
                d = d->next;
              }
            else
              {
                DB (DB_VERBOSE,
                    (_("Makefile '%s' might loop; not remaking it.\n"),
                     f->name));

                if (last)
                  last->next = d->next;
                else
                  read_files = d->next;

                if (d->error && ! (d->flags & RM_DONTCARE))
                  {
                    /* This file won't be rebuilt, was not found, and we care,
                       so remember it to report later.  */
                    d->next = skipped_makefiles;
                    skipped_makefiles = d;
                    any_failed = 1;
                  }
                else
                  free_goaldep (d);

                d = last ? last->next : read_files;
              }
          }
      }

      /* Set up 'MAKEFLAGS' specially while remaking makefiles.  */
      define_makeflags (1);

      {
        int orig_db_level = db_level;

        if (! ISDB (DB_MAKEFILES))
          db_level = DB_NONE;

        rebuilding_makefiles = 1;
        status = update_goal_chain (read_files);
        rebuilding_makefiles = 0;

        db_level = orig_db_level;
      }

      /* Report errors for makefiles that needed to be remade but were not.  */
      while (skipped_makefiles != NULL)
        {
          struct goaldep *d = skipped_makefiles;
          const char *err = strerror (d->error);

          OSS (error, &d->floc, _("%s: %s"), dep_name (d), err);

          skipped_makefiles = skipped_makefiles->next;
          free_goaldep (d);
        }

      /* If we couldn't build something we need but otherwise we succeeded,
         reset the status.  */
      if (any_failed && status == us_success)
        status = us_none;

      switch (status)
        {
        case us_question:
          /* The only way this can happen is if the user specified -q and asked
             for one of the makefiles to be remade as a target on the command
             line.  Since we're not actually updating anything with -q we can
             treat this as "did nothing".  */
          break;

        case us_none:
          {
             /* Reload any unloaded shared objects.  Do not re-exec to have
                that shared object loaded: a re-exec would cause an infinite
                loop, because the shared object was not updated.  */
            struct goaldep *d;

            for (d = read_files; d; d = d->next)
              if (d->file->unloaded)
                {
                  struct file *f = d->file;
                  /* Load the file.  0 means failure.  */
                  if (load_file (&d->floc, f, 0) == 0)
                    OS (fatal, &d->floc, _("%s: failed to load"), f->name);
                  f->unloaded = 0;
                  f->loaded = 1;
                }
          }

          /* No makefiles needed to be updated.  If we couldn't read some
             included file that we care about, fail.  */
          if (0)
            {
              /* This runs afoul of https://savannah.gnu.org/bugs/?61226
                 The problem is that many makefiles use a "dummy rule" to
                 pretend that an included file is rebuilt, without actually
                 rebuilding it, and this has always worked.  There are a
                 number of solutions proposed in that bug but for now we'll
                 put things back so they work the way they did before.  */
              struct goaldep *d;

              for (d = read_files; d != 0; d = d->next)
                if (d->error && ! (d->flags & RM_DONTCARE))
                  {
                    /* This makefile couldn't be loaded, and we care.  */
                    const char *err = strerror (d->error);
                    OSS (error, &d->floc, _("%s: %s"), dep_name (d), err);
                    any_failed = 1;
                  }
            }
          break;

        case us_failed:
          /* Failed to update.  Figure out if we care.  */
          {
            /* Nonzero if any makefile was successfully remade.  */
            int any_remade = 0;
            unsigned int i;
            struct goaldep *d;

            for (i = 0, d = read_files; d != 0; ++i, d = d->next)
              {
                if (d->file->updated)
                  {
                    /* This makefile was updated.  */
                    if (d->file->update_status == us_success)
                      /* It was successfully updated.  */
                      any_remade |= (file_mtime_no_search (d->file)
                                     != makefile_mtimes[i]);
                    else if (! (d->flags & RM_DONTCARE))
                      {
                        FILE_TIMESTAMP mtime;
                        /* The update failed and this makefile was not
                           from the MAKEFILES variable, so we care.  */
                        OS (error, &d->floc,
                            _("Failed to remake makefile '%s'."),
                            d->file->name);
                        mtime = file_mtime_no_search (d->file);
                        any_remade |= (mtime != NONEXISTENT_MTIME
                                       && mtime != makefile_mtimes[i]);
                        makefile_status = MAKE_FAILURE;
                        any_failed = 1;
                      }
                  }

                /* This makefile was not found at all.  */
                else if (! (d->flags & RM_DONTCARE))
                  {
                    const char *dnm = dep_name (d);

                    /* This is a makefile we care about.  See how much.  */
                    if (d->flags & RM_INCLUDED)
                      /* An included makefile.  We don't need to die, but we
                         do want to complain.  */
                      OS (error, &d->floc,
                          _("Included makefile '%s' was not found."), dnm);
                    else
                      {
                        /* A normal makefile.  We must die later.  */
                        OS (error, NILF, _("Makefile '%s' was not found"), dnm);
                        any_failed = 1;
                      }
                  }
              }

            if (any_remade)
              goto re_exec;

            break;
          }

        case us_success:
        re_exec:
          /* Updated successfully.  Re-exec ourselves.  */

          remove_intermediates (0);

          if (print_data_base_flag)
            print_data_base ();

          clean_jobserver (0);

          if (makefiles != 0)
            {
              /* Makefile names might have changed due to expansion.
                 It's possible we'll need one extra argument:
                   make -Rf-
                 will expand to:
                   make -R --temp-stdin=<tmpfile>
                 so allocate more space.
              */
              int mfidx = 0;
              char** av = argv;
              const char** nv;

              nv = nargv = alloca (sizeof (char*) * (argc + 1 + 1));
              *(nv++) = *(av++);

              for (; *av; ++av, ++nv)
                {
                  char *f;
                  char *a = *av;
                  const char *mf = makefiles->list[mfidx];

                  assert (strlen (a) > 0);

                  *nv = a;

                  /* Not an option: we handled option args earlier.  */
                  if (a[0] != '-')
                    continue;

                  /* See if this option specifies a filename.  If so we need
                     to replace it with the value from makefiles->list.

                     To simplify, we'll replace all possible versions of this
                     flag with a simple "-f<name>".  */

                  /* Handle long options.  */
                  if (a[1] == '-')
                    {
                      if (strcmp (a, "--file") == 0 || strcmp (a, "--makefile") == 0)
                        /* Skip the next arg as we'll combine them.  */
                        ++av;
                      else if (!strneq (a, "--file=", 7)
                               && !strneq (a, "--makefile=", 11))
                        continue;

                      if (mfidx == stdin_offset)
                        {
                          char *na = alloca (CSTRLEN ("--temp-stdin=")
                                             + strlen (mf) +  1);
                          sprintf (na, "--temp-stdin=%s", mf);
                          *nv = na;
                        }
                      else
                        {
                          char *na = alloca (strlen (mf) + 3);
                          sprintf (na, "-f%s", mf);
                          *nv = na;
                        }

                      ++mfidx;
                      continue;
                    }

                  /* Handle short options.  If 'f' is the last option, it may
                     be followed by <name>.  */
                  f = strchr (a, 'f');
                  if (!f)
                    continue;

                  /* If there's an extra argument option skip it.  */
                  if (f[1] == '\0')
                    ++av;

                  if (mfidx == stdin_offset)
                    {
                      const size_t al = f - a;
                      char *na;

                      if (al > 1)
                        {
                          /* Preserve the prior options.  */
                          na = alloca (al + 1);
                          memcpy (na, a, al);
                          na[al] = '\0';
                          *(nv++) = na;
                        }

                      /* Remove the "f" and any subsequent content.  */
                      na = alloca (CSTRLEN ("--temp-stdin=") + strlen (mf) + 1);
                      sprintf (na, "--temp-stdin=%s", mf);
                      *nv = na;
                    }
                  else if (f[1] == '\0')
                    /* -f <name> or -xyzf <name>.  Replace the name.  */
                    *(++nv) = mf;
                  else
                    {
                      /* -f<name> or -xyzf<name>. */
                      const size_t al = f - a + 1;
                      const size_t ml = strlen (mf) + 1;
                      char *na = alloca (al + ml);
                      memcpy (na, a, al);
                      memcpy (na + al, mf, ml);
                      *nv = na;
                    }

                  ++mfidx;
                }

              *nv = NULL;
            }

          if (directories != 0 && directories->idx > 0)
            {
              int bad = 1;
              if (directory_before_chdir != 0)
                {
                  if (chdir (directory_before_chdir) < 0)
                      perror_with_name ("chdir", "");
                  else
                    bad = 0;
                }
              if (bad)
                O (fatal, NILF,
                   _("Couldn't change back to original directory"));
            }

          ++restarts;

          if (ISDB (DB_BASIC))
            {
              const char **p;
              printf (_("Re-executing[%u]:"), restarts);
              for (p = nargv; *p != 0; ++p)
                printf (" %s", *p);
              putchar ('\n');
              fflush (stdout);
            }

#ifndef _AMIGA
          {
            char **p;
            for (p = environ; *p != 0; ++p)
              {
                if (strneq (*p, MAKELEVEL_NAME "=", MAKELEVEL_LENGTH+1))
                  {
                    *p = alloca (40);
                    sprintf (*p, "%s=%u", MAKELEVEL_NAME, makelevel);
#ifdef VMS
                    vms_putenv_symbol (*p);
#endif
                  }
                else if (strneq (*p, "MAKE_RESTARTS=", CSTRLEN ("MAKE_RESTARTS=")))
                  {
                    *p = alloca (40);
                    sprintf (*p, "MAKE_RESTARTS=%s%u",
                             OUTPUT_IS_TRACED () ? "-" : "", restarts);
                    restarts = 0;
                  }
              }
          }
#else /* AMIGA */
          {
            char buffer[256];

            sprintf (buffer, "%u", makelevel);
            SetVar (MAKELEVEL_NAME, buffer, -1, GVF_GLOBAL_ONLY);

            sprintf (buffer, "%s%u", OUTPUT_IS_TRACED () ? "-" : "", restarts);
            SetVar ("MAKE_RESTARTS", buffer, -1, GVF_GLOBAL_ONLY);
            restarts = 0;
          }
#endif

          /* If we didn't set the restarts variable yet, add it.  */
          if (restarts)
            {
              char *b = alloca (40);
              sprintf (b, "MAKE_RESTARTS=%s%u",
                       OUTPUT_IS_TRACED () ? "-" : "", restarts);
              putenv (b);
            }

          fflush (stdout);
          fflush (stderr);

          osync_clear();

          /* The exec'd "child" will be another make, of course.  */
          jobserver_pre_child(1);

#ifdef _AMIGA
          exec_command (nargv);
          exit (0);
#elif defined (__EMX__)
          {
            /* It is not possible to use execve() here because this
               would cause the parent process to be terminated with
               exit code 0 before the child process has been terminated.
               Therefore it may be the best solution simply to spawn the
               child process including all file handles and to wait for its
               termination. */
            pid_t pid;
            int r;
            struct childbase child;
            child.cmd_name = NULL;
            child.output.syncout = 0;
            child.environment = environ;

            pid = child_execute_job (&child, 1, (char **)nargv);

            /* is this loop really necessary? */
            do {
              pid = wait (&r);
            } while (pid <= 0);
            /* use the exit code of the child process */
            exit (WIFEXITED(r) ? WEXITSTATUS(r) : EXIT_FAILURE);
          }
#else
#ifdef SET_STACK_SIZE
          /* Reset limits, if necessary.  */
          if (stack_limit.rlim_cur)
            setrlimit (RLIMIT_STACK, &stack_limit);
#endif
          exec_command ((char **)nargv, environ);
#endif
          jobserver_post_child(1);

          temp_stdin_unlink ();

          _exit (127);
        }

      if (any_failed)
        die (MAKE_FAILURE);
    }

  /* Set up 'MAKEFLAGS' again for the normal targets.  */
  define_makeflags (0);

  /* Set always_make_flag if -B was given.  */
  always_make_flag = always_make_set;

  /* If restarts is set we haven't set up -W files yet, so do that now.  */
  if (restarts && new_files != 0)
    {
      const char **p;
      for (p = new_files->list; *p != 0; ++p)
        {
          struct file *f = enter_file (*p);
          f->last_mtime = f->mtime_before_update = NEW_MTIME;
        }
    }

  temp_stdin_unlink ();

  /* If there were no command-line goals, use the default.  */
  if (goals == 0)
    {
      char *p;

      if (default_goal_var->recursive)
        p = variable_expand (default_goal_var->value);
      else
        {
          p = variable_buffer_output (variable_buffer, default_goal_var->value,
                                      strlen (default_goal_var->value));
          *p = '\0';
          p = variable_buffer;
        }

      if (*p != '\0')
        {
          struct file *f = lookup_file (p);

          /* If .DEFAULT_GOAL is a non-existent target, enter it into the
             table and let the standard logic sort it out. */
          if (f == 0)
            {
              struct nameseq *ns;

              ns = PARSE_SIMPLE_SEQ (&p, struct nameseq);
              if (ns)
                {
                  /* .DEFAULT_GOAL should contain one target. */
                  if (ns->next != 0)
                    O (fatal, NILF,
                       _(".DEFAULT_GOAL contains more than one target"));

                  f = enter_file (strcache_add (ns->name));

                  ns->name = 0; /* It was reused by enter_file(). */
                  free_ns_chain (ns);
                }
            }

          if (f)
            {
              goals = alloc_goaldep ();
              goals->file = f;
            }
        }
    }
  else
    lastgoal->next = 0;


  if (!goals)
    {
      struct variable *v = lookup_variable (STRING_SIZE_TUPLE ("MAKEFILE_LIST"));
      if (v && v->value && v->value[0] != '\0')
        O (fatal, NILF, _("No targets"));

      O (fatal, NILF, _("No targets specified and no makefile found"));
    }

  /* Shuffle prerequisites to catch makefiles with incomplete depends. */

  shuffle_goaldeps_recursive (goals);

  /* Update the goals.  */

  DB (DB_BASIC, (_("Updating goal targets....\n")));

  {
    switch (update_goal_chain (goals))
    {
      case us_none:
        /* Nothing happened.  */
        /* FALLTHROUGH */
      case us_success:
        /* Keep the previous result.  */
        break;
      case us_question:
        /* We are under -q and would run some commands.  */
        makefile_status = MAKE_TROUBLE;
        break;
      case us_failed:
        /* Updating failed.  POSIX.2 specifies exit status >1 for this; */
        makefile_status = MAKE_FAILURE;
        break;
    }

    /* If we detected some clock skew, generate one last warning */
    if (clock_skew_detected)
      O (error, NILF,
         _("warning:  Clock skew detected.  Your build may be incomplete."));

    /* Exit.  */
    die (makefile_status);
  }

  /* NOTREACHED */
  exit (MAKE_SUCCESS);
}

/* Parsing of arguments, decoding of switches.  */

static char options[1 + sizeof (switches) / sizeof (switches[0]) * 3];
static struct option long_options[(sizeof (switches) / sizeof (switches[0])) +
                                  (sizeof (long_option_aliases) /
                                   sizeof (long_option_aliases[0]))];

/* Fill in the string and vector for getopt.  */
static void
init_switches (void)
{
  char *p;
  unsigned int c;
  unsigned int i;

  if (options[0] != '\0')
    /* Already done.  */
    return;

  p = options;

  /* Return switch and non-switch args in order, regardless of
     POSIXLY_CORRECT.  Non-switch args are returned as option 1.  */
  *p++ = '-';

  for (i = 0; switches[i].c != '\0'; ++i)
    {
      long_options[i].name = (char *) (switches[i].long_name == 0 ? "" :
                                       switches[i].long_name);
      long_options[i].flag = 0;
      long_options[i].val = switches[i].c;
      if (short_option (switches[i].c))
        *p++ = (char) switches[i].c;
      switch (switches[i].type)
        {
        case flag:
        case flag_off:
        case ignore:
          long_options[i].has_arg = no_argument;
          break;

        case string:
        case strlist:
        case filename:
        case positive_int:
        case floating:
          if (short_option (switches[i].c))
            *p++ = ':';
          if (switches[i].noarg_value != 0)
            {
              if (short_option (switches[i].c))
                *p++ = ':';
              long_options[i].has_arg = optional_argument;
            }
          else
            long_options[i].has_arg = required_argument;
          break;
        }
    }
  *p = '\0';
  for (c = 0; c < (sizeof (long_option_aliases) /
                   sizeof (long_option_aliases[0]));
       ++c)
    long_options[i++] = long_option_aliases[c];
  long_options[i].name = 0;
}


/* Non-option argument.  It might be a variable definition.  */
static void
handle_non_switch_argument (const char *arg, enum variable_origin origin)
{
  struct variable *v;

  if (arg[0] == '-' && arg[1] == '\0')
    /* Ignore plain '-' for compatibility.  */
    return;

#ifdef VMS
  {
    /* VMS DCL quoting can result in foo="bar baz" showing up here.
       Need to remove the double quotes from the value. */
    char * eq_ptr;
    char * new_arg;
    eq_ptr = strchr (arg, '=');
    if ((eq_ptr != NULL) && (eq_ptr[1] == '"'))
      {
         int len;
         int seg1;
         int seg2;
         len = strlen(arg);
         new_arg = alloca(len);
         seg1 = eq_ptr - arg + 1;
         strncpy(new_arg, arg, (seg1));
         seg2 = len - seg1 - 1;
         strncpy(&new_arg[seg1], &eq_ptr[2], seg2);
         new_arg[seg1 + seg2] = 0;
         if (new_arg[seg1 + seg2 - 1] == '"')
           new_arg[seg1 + seg2 - 1] = 0;
         arg = new_arg;
      }
  }
#endif
  v = try_variable_definition (0, arg, origin, 0);
  if (v != 0)
    {
      /* It is indeed a variable definition.  If we don't already have this
         one, record a pointer to the variable for later use in
         define_makeflags.  */
      struct command_variable *cv;

      for (cv = command_variables; cv != 0; cv = cv->next)
        if (cv->variable == v)
          break;

      if (! cv)
        {
          cv = xmalloc (sizeof (*cv));
          cv->variable = v;
          cv->next = command_variables;
          command_variables = cv;
        }
    }
  else if (arg[0] != '\0' && origin == o_command)
    {
      /* Not an option or variable definition; it must be a goal target.
         Enter it as a file and add it to the dep chain of goals.
         Check ARG[0] because if the top makefile resets MAKEOVERRIDES
         then ARG points to an empty string in the submake.  */
      struct file *f = enter_file (strcache_add (expand_command_line_file (arg)));
      f->cmd_target = 1;

      if (goals == 0)
        {
          goals = alloc_goaldep ();
          lastgoal = goals;
        }
      else
        {
          lastgoal->next = alloc_goaldep ();
          lastgoal = lastgoal->next;
        }

      lastgoal->file = f;

      {
        /* Add this target name to the MAKECMDGOALS variable. */
        struct variable *gv;
        const char *value;

        gv = lookup_variable (STRING_SIZE_TUPLE ("MAKECMDGOALS"));
        if (gv == 0)
          value = f->name;
        else
          {
            /* Paste the old and new values together */
            size_t oldlen, newlen;
            char *vp;

            oldlen = strlen (gv->value);
            newlen = strlen (f->name);
            vp = alloca (oldlen + 1 + newlen + 1);
            memcpy (vp, gv->value, oldlen);
            vp[oldlen] = ' ';
            memcpy (&vp[oldlen + 1], f->name, newlen + 1);
            value = vp;
          }
        define_variable_cname ("MAKECMDGOALS", value, o_default, 0);
      }
    }
}

/* Called if the makefile resets the MAKEFLAGS variable.  */
void
reset_makeflags (enum variable_origin origin)
{
  decode_env_switches (STRING_SIZE_TUPLE(MAKEFLAGS_NAME), origin);
  construct_include_path (include_dirs ? include_dirs->list : NULL);
  define_makeflags (rebuilding_makefiles);
}

/* Decode switches from ARGC and ARGV.
   They came from the environment if ORIGIN is o_env.  */

static void
decode_switches (int argc, const char **argv, enum variable_origin origin)
{
  int bad = 0;
  struct command_switch *cs;
  struct stringlist *sl;
  int c;

  /* getopt does most of the parsing for us.
     First, get its vectors set up.  */

  init_switches ();

  /* Let getopt produce error messages for the command line,
     but not for options from the environment.  */
  opterr = origin == o_command;
  /* Reset getopt's state.  */
  optind = 0;

  while (optind < argc)
    {
      const char *coptarg;

      /* Parse the next argument.  */
      c = getopt_long (argc, (char *const *)argv, options, long_options, NULL);
      coptarg = optarg;
      if (c == EOF)
        /* End of arguments, or "--" marker seen.  */
        break;
      else if (c == 1)
        /* An argument not starting with a dash.  */
        handle_non_switch_argument (coptarg, origin);
      else if (c == '?')
        /* Bad option.  We will print a usage message and die later.
           But continue to parse the other options so the user can
           see all he did wrong.  */
        bad = 1;
      else
        for (cs = switches; cs->c != '\0'; ++cs)
          if (cs->c == c)
            {
              /* Whether or not we will actually do anything with
                 this switch.  We test this individually inside the
                 switch below rather than just once outside it, so that
                 options which are to be ignored still consume args.  */
              int doit = (origin == o_command
                          || (cs->env &&
                              (cs->origin == NULL || origin >= *cs->origin)));

              if (doit)
                cs->specified = 1;

              switch (cs->type)
                {
                default:
                  abort ();

                case ignore:
                  break;

                case flag:
                case flag_off:
                  if (doit)
                    {
                      *(int *) cs->value_ptr = cs->type == flag;
                      if (cs->origin)
                        *cs->origin = origin;
                    }
                  break;

                case string:
                case strlist:
                case filename:
                  if (!doit)
                    break;

                  if (! coptarg)
                    coptarg = cs->noarg_value;
                  else if (*coptarg == '\0')
                    {
                      char opt[2] = "c";
                      const char *op = opt;

                      if (short_option (cs->c))
                        opt[0] = (char) cs->c;
                      else
                        op = cs->long_name;

                      error (NILF, strlen (op),
                             _("the '%s%s' option requires a non-empty string argument"),
                             short_option (cs->c) ? "-" : "--", op);
                      bad = 1;
                      break;
                    }

                  if (cs->type == string)
                    {
                      char **val = (char **)cs->value_ptr;
                      free (*val);
                      *val = xstrdup (coptarg);
                      if (cs->origin)
                        *cs->origin = origin;
                      break;
                    }

                  sl = *(struct stringlist **) cs->value_ptr;
                  if (sl == 0)
                    {
                      sl = xmalloc (sizeof (struct stringlist));
                      sl->max = 5;
                      sl->idx = 0;
                      sl->list = xmalloc (5 * sizeof (char *));
                      *(struct stringlist **) cs->value_ptr = sl;
                    }
                  else if (sl->idx == sl->max - 1)
                    {
                      sl->max += 5;
                      /* MSVC erroneously warns without a cast here.  */
                      sl->list = xrealloc ((void *)sl->list,
                                           sl->max * sizeof (char *));
                    }

                  /* Filter out duplicate options.
                   * Allow duplicate makefiles for backward compatibility.  */
                  if (cs->c != 'f')
                    {
                      unsigned int k;
                      for (k = 0; k < sl->idx; ++k)
                        if (streq (sl->list[k], coptarg))
                          break;
                      if (k < sl->idx)
                        break;
                    }

                  if (cs->type == strlist)
                    {
                      sl->list[sl->idx++] = xstrdup (coptarg);
                      if (cs->origin)
                        *cs->origin = origin;
                    }
                  else if (cs->c == TEMP_STDIN_OPT)
                    {
                      if (stdin_offset > 0)
                        fatal (NILF, 0, "INTERNAL: multiple --temp-stdin options provided!");
                      /* We don't need to expand the temp file.  */
                      stdin_offset = sl->idx;
                      sl->list[sl->idx++] = strcache_add (coptarg);
                      if (cs->origin)
                        *cs->origin = origin;
                    }
                  else
                    {
                      sl->list[sl->idx++] = expand_command_line_file (coptarg);
                      if (cs->origin)
                        *cs->origin = origin;
                    }
                  sl->list[sl->idx] = 0;
                  break;

                case positive_int:
                  /* See if we have an option argument; if we do require that
                     it's all digits, not something like "10foo".  */
                  if (coptarg == 0 && argc > optind)
                    {
                      const char *cp;
                      for (cp=argv[optind]; ISDIGIT (cp[0]); ++cp)
                        ;
                      if (cp[0] == '\0')
                        coptarg = argv[optind++];
                    }

                  if (!doit)
                    break;

                  if (coptarg)
                    {
                      const char *err;
                      unsigned int i = make_toui (coptarg, &err);

                      if (err || i == 0)
                        {
                          error (NILF, 0,
                                 _("the '-%c' option requires a positive integer argument"),
                                 cs->c);
                          bad = 1;
                        }
                      else
                        {
                          *(unsigned int *) cs->value_ptr = i;
                          if (cs->origin)
                            *cs->origin = origin;
                        }
                    }
                  else
                    {
                      *(unsigned int *) cs->value_ptr
                        = *(unsigned int *) cs->noarg_value;
                      if (cs->origin)
                        *cs->origin = origin;
                    }
                  break;

                case floating:
                  if (coptarg == 0 && optind < argc
                      && (ISDIGIT (argv[optind][0]) || argv[optind][0] == '.'))
                    coptarg = argv[optind++];

                  if (doit)
                    {
                      *(double *) cs->value_ptr = (coptarg != 0 ? atof (coptarg)
                                                   : *(double *) cs->noarg_value);
                      if (cs->origin)
                        *cs->origin = origin;
                    }

                  break;
                }

              /* We've found the switch.  Stop looking.  */
              break;
            }
    }

  /* There are no more options according to getting getopt, but there may
     be some arguments left.  Since we have asked for non-option arguments
     to be returned in order, this only happens when there is a "--"
     argument to prevent later arguments from being options.  */
  while (optind < argc)
    handle_non_switch_argument (argv[optind++], origin);

  if (bad && origin == o_command)
    print_usage (bad);

  /* If there are any options that need to be decoded do it now.  */
  decode_debug_flags ();
  decode_output_sync_flags ();

  /* Perform any special switch handling.  */
  run_silent = silent_flag;
}

/* Decode switches from environment variable ENVAR (which is LEN chars long).
   We do this by chopping the value into a vector of words, prepending a
   dash to the first word if it lacks one, and passing the vector to
   decode_switches.  */

static void
decode_env_switches (const char *envar, size_t len, enum variable_origin origin)
{
  char *varref = alloca (2 + len + 2);
  char *value, *p, *buf;
  int argc;
  const char **argv;

  /* Get the variable's value.  */
  p = varref;
  *(p++) = '$';
  *(p++) = '(';
  p = mempcpy (p, envar, len);
  *(p++) = ')';
  *p = '\0';
  value = variable_expand (varref);

  /* Skip whitespace, and check for an empty value.  */
  NEXT_TOKEN (value);
  len = strlen (value);
  if (len == 0)
    return;

  /* Allocate a vector that is definitely big enough.  */
  argv = alloca ((1 + len + 1) * sizeof (char *));

  /* getopt will look at the arguments starting at ARGV[1].
     Prepend a spacer word.  */
  argv[0] = "";
  argc = 1;

  /* We need a buffer to copy the value into while we split it into words
     and unquote it.  Set up in case we need to prepend a dash later.  */
  buf = alloca (1 + len + 1);
  buf[0] = '-';
  p = buf+1;
  argv[argc] = p;
  while (*value != '\0')
    {
      if (*value == '\\' && value[1] != '\0')
        ++value;                /* Skip the backslash.  */
      else if (ISBLANK (*value))
        {
          /* End of the word.  */
          *p++ = '\0';
          argv[++argc] = p;
          do
            ++value;
          while (ISBLANK (*value));
          continue;
        }
      *p++ = *value++;
    }
  *p = '\0';
  argv[++argc] = 0;
  assert (p < buf + len + 2);

  if (argv[1][0] != '-' && strchr (argv[1], '=') == 0)
    /* The first word doesn't start with a dash and isn't a variable
       definition, so add a dash.  */
    argv[1] = buf;

  /* Parse those words.  */
  decode_switches (argc, argv, origin);
}

/* Quote the string IN so that it will be interpreted as a single word with
   no magic by decode_env_switches; also double dollar signs to avoid
   variable expansion in make itself.  Write the result into OUT, returning
   the address of the next character to be written.
   Allocating space for OUT twice the length of IN is always sufficient.  */

static char *
quote_for_env (char *out, const char *in)
{
  while (*in != '\0')
    {
      if (*in == '$')
        *out++ = '$';
      else if (ISBLANK (*in) || *in == '\\')
        *out++ = '\\';
      *out++ = *in++;
    }

  return out;
}

/* Define the MAKEFLAGS and MFLAGS variables to reflect the settings of the
   command switches. Always include options with args.
   Don't include options with the 'no_makefile' flag set if MAKEFILE.  */

struct variable *
define_makeflags (int makefile)
{
  const char ref[] = "MAKEOVERRIDES";
  const char posixref[] = "-*-command-variables-*-";
  const char evalref[] = "$(-*-eval-flags-*-)";
  const struct command_switch *cs;
  struct variable *v;
  char *flagstring;
  char *p;

  /* We will construct a linked list of 'struct flag's describing
     all the flags which need to go in MAKEFLAGS.  Then, once we
     know how many there are and their lengths, we can put them all
     together in a string.  */

  struct flag
    {
      struct flag *next;
      const struct command_switch *cs;
      const char *arg;
    };
  struct flag *flags = 0;
  struct flag *last = 0;
  size_t flagslen = 0;
#define ADD_FLAG(ARG, LEN) \
  do {                                                                        \
    struct flag *new = alloca (sizeof (struct flag));                         \
    new->cs = cs;                                                             \
    new->arg = (ARG);                                                         \
    new->next = 0;                                                            \
    if (! flags)                                                              \
      flags = new;                                                            \
    else                                                                      \
      last->next = new;                                                       \
    last = new;                                                               \
    if (new->arg == 0)                                                        \
      /* Just a single flag letter: " -x"  */                                 \
      flagslen += 3;                                                          \
    else                                                                      \
      /* " -xfoo", plus space to escape "foo".  */                            \
      flagslen += 1 + 1 + 1 + (3 * (LEN));                                    \
    if (!short_option (cs->c))                                                \
      /* This switch has no single-letter version, so we use the long.  */    \
      flagslen += 2 + strlen (cs->long_name);                                 \
  } while (0)

  for (cs = switches; cs->c != '\0'; ++cs)
    if (cs->toenv && (!makefile || !cs->no_makefile))
      switch (cs->type)
        {
        case ignore:
          break;

        case flag:
        case flag_off:
          if ((!*(int *) cs->value_ptr) == (cs->type == flag_off)
              && (cs->default_value == NULL || cs->specified
                  || *(int *) cs->value_ptr != *(int *) cs->default_value))
            ADD_FLAG (0, 0);
          break;

        case positive_int:
          if ((cs->default_value != 0
               && (*(unsigned int *) cs->value_ptr
                   == *(unsigned int *) cs->default_value)))
            break;
          if (cs->noarg_value != 0
              && (*(unsigned int *) cs->value_ptr ==
                  *(unsigned int *) cs->noarg_value))
            ADD_FLAG ("", 0); /* Optional value omitted; see below.  */
          else
            {
              char *buf = alloca (30);
              sprintf (buf, "%u", *(unsigned int *) cs->value_ptr);
              ADD_FLAG (buf, strlen (buf));
            }
          break;

        case floating:
          if (cs->default_value != 0
              && (*(double *) cs->value_ptr == *(double *) cs->default_value))
            break;
          if (cs->noarg_value != 0
              && (*(double *) cs->value_ptr == *(double *) cs->noarg_value))
            ADD_FLAG ("", 0); /* Optional value omitted; see below.  */
          else
            {
              char *buf = alloca (100);
              sprintf (buf, "%g", *(double *) cs->value_ptr);
              ADD_FLAG (buf, strlen (buf));
            }
          break;

        case string:
          p = *((char **)cs->value_ptr);
          if (p)
            ADD_FLAG (p, strlen (p));
          break;

        case filename:
        case strlist:
          {
            struct stringlist *sl = *(struct stringlist **) cs->value_ptr;
            if (sl != 0)
              {
                unsigned int i;
                for (i = 0; i < sl->idx; ++i)
                  ADD_FLAG (sl->list[i], strlen (sl->list[i]));
              }
          }
          break;

        default:
          abort ();
        }

#undef  ADD_FLAG

  /* Four more for the possible " -- ", plus variable references.  */
  flagslen += 4 + CSTRLEN (posixref) + 4 + CSTRLEN (evalref) + 4;

  /* Construct the value in FLAGSTRING.
     We allocate enough space for a preceding dash and trailing null.  */
  flagstring = alloca (1 + flagslen + 1);
  memset (flagstring, '\0', 1 + flagslen + 1);
  p = flagstring;

  /* Start with a dash, for MFLAGS.  */
  *p++ = '-';

  /* Add simple options as a group.  */
  while (flags != 0 && !flags->arg && short_option (flags->cs->c))
    {
      *p++ = (char) flags->cs->c;
      flags = flags->next;
    }

  /* Now add more complex flags: ones with options and/or long names.  */
  while (flags)
    {
      *p++ = ' ';
      *p++ = '-';

      /* Add the flag letter or name to the string.  */
      if (short_option (flags->cs->c))
        *p++ = (char) flags->cs->c;
      else
        {
          /* Long options require a double-dash.  */
          *p++ = '-';
          p = stpcpy (p, flags->cs->long_name);
        }
      /* An omitted optional argument has an ARG of "".  */
      if (flags->arg && flags->arg[0] != '\0')
        {
          if (!short_option (flags->cs->c))
            /* Long options require '='.  */
            *p++ = '=';
          p = quote_for_env (p, flags->arg);
        }
      flags = flags->next;
    }

  /* If no flags at all, get rid of the initial dash.  */
  if (p == &flagstring[1])
    {
      flagstring[0] = '\0';
      p = flagstring;
    }

  /* Define MFLAGS before appending variable definitions.  Omit an initial
     empty dash.  Since MFLAGS is not parsed for flags, there is no reason to
     override any makefile redefinition.  */
  define_variable_cname ("MFLAGS",
                         flagstring + (flagstring[0] == '-' && flagstring[1] == ' ' ? 2 : 0),
                         o_env, 1);

  /* Write a reference to -*-eval-flags-*-, which contains all the --eval
     flag options.  */
  if (eval_strings)
    {
      *p++ = ' ';
      p = mempcpy (p, evalref, CSTRLEN (evalref));
    }

  {
    /* If there are any overrides to add, write a reference to
       $(MAKEOVERRIDES), which contains command-line variable definitions.
       Separate the variables from the switches with a "--" arg.  */

    const char *r = posix_pedantic ? posixref : ref;
    size_t l = strlen (r);
    v = lookup_variable (r, l);

    if (v && v->value && v->value[0] != '\0')
      {
        p = stpcpy (p, " -- ");
        *(p++) = '$';
        *(p++) = '(';
        p = mempcpy (p, r, l);
        *(p++) = ')';
      }
  }

  /* If there is a leading dash, omit it.  */
  if (flagstring[0] == '-')
    ++flagstring;

  /* This used to use o_env, but that lost when a makefile defined MAKEFLAGS.
     Makefiles set MAKEFLAGS to add switches, but we still want to redefine
     its value with the full set of switches.  Then we used o_file, but that
     lost when users added -e, causing a previous MAKEFLAGS env. var. to take
     precedence over the new one.  Of course, an override or command
     definition will still take precedence.  */
  v =  define_variable_cname (MAKEFLAGS_NAME, flagstring,
                              env_overrides ? o_env_override : o_file, 1);
  v->special = 1;

  return v;
}

/* Return 1 if the working directory change message should be printed.
   Otherwise, return 0.  */
int
should_print_dir (void)
{
    if (print_directory_flag >= 0)
        return print_directory_flag;

    /* If the user didn't specify any print-directory options, compute the
       default setting: disable under -s / print in sub-makes and under -C.  */
    return !silent_flag && (makelevel > 0 || directories != NULL);
}

/* Print version information.  */

static void
print_version (void)
{
  static int printed_version = 0;

  const char *precede = print_data_base_flag ? "# " : "";

  if (printed_version)
    /* Do it only once.  */
    return;

  printf ("%sGNU Make %s\n", precede, version_string);

  if (!remote_description || *remote_description == '\0')
    printf (_("%sBuilt for %s\n"), precede, make_host);
  else
    printf (_("%sBuilt for %s (%s)\n"),
            precede, make_host, remote_description);

  /* Print this untranslated.  The coding standards recommend translating the
     (C) to the copyright symbol, but this string is going to change every
     year, and none of the rest of it should be translated (including the
     word "Copyright"), so it hardly seems worth it.  */

  printf ("%sCopyright (C) 1988-2023 Free Software Foundation, Inc.\n",
          precede);

  printf (_("%sLicense GPLv3+: GNU GPL version 3 or later <https://gnu.org/licenses/gpl.html>\n\
%sThis is free software: you are free to change and redistribute it.\n\
%sThere is NO WARRANTY, to the extent permitted by law.\n"),
            precede, precede, precede);

  /* Print compiled features for debugging */
  print_compiled_features (precede);

  printed_version = 1;
}

/* Print compiled features for debugging purposes */
static void
print_compiled_features (const char *precede)
{
  printf ("%sCompiled features:\n", precede);

  /* System headers */
#ifdef HAVE_SYS_MMAN_H
  printf ("%s  ✓ sys/mman.h (memory mapping)\n", precede);
#else
  printf ("%s  ✗ sys/mman.h (memory mapping)\n", precede);
#endif

#ifdef HAVE_PTHREAD_H
  printf ("%s  ✓ pthread.h (POSIX threads)\n", precede);
#else
  printf ("%s  ✗ pthread.h (POSIX threads)\n", precede);
#endif

#ifdef HAVE_SYS_IOCTL_H
  printf ("%s  ✓ sys/ioctl.h (terminal control)\n", precede);
#else
  printf ("%s  ✗ sys/ioctl.h (terminal control)\n", precede);
#endif

#ifdef HAVE_DIRENT_H
  printf ("%s  ✓ dirent.h (directory operations)\n", precede);
#else
  printf ("%s  ✗ dirent.h (directory operations)\n", precede);
#endif

  /* Additional important features */
#ifdef HAVE_SHM_OPEN
  printf ("%s  ✓ shm_open (shared memory)\n", precede);
#else
  printf ("%s  ✗ shm_open (shared memory)\n", precede);
#endif

#ifdef HAVE_POSIX_SPAWN
  printf ("%s  ✓ posix_spawn (process spawning)\n", precede);
#else
  printf ("%s  ✗ posix_spawn (process spawning)\n", precede);
#endif

#ifdef MAKE_JOBSERVER
  printf ("%s  ✓ job server support\n", precede);
#else
  printf ("%s  ✗ job server support\n", precede);
#endif

#ifdef MAKE_LOAD
  printf ("%s  ✓ load average support\n", precede);
#else
  printf ("%s  ✗ load average support\n", precede);
#endif

#ifdef MAKE_SYMLINKS
  printf ("%s  ✓ symbolic link timestamp checking\n", precede);
#else
  printf ("%s  ✗ symbolic link timestamp checking\n", precede);
#endif

  /* Platform-specific features */
#ifdef WINDOWS32
  printf ("%s  ✓ Windows32 API\n", precede);
#else
  printf ("%s  ✗ Windows32 API\n", precede);
#endif

#ifdef VMS
  printf ("%s  ✓ VMS support\n", precede);
#else
  printf ("%s  ✗ VMS support\n", precede);
#endif

#ifdef _AMIGA
  printf ("%s  ✓ Amiga support\n", precede);
#else
  printf ("%s  ✗ Amiga support\n", precede);
#endif
}

/* Print a bunch of information about this and that.  */

static void
print_data_base (void)
{
  time_t when = time ((time_t *) 0);

  print_version ();

  printf (_("\n# Make data base, printed on %s"), ctime (&when));

  print_variable_data_base ();
  print_dir_data_base ();
  print_rule_data_base ();
  print_file_data_base ();
  print_vpath_data_base ();
  strcache_print_stats ("#");

  when = time ((time_t *) 0);
  printf (_("\n# Finished Make data base on %s\n"), ctime (&when));
}

static void
clean_jobserver (int status)
{
  /* Sanity: have we written all our jobserver tokens back?  If our
     exit status is 2 that means some kind of syntax error; we might not
     have written all our tokens so do that now.  If tokens are left
     after any other error code, that's bad.  */

  if (jobserver_enabled() && jobserver_tokens)
    {
      if (status != 2)
        ON (error, NILF,
            "INTERNAL: Exiting with %u jobserver tokens (should be 0)!",
            jobserver_tokens);
      else
        /* Don't write back the "free" token */
        while (--jobserver_tokens)
          jobserver_release (0);
    }


  /* Sanity: If we're the master, were all the tokens written back?  */

  if (master_job_slots)
    {
      /* We didn't write one for ourself, so start at 1.  */
      unsigned int tokens = 1 + jobserver_acquire_all ();

      if (tokens != master_job_slots)
        ONN (error, NILF,
             "INTERNAL: Exiting with %u jobserver tokens available; should be %u!",
             tokens, master_job_slots);

      reset_jobserver ();
    }
}

/* Exit with STATUS, cleaning up as necessary.  */

void
die (int status)
{
  static char dying = 0;

  if (!dying)
    {
      int err;
      struct timeval end_time;
      double duration_seconds;

      dying = 1;

      gettimeofday(&end_time, NULL);
      duration_seconds = (end_time.tv_sec - make_start_time.tv_sec) +
                         (end_time.tv_usec - make_start_time.tv_usec) / 1000000.0;
      debug_write(duration_seconds > 0.1 ? MEM_DEBUG_ERROR : MEM_DEBUG_MAX,
                  "[EXIT] die() called with status=%d (PID=%d, makelevel=%u, duration=%.3fs)\n",
                  status, getpid(), makelevel, duration_seconds);
      if (makelevel == 0) {
        save_memory_profiles ();
#if defined(HAVE_SYS_MMAN_H) && defined(HAVE_SHM_OPEN) && defined(HAVE_PTHREAD_H)
        cleanup_shared_memory ();
#endif
      }

      if (print_version_flag)
        print_version ();

      /* Get rid of a temp file from reading a makefile from stdin.  */
      temp_stdin_unlink ();

      /* Wait for children to die.  */
      err = (status != 0);
      while (job_slots_used > 0)
        reap_children (1, err);

      /* Let the remote job module clean up its state.  */
      remote_cleanup ();

      /* Remove the intermediate files.  */
      remove_intermediates (0);

      if (print_data_base_flag)
        print_data_base ();

      if (verify_flag)
        verify_file_data_base ();

      clean_jobserver (status);

      if (output_context)
        {
          /* die() might be called in a recipe output context due to an
             $(error ...) function.  */
          output_close (output_context);

          if (output_context != &make_sync)
            output_close (&make_sync);

          OUTPUT_UNSET ();
        }

      output_close (NULL);

      osync_clear ();

      /* Try to move back to the original directory.  This is essential on
         MS-DOS (where there is really only one process), and on Unix it
         puts core files in the original directory instead of the -C
         directory.  Must wait until after remove_intermediates(), or unlinks
         of relative pathnames fail.  */
      if (directory_before_chdir != 0)
        {
          /* If it fails we don't care: shut up GCC.  */
          int _x UNUSED;
          _x = chdir (directory_before_chdir);
        }
    }

  exit (status);
}
