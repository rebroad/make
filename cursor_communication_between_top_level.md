# Communication between top-level and sub-makes
_Exported on 11/2/2025 at 19:38:23 GMT+1 from Cursor (2.0.34)_

---

**User**

How does the toplevel (makelevel==0) make and the sub-makes (makelevel>0) talk to each other?

---

**Cursor**


Searching the codebase for how top-level and sub-makes communicate.


Checking how makelevel is used and where sub-makes are invoked:


Reading the key source files to understand how communication works:


Checking how sub-makes are invoked and how environment variables are passed:


Reading `target_environment` and checking how the jobserver is set up:


Checking jobserver implementation and how it's passed via MAKEFLAGS:


Checking how MAKEFLAGS is constructed with jobserver info:


Overview of how top-level and sub-makes communicate:


How top-level (makelevel==0) and sub-makes (makelevel>0) communicate:

## 1. Environment variables

### MAKELEVEL — recursion depth
Top-level make initializes `MAKELEVEL` from the environment:

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

When spawning sub-makes, `target_environment()` increments `MAKELEVEL`:

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

If `MAKELEVEL` wasn’t found in the environment, it’s added:

```1231:1236:src/variable.c
  if (!found_makelevel)
    {
      char val[MAKELEVEL_LENGTH + 1 + INTSTR_LENGTH + 1];
      sprintf (val, "%s=%u", MAKELEVEL_NAME, makelevel + 1);
      *result++ = xstrdup (val);
    }
```

### MAKEFLAGS — command-line options
`MAKEFLAGS` carries flags and the jobserver auth info. It’s built in `define_makeflags()`:

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

Sub-makes read `MAKEFLAGS` from the environment during startup:

```1600:1603:src/main.c
   /* Set MAKEFLAGS's origin to command line: in submakes MAKEFLAGS will carry
      command line switches.  This causes env variable MAKEFLAGS to beat
      makefile modifications to MAKEFLAGS.  */
  decode_env_switches (STRING_SIZE_TUPLE (MAKEFLAGS_NAME), o_command);
```

## 2. Jobserver — parallel build coordination

For parallel builds (`-j`), a jobserver (pipe or named pipe/semaphore) is created and passed via `MAKEFLAGS`:

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

The jobserver auth string is included in `MAKEFLAGS` as `--jobserver-auth=<R,W>` (POSIX pipe) or `--jobserver-auth=fifo:PATH` (named pipe), allowing sub-makes to access the same jobserver pipe to coordinate parallelism.

When creating the environment for sub-makes, `target_environment()` may modify `MAKEFLAGS` if the jobserver needs invalidation:

```1163:1191:src/variable.c
        /* If we need to reset jobserver, check for MAKEFLAGS / MFLAGS.  */
        if (invalid)
          {
            if (!found_makeflags && streq (v->name, MAKEFLAGS_NAME))
              {
                char *mf;
                char *vars;
                found_makeflags = 1;

                if (!strstr (value, " --" JOBSERVER_AUTH_OPT "="))
                  goto setit;

                /* The invalid option must come before variable overrides.  */
                vars = strstr (value, " -- ");
                // ... modifies MAKEFLAGS to invalidate jobserver ...
              }
```

## 3. Exported variables

Any variable marked with `export` is passed via the environment. `target_environment()` builds the environment for sub-processes:

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

## 4. Command-line variable overrides

Variables set on the command line (e.g., `make CC=gcc`) are passed via `MAKEOVERRIDES`, which is included in `MAKEFLAGS`:

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

## Summary

- Environment variables: `MAKELEVEL` (recursion depth), `MAKEFLAGS` (options), and exported variables
- Jobserver: passed via `--jobserver-auth` in `MAKEFLAGS` for parallel build coordination
- Command-line variables: included in `MAKEOVERRIDES`, which is referenced in `MAKEFLAGS`
- No direct IPC: communication is via environment variables passed during sub-process creation

Sub-makes are independent processes; the parent doesn’t directly talk to them after spawning. They communicate via the inherited environment and the jobserver mechanism.

```c
  /* Figure out the level of recursion.  */
  {
    struct variable *v = lookup_variable (STRING_SIZE_TUPLE (MAKELEVEL_NAME));
    if (v && v->value[0] != '\0' && v->value[0] != '-')
      makelevel = make_toui (v->value, NULL);
    else
      makelevel = 0;
  }
```

```c
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

```c
  if (!found_makelevel)
    {
      char val[MAKELEVEL_LENGTH + 1 + INTSTR_LENGTH + 1];
      sprintf (val, "%s=%u", MAKELEVEL_NAME, makelevel + 1);
      *result++ = xstrdup (val);
    }
```

```c
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

```c
   /* Set MAKEFLAGS's origin to command line: in submakes MAKEFLAGS will carry
      command line switches.  This causes env variable MAKEFLAGS to beat
      makefile modifications to MAKEFLAGS.  */
  decode_env_switches (STRING_SIZE_TUPLE (MAKEFLAGS_NAME), o_command);
```

```c
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

```c
        /* If we need to reset jobserver, check for MAKEFLAGS / MFLAGS.  */
        if (invalid)
          {
            if (!found_makeflags && streq (v->name, MAKEFLAGS_NAME))
              {
                char *mf;
                char *vars;
                found_makeflags = 1;

                if (!strstr (value, " --" JOBSERVER_AUTH_OPT "="))
                  goto setit;

                /* The invalid option must come before variable overrides.  */
                vars = strstr (value, " -- ");
                // ... modifies MAKEFLAGS to invalidate jobserver ...
              }
```

```c
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

```c
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

---

**User**

What is "jobserver auth info"? What is the "jobserver mechanism"? Does it involve bidirectional communication?

---

**Cursor**


Searching for details on the jobserver mechanism and how it's implemented:


Examining the jobserver implementation:


Reading how jobserver_acquire reads tokens:


Checking how file descriptors are passed to sub-makes:


## 1. What is "jobserver auth info"?

Jobserver auth info describes how to access the jobserver pipe/semaphore. It's passed via the `--jobserver-auth` option in `MAKEFLAGS`.

On POSIX, it can be:
- Named pipe: `--jobserver-auth=fifo:/tmp/GMfifo12345` (file descriptor numbers via the named pipe)
- Simple pipe: `--jobserver-auth=4,5` (read FD, write FD)

On Windows, it's:
- Named semaphore: `--jobserver-auth=SEMAPHORE_NAME`

Sub-makes parse this from `MAKEFLAGS` to connect to the same jobserver:

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

## 2. What is the "jobserver mechanism"?

A token-based semaphore for coordinating parallel jobs across make processes. When you run `make -j4`, it:

- Creates the jobserver (pipe or named pipe/semaphore)
- Pre-loads it with tokens (one per available job slot)

```210:215:src/posixos.c
  while (slots--)
    {
      EINTRLOOP (r, write (job_fds[1], &token, 1));
      if (r != 1)
        pfatal_with_name (_("init jobserver pipe"));
    }
```

- Passes the auth info to sub-makes via `MAKEFLAGS`
- For recursive makes, inherits the pipe FDs:

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

/* Reconfigure the jobserver after starting a child process.  */
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

## 3. Does it involve bidirectional communication?

Yes. Processes read to acquire a token and write to release it, forming a pool shared across all participating makes.

### Acquiring a token (reading from pipe):
Before starting a job, make must read one character from the jobserver pipe:

```459:520:src/posixos.c
jobserver_acquire (int timeout)
{
  // ... setup ...
  while (1)
    {
      // ... select/pselect to wait for data ...
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

This blocks if no tokens are available, ensuring the parallelism limit.

### Releasing a token (writing to pipe):
When a job completes, make writes the token back:

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

### The token character:
Tokens are single characters. The default token is `'+'`:

```93:94:src/posixos.c
/* Token written to the pipe (could be any character...)  */
static char token = '+';
```

Important: The same character read must be written back; different characters may have different meanings.

## Summary

The jobserver is bidirectional: all makes (parent and children) read and write on the same pipe/semaphore to coordinate parallelism. This ensures that `make -j4` runs at most 4 jobs total across all make processes, not 4 per process. Without it, recursive makes would each run 4 jobs, causing over-parallelism.

The auth info lets sub-makes find and connect to the same jobserver, creating a distributed semaphore across the make process tree.

```c
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

```c
  while (slots--)
    {
      EINTRLOOP (r, write (job_fds[1], &token, 1));
      if (r != 1)
        pfatal_with_name (_("init jobserver pipe"));
    }
```

```c
void
jobserver_pre_child (int recursive)
{
  if (recursive && js_type == js_pipe)
    {
      fd_inherit (job_fds[0]);
      fd_inherit (job_fds[1]);
    }
}

/* Reconfigure the jobserver after starting a child process.  */
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

```c
jobserver_acquire (int timeout)
{
  // ... setup ...
  while (1)
    {
      // ... select/pselect to wait for data ...
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

```c
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

```c
  /* If we're using the jobserver and this child is not the only outstanding
     job, put a token back into the pipe for it.  */
  if (jobserver_enabled () && jobserver_tokens > 1)
    {
      jobserver_release (1);
      DB (DB_JOBS, (_("Released token for child %p (%s).\n"),
                    child, child->file->name));
    }
```

```c
/* Token written to the pipe (could be any character...)  */
static char token = '+';
```

