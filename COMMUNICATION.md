# How Top-Level Make and Sub-Makes Communicate

This document details the communication mechanisms between the top-level make process (`makelevel==0`) and sub-make processes (`makelevel>0`) in GNU Make.

## Overview

GNU Make uses **environment variables** as the primary communication channel between parent and child make processes. There is no direct inter-process communication (IPC) mechanism like sockets or shared memory for routine communication. However, for parallel builds, make uses a **jobserver** mechanism implemented via pipes or named pipes/semaphores.

## Table of Contents

1. [Environment Variables](#environment-variables)
   - [MAKELEVEL](#makelevel)
   - [MAKEFLAGS](#makeflags)
2. [Exported Variables](#exported-variables)
3. [Command-Line Variable Overrides](#command-line-variable-overrides)
4. [The Jobserver Mechanism](#the-jobserver-mechanism)
   - [What is Jobserver Auth Info?](#what-is-jobserver-auth-info)
   - [How the Jobserver Works](#how-the-jobserver-works)
   - [Token-Based Coordination](#token-based-coordination)
   - [Bidirectional Communication](#bidirectional-communication)
5. [Implementation Details](#implementation-details)

---

## Environment Variables

### MAKELEVEL

The `MAKELEVEL` variable tracks the recursion depth of make invocations.

#### Initialization

When make starts, it reads `MAKELEVEL` from the environment to determine its recursion level:

```1692:1699:src/main.c
  /* Figure out the level of recursion.  */
  {
    struct variable *v = lookup_variable (STRING_SIZE_TUPLE (MAKELEVEL_NAME));
    if (v && v->value[0] != '\0' && v->value[0] != '-')
      makelevel = make_toui (v->value, NULL);
    else
      makelevel = 0;
  }
```

- If `MAKELEVEL` exists in the environment and is valid, make uses that value
- If not present or invalid, make sets `makelevel = 0` (top-level)

#### Incrementing for Sub-Makes

When creating the environment for sub-processes, `target_environment()` increments `MAKELEVEL`:

```1152:1161:src/variable.c
        /* If this is MAKELEVEL, update it.  */
        if (!found_makelevel && streq (v->name, MAKELEVEL_NAME))
          {
            char val[INTSTR_LENGTH + 1];
            sprintf (val, "%u", makelevel + 1);
            free (cp);
            value = cp = xstrdup (val);
            found_makelevel = 1;
            goto setit;
          }
```

If `MAKELEVEL` wasn't found in the existing variables, it's added explicitly:

```1231:1236:src/variable.c
  if (!found_makelevel)
    {
      char val[MAKELEVEL_LENGTH + 1 + INTSTR_LENGTH + 1];
      sprintf (val, "%s=%u", MAKELEVEL_NAME, makelevel + 1);
      *result++ = xstrdup (val);
    }
```

**Example:**
- Top-level make: `MAKELEVEL=0`
- First sub-make: `MAKELEVEL=1`
- Sub-sub-make: `MAKELEVEL=2`

### MAKEFLAGS

The `MAKEFLAGS` variable contains command-line options and is automatically passed to sub-makes through the environment.

#### Construction

`MAKEFLAGS` is constructed by the `define_makeflags()` function, which collects all command-line switches marked for export:

```3466:3689:src/main.c
/* Define the MAKEFLAGS and MFLAGS variables to reflect the settings of the
   command switches. Always include options with args.
   Don't include options with the 'no_makefile' flag set if MAKEFILE.  */

struct variable *
define_makeflags (int makefile)
{
  // ... constructs flagstring with all switches including jobserver-auth ...
  v =  define_variable_cname (MAKEFLAGS_NAME, flagstring,
                              env_overrides ? o_env_override : o_file, 1);
  v->special = 1;

  return v;
}
```

#### Reading in Sub-Makes

Sub-makes parse `MAKEFLAGS` from the environment during startup:

```1600:1603:src/main.c
   /* Set MAKEFLAGS's origin to command line: in submakes MAKEFLAGS will carry
      command line switches.  This causes env variable MAKEFLAGS to beat
      makefile modifications to MAKEFLAGS.  */
  decode_env_switches (STRING_SIZE_TUPLE (MAKEFLAGS_NAME), o_command);
```

**Example:**
If you run `make -j4 -k`, sub-makes will receive:
```
MAKEFLAGS=-j4 -k
```

This ensures sub-makes inherit the same options (like `-j` for parallel builds, `-k` for keep-going, etc.).

---

## Exported Variables

Any variable marked with the `export` keyword is passed through the environment to sub-processes. The `target_environment()` function builds the complete environment for child processes:

```1045:1244:src/variable.c
/* Create a new environment for FILE's commands.
   If FILE is nil, this is for the 'shell' function.
   The child's MAKELEVEL variable is incremented.
   If recursive is true then we're running a recursive make, else not.  */

char **
target_environment (struct file *file, int recursive)
{
  // ... collects all exportable variables ...
  // ... updates MAKELEVEL ...
  // ... modifies MAKEFLAGS if needed for jobserver ...
  // ... returns environment array ...
}
```

**Variables are exportable if:**
- They're explicitly marked with `export` in the makefile
- They were originally defined in the environment
- They're special variables like `MAKEFLAGS`, `MAKELEVEL`, etc.

---

## Command-Line Variable Overrides

Variables set on the command line (e.g., `make CC=gcc`) are passed to sub-makes via the `MAKEOVERRIDES` variable, which is referenced in `MAKEFLAGS`:

```3656:3672:src/main.c
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
```

**Example:**
Running `make -j4 CC=gcc CFLAGS=-O2` results in:
```
MAKEFLAGS=-j4 -- $(MAKEOVERRIDES)
MAKEOVERRIDES=CC=gcc CFLAGS=-O2
```

---

## The Jobserver Mechanism

The jobserver is a **token-based semaphore system** used to coordinate parallel job execution across all make processes (parent and children). It prevents over-parallelization when sub-makes are involved.

### What is Jobserver Auth Info?

**Jobserver auth info** describes how to access the jobserver pipe or semaphore. It's passed via the `--jobserver-auth` option in `MAKEFLAGS`.

#### POSIX Systems

On POSIX systems, there are two implementations:

1. **Named Pipe (FIFO)** - Preferred on systems that support it:
   ```
   --jobserver-auth=fifo:/tmp/GMfifo12345
   ```
   The path points to a named pipe (FIFO) that all make processes can open.

2. **Simple Pipe** - Fallback or when explicitly requested:
   ```
   --jobserver-auth=4,5
   ```
   Where `4` is the read file descriptor and `5` is the write file descriptor.

The auth string is generated by `jobserver_get_auth()`:

```302:316:src/posixos.c
char *
jobserver_get_auth ()
{
  char *auth;

  if (js_type == js_fifo) {
    auth = xmalloc (strlen (fifo_name) + CSTRLEN (FIFO_PREFIX) + 1);
    sprintf (auth, FIFO_PREFIX "%s", fifo_name);
  } else {
    auth = xmalloc ((INTSTR_LENGTH * 2) + 2);
    sprintf (auth, "%d,%d", job_fds[0], job_fds[1]);
  }

  return auth;
}
```

#### Windows Systems

On Windows, the jobserver uses a named semaphore:
```
--jobserver-auth=SEMAPHORE_NAME
```

### How the Jobserver Works

When you run `make -j4`:

1. **Jobserver Creation**: The top-level make creates a jobserver (pipe or named pipe/semaphore):

```146:223:src/posixos.c
jobserver_setup (int slots, const char *style)
{
  // ... creates pipe or named pipe ...
  
  /* Pre-load the pipe with tokens (one per available job slot). */
  while (slots--)
    {
      EINTRLOOP (r, write (job_fds[1], &token, 1));
      if (r != 1)
        pfatal_with_name (_("init jobserver pipe"));
    }
  
  return 1;
}
```

2. **Token Pre-loading**: The pipe is pre-loaded with tokens representing available job slots. For `-j4`, it writes 3 tokens (the parent make keeps one slot for itself).

3. **Auth Info Passing**: The jobserver auth info is included in `MAKEFLAGS` and passed to sub-makes:

```2217:2227:src/main.c
  if (job_slots > 1 && jobserver_setup (job_slots - 1, jobserver_style))
    {
      /* Fill in the jobserver_auth for our children.  */
      jobserver_auth = jobserver_get_auth ();

      if (jobserver_auth)
        {
          /* We're using the jobserver so set job_slots to 0.  */
          master_job_slots = job_slots;
          job_slots = 0;
        }
    }
```

4. **FD Inheritance (for simple pipes)**: For recursive makes using simple pipes, the file descriptors are inherited:

```411:428:src/posixos.c
void
jobserver_pre_child (int recursive)
{
  if (recursive && js_type == js_pipe)
    {
      fd_inherit (job_fds[0]);
      fd_inherit (job_fds[1]);
    }
}

void
jobserver_post_child (int recursive)
{
  if (recursive && js_type == js_pipe)
    {
      fd_noinherit (job_fds[0]);
      fd_noinherit (job_fds[1]);
    }
}
```

5. **Sub-Make Connection**: Sub-makes parse the auth info from `MAKEFLAGS` and connect to the same jobserver:

```225:300:src/posixos.c
jobserver_parse_auth (const char *auth)
{
  /* Parse the --jobserver-auth string and open the appropriate FDs */
  // ... opens named pipe or uses FD numbers from simple pipe ...
}
```

### Token-Based Coordination

The jobserver uses a **token-based semaphore** pattern:

- **Tokens** are single characters (default: `'+'`)
- Each token represents permission to run one job
- Tokens circulate through the pipe/semaphore

```93:94:src/posixos.c
/* Token written to the pipe (could be any character...)  */
static char token = '+';
```

**Important Rule:** When you release a token, you must write back the **same character** you read. Different characters may have different meanings to GNU Make.

### Bidirectional Communication

Yes, the jobserver involves **bidirectional communication**. All make processes (parent and children) read from and write to the same pipe/semaphore.

#### Acquiring a Token (Reading)

Before starting a job, a make process must read one character from the jobserver pipe:

```459:520:src/posixos.c
jobserver_acquire (int timeout)
{
  struct timespec spec;
  struct timespec *specp = NULL;
  sigset_t empty;

  sigemptyset (&empty);

  if (timeout)
    {
      spec.tv_sec = 1;
      spec.tv_nsec = 0;
      specp = &spec;
    }

  while (1)
    {
      fd_set readfds;
      int r;
      char intake;

      FD_ZERO (&readfds);
      FD_SET (job_fds[0], &readfds);

      r = pselect (job_fds[0]+1, &readfds, NULL, NULL, specp, &empty);
      // ... error handling ...

      /* The read FD is ready: read it!  This is non-blocking.  */
      EINTRLOOP (r, read (job_fds[0], &intake, 1));

      if (r < 0)
        {
          /* Someone sniped our token!  Try again.  */
          if (errno == EAGAIN)
            continue;

          pfatal_with_name (_("read jobs pipe"));
        }

      /* read() should never return 0: only the parent make can reap all the
         tokens and close the write side...??  */
      return r > 0;
    }
}
```

This blocks if no tokens are available, ensuring the parallelism limit is respected.

Called before starting a job:

```1870:1894:src/job.c
        start_waiting_jobs ();

        /* If our "free" slot is available, use it; we don't need a token.  */
        if (!jobserver_tokens)
          break;

        /* There must be at least one child already, or we have no business
           waiting for a token. */
        if (!children)
          O (fatal, NILF, "INTERNAL: no children as we go to sleep on read");

        /* Get a token.  */
        got_token = jobserver_acquire (waiting_jobs != NULL);

        /* If we got one, we're done here.  */
        if (got_token == 1)
          {
            DB (DB_JOBS, (_("Obtained token for child %p (%s).\n"),
                          c, c->file->name));
            break;
          }
      }

  ++jobserver_tokens;
```

#### Releasing a Token (Writing)

When a job completes, the make process writes the token back to the pipe:

```367:378:src/posixos.c
void
jobserver_release (int is_fatal)
{
  int r;
  EINTRLOOP (r, write (job_fds[1], &token, 1));
  if (r != 1)
    {
      if (is_fatal)
        pfatal_with_name (_("write jobserver"));
      perror_with_name ("write", "");
    }
}
```

Called when a child finishes:

```1137:1145:src/job.c
  /* If we're using the jobserver and this child is not the only outstanding
     job, put a token back into the pipe for it.  */
  if (jobserver_enabled () && jobserver_tokens > 1)
    {
      jobserver_release (1);
      DB (DB_JOBS, (_("Released token for child %p (%s).\n"),
                    child, child->file->name));
    }
```

### Why This Matters

Without the jobserver, if you run `make -j4` and a sub-make also runs with parallelism, you could end up with:
- 4 jobs from the parent make
- Plus 4 jobs from each sub-make
- Resulting in potentially 8, 12, or more concurrent jobs

With the jobserver:
- All make processes share the same token pool
- `make -j4` means **at most 4 jobs total** across all make processes
- Sub-makes participate in the same jobserver, so they coordinate together

---

## Implementation Details

### Jobserver Setup Flow

1. Top-level make calls `jobserver_setup(job_slots - 1, style)`
2. Creates pipe or named pipe
3. Pre-loads with tokens
4. Gets auth string via `jobserver_get_auth()`
5. Includes auth in `MAKEFLAGS` via `define_makeflags()`
6. Passes `MAKEFLAGS` to sub-makes via environment

### Sub-Make Startup Flow

1. Sub-make reads `MAKEFLAGS` from environment
2. Parses `--jobserver-auth=...` option
3. Calls `jobserver_parse_auth()` to connect
4. For simple pipes: uses inherited file descriptors
5. For named pipes: opens the named pipe path
6. Participates in token-based coordination

### Token Lifecycle

```
[Pipe with 3 tokens: +++]
   ↓ (make starts job 1)
[Pipe with 2 tokens: ++]
   ↓ (make starts job 2)
[Pipe with 1 token: +]
   ↓ (make starts job 3)
[Pipe empty, blocks]
   ↓ (job 1 finishes, writes token back)
[Pipe with 1 token: +]
   ↓ (make can start job 4)
```

---

## Summary

- **Environment Variables**: Primary communication channel (`MAKELEVEL`, `MAKEFLAGS`, exported variables)
- **Jobserver**: Token-based semaphore for coordinating parallelism across all make processes
- **Bidirectional**: All makes read and write to the same pipe/semaphore
- **No Direct IPC**: Beyond the jobserver, communication is one-way (parent → child via environment)
- **Coordination**: Ensures `make -j4` means 4 jobs total, not 4 per make process

The jobserver mechanism is essential for correct parallel execution when recursive makes are involved, preventing over-parallelization and ensuring efficient resource usage.

