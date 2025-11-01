# Memory-Aware Make System Documentation

## Overview

This document describes the memory-aware job pausing system implemented in GNU Make. The system monitors memory usage of compilation processes and pauses job starts when memory is low, preventing system memory exhaustion while maintaining the user's requested parallel job count (`-j`).

## Architecture

### Process Hierarchy

```
Main Make Process (makelevel=0)
├── Starts memory monitoring thread
├── Uses existing `children` list as starting point
├── Tracks descendant PIDs (compilers, sub-makes, etc.) via targeted scanning
├── Monitors memory usage of each descendant
└── Adjusts job slots based on memory usage

Sub-Make Processes (makelevel>0)
├── Do NOT start memory monitoring
├── Do NOT track PIDs
└── Just compile files normally
```

### Key Components

1. **Memory Monitoring Thread** - Background thread that continuously monitors descendant processes
2. **Shared Memory System** - Inter-process communication for memory tracking data
3. **Optimized PID Tracking** - Uses `children` list as starting point for efficient descendant discovery
4. **Source File Tracking** - Tracks unique source files and their memory requirements
5. **Job Pausing Logic** - Pauses job starts when memory is low, resumes when memory is available

## Constants and Limits

### Memory Tracking Limits

```c
#define MAX_TRACKED_COMPILATIONS 100    /* Max concurrent compilations to track */
```

- **`MAX_TRACKED_COMPILATIONS`**: Tracks currently running compilations (1 PID + 1 source file per compilation)

### Why This Limit?

- **100 concurrent compilations**: Most systems can handle ~100 parallel compilation processes
- **Slot reuse**: When compilations finish, their slots are freed and can be reused
- **Handles any number of total compilations**: The limit is on concurrent compilations, not total compilations

## Data Structures

### Shared Memory Structure

```c
struct shared_memory_data {
  volatile unsigned long reserved_memory_mb;          /* Reserved peak memory for all active processes */
  volatile unsigned long current_compile_usage_mb;    /* Sum of current memory usage of all running compilations */
  pthread_mutex_t reserved_memory_mutex;              /* Mutex for reserved_memory_mb access */
  pthread_mutex_t current_usage_mutex;                /* Mutex for current_compile_usage_mb access */
};
```

### Memory Profile Structure

```c
struct memory_profile {
  char *filename;              /* Full path to source file */
  unsigned long peak_memory_mb; /* Peak memory usage for this file */
};

/* Global array of memory profiles (loaded from cache file) */
static struct memory_profile *memory_profiles = NULL;
static unsigned int memory_profiles_count = 0;
```

### Main-Make Monitoring Data

```c
/* Main-make-only monitoring data (not shared between processes) */
static struct {
  unsigned int compile_count;
  struct {
    pid_t pid;
    int profile_idx;           /* Index into memory_profiles array, -1 if no profile */
    unsigned long current_mb;  /* Current RSS from /proc/PID/status */
    unsigned long peak_mb;     /* Peak memory seen so far */
    unsigned long old_peak_mb; /* Previous peak from memory_profiles */
  } descendants[MAX_TRACKED_COMPILATIONS];
} main_monitoring_data;
```

## How It Works

### 1. Initialization

The memory monitoring system is initialized only in the **top-level make process** (`makelevel == 0`):

```c
if (auto_adjust_jobs_flag && makelevel == 0) {
    start_memory_monitor();
}
```

### 2. Memory Monitoring Thread

The background thread (`memory_monitor_thread_func`) continuously:

1. **Starts with `children` list** - Uses make's existing child process tracking
2. **Checks child memory** - Monitors direct children via `/proc/PID/status`
3. **Finds descendants** - Scans only processes with known children as parents
4. **Tracks new processes** by parsing `/proc/PID/cmdline` and `/proc/PID/status`
5. **Monitors memory usage** via `/proc/PID/status` (RSS)
6. **Records source files** by extracting filenames from command lines
7. **Cleans up exited processes** and frees their slots

### 3. Process Discovery (Optimized)

The system discovers descendant processes using an **optimized approach**:

1. **Starts with `children` list** - Uses make's existing `struct child *children` linked list
2. **Targeted scanning** - Only scans processes that are direct children of known processes
3. **Recursive discovery** - For each descendant found, recursively finds its descendants
4. **Reading `/proc/PID/status`** to find parent-child relationships
5. **Parsing `/proc/PID/cmdline`** to identify compilation processes
6. **Extracting source filenames** from compiler command lines

**Performance Benefits:**
- **10-100x faster** than scanning all of `/proc`
- **Surgical approach** - Only scans relevant processes
- **Scales with build size** - Not with total system processes

### 4. Memory Tracking

For each tracked process:

1. **Current memory usage** is read from `/proc/PID/status` (RSS field)
2. **Peak memory usage** is tracked and updated during compilation
3. **Memory reservations** are made based on historical profiles to prevent overallocation
4. **Final memory usage** is recorded when processes exit and saved to profiles
5. **Imminent memory** is calculated as the difference between reserved peak and current usage

### 5. Memory Profile System

Source files are tracked by:

1. **Profile lookup** - Each tracked compilation has a `profile_idx` that references `memory_profiles`
2. **Cache persistence** - Memory profiles are saved to `.make_memory_cache` file
3. **Historical data** - Peak memory usage is learned from previous compilations
4. **Initialization** - `profile_idx` is initialized to -1 when files are created, indicating no profile yet
5. **Profile assignment** - When starting a job, the profile is looked up by filename and assigned to `child->file->profile_idx`

### 6. Optimized Process Discovery Implementation

The system uses two helper functions for efficient process discovery:

**`check_child_memory_usage(pid_t child_pid)`:**
- Checks memory usage of a specific child process
- Updates or creates tracking entries for the child
- Handles both new and existing child processes

**`find_child_descendants(pid_t parent_pid)`:**
- Scans only processes with `parent_pid` as their parent
- Recursively finds descendants of descendants
- Extracts source filenames from command lines
- Updates memory tracking for all found descendants

**Main Loop:**
```c
for (c = children; c != 0; c = c->next) {
    if (c->pid > 0) {
        check_child_memory_usage(c->pid);
        find_child_descendants(c->pid);
    }
}
```

### 7. Job Pausing Logic

The system pauses job starts when memory is low and resumes when memory is available:

1. **Check imminent memory** - Calculate expected additional memory usage from reservations
2. **Calculate effective free** - `effective_free = free_mb - imminent_mb`
3. **Compare requirements** - Check if `required_mb <= effective_free`
4. **Pause if needed** - Wait 100ms and retry if not enough memory
5. **Proceed when ready** - Reserve memory and start job when conditions are met

**Key Insight**: The system uses **imminent memory** (reserved peak - current usage) to predict how much more memory will be needed as active compilations approach their peaks. This prevents overallocation before the peak usage is reached.

### 8. Visual Memory Display

The system provides a real-time visual memory bar that shows:

```
⠋ ████████████░░░░░░░░ 62% (5905MB) -j1 5 procs
```

**Display Components:**
- **Spinner** (`⠋`) - Indicates active monitoring
- **Progress bar** (`████████████░░░░░░░░`) - Visual memory usage
- **Memory amount** (`5905MB`) - Available memory
- **Job count** (`-j1`) - Current parallel jobs
- **Process count** (`5 procs`) - Active compilation processes

**Color Coding:**
- **Purple** (`█`) - Make-tracked memory (current usage by compilations)
- **Green** (`█`) - Other used memory (system processes)
- **Yellow** (`░`) - Imminent memory (predicted additional usage)
- **Gray** (`░`) - Available memory
- **Dynamic updates** - Refreshes every 300ms for smooth animation

**TTY Safety Features:**
- **Terminal width detection** using `ioctl(TIOCGWINSZ)` with state preservation
- **Cached width** - Terminal width is determined once at thread start and cached
- **Graceful degradation** - Disables display if terminal width cannot be obtained
- **Conditional formatting** - Only uses cursor positioning if both stderr and stdout are TTYs
- **Non-blocking writes** - Uses private duplicated file descriptor (`dup(STDERR_FILENO)`)
- **State restoration** - Saves and restores terminal state after width detection

**Thread Safety:**
- **No mutexes in display** - All values are calculated before calling `display_memory_status()`
- **Parameters passed** - `make_usage_mb` and `imminent_mb` are pre-calculated by the monitor thread
- **Lock-free rendering** - Display function performs no shared memory access

### 9. Debug System

The system includes a comprehensive debug logging system:

**`debug_write()` Function:**
- **Thread-safe** non-blocking debug output
- **Uses private file descriptor** to avoid blocking main process
- **Consistent formatting** for all memory-related messages
- **Filename context** included in memory reserve/release messages

**Debug Message Types:**
- `[DEBUG]` - General debugging information
- `[MEMORY]` - Memory tracking and reservation messages
- `[MONITOR]` - Memory monitor thread status
- `[PREDICT]` - Memory prediction and job start decisions with color coding:
  - Green: OK - Enough memory available
  - Blue: PROCEEDING - Started after waiting
  - Purple: WAITING - Pausing due to low memory
- `[SIGNAL]` - Signal handling (OOM kills, etc.)
- `[EXIT]` - Process exit information

**Example Debug Output:**
```
[DEBUG] Reserved memory: 0 MB -> 15 MB (+15 MB) for src/main.c (PID=12345, makelevel=0)
[MEMORY] PID=12345 Compilation exited, final peak for src/main.c: 15MB -> 25MB
[MEMORY] Released 15MB reservation for src/main.c (job completed) (PID=12345, makelevel=0)
[MONITOR] Using private fd=5 (dup of stderr=2), term_width=80
[PREDICT] PID=12345 src/main.c: needs 15MB, have 8192MB free (150MB imminent) - OK
[PREDICT] PID=12345 src/main.c: needs 256MB, only 100MB free (200MB imminent) - WAITING
[SIGNAL] fatal_error_signal called: sig=9 (Killed) PID=12345 makelevel=0
[EXIT] die() called with status=137 (PID=12345, makelevel=0)
```

## Memory Management

### Memory Reservation System

The system maintains a **reserved memory counter** to prevent overallocation:

```c
volatile unsigned long reserved_memory_mb;
```

**Reservation Process:**
- **Reservations** are made when processes start using `reserve_memory_mb(mb, filepath)`
- **Releases** are made when processes exit via `reap_children()`
- **Thread-safe** - Uses `reserved_memory_mutex` to protect shared counter
- **Filename context** is included in all reservation messages for better tracking
- **PID and makelevel** are logged with every reservation for debugging sub-makes

**Example Reservation Messages:**
```
[MEMORY] Reserved memory: 0 MB -> 15 MB (+15 MB) for src/main.c (PID=12345, makelevel=0)
[MEMORY] Released 15MB reservation for src/main.c (job completed) (PID=12345, makelevel=0)
```

### Memory Profile Persistence

Memory usage data is saved to disk for future predictions:

1. **Cache file** - `.make_memory_cache` stores memory profiles for all tracked files
2. **Historical data** - Peak memory usage from previous compilations
3. **Cross-session learning** - Predictions improve as more data is collected
4. **Rate-limited saves** - Dirty flag triggers saves every 10 seconds maximum
5. **Sub-make cleanup** - Only top-level make saves profiles to avoid race conditions

## Thread Safety

### Shared Memory Access

All access to shared memory is protected by separate mutexes:

```c
/* For reserved_memory_mb */
pthread_mutex_lock(&shared_data->reserved_memory_mutex);
// ... access reserved_memory_mb ...
pthread_mutex_unlock(&shared_data->reserved_memory_mutex);

/* For current_compile_usage_mb */
pthread_mutex_lock(&shared_data->current_usage_mutex);
// ... access current_compile_usage_mb ...
pthread_mutex_unlock(&shared_data->current_usage_mutex);
```

### Process-Safe Mutexes

The shared memory mutexes are configured for process sharing:

```c
pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
```

This allows sub-makes to safely access shared memory counters from separate processes.

## Configuration

### Environment Variables

The system can be configured via environment variables:

- **`MAKE_MEMORY_AWARE`**: Enable memory-aware job pausing
- **`MAKE_MEMORY_PROFILES_DIR`**: Directory for memory profile storage

### Command Line Options

- **`--memory-aware`**: Enable memory-aware mode
- **`--memory-profiles-dir=DIR`**: Set profiles directory

## Error Handling

### Graceful Degradation

If memory monitoring fails:

1. **Fallback to normal make** behavior
2. **Log warnings** about monitoring failures
3. **Continue compilation** without memory tracking

### Common Failure Modes

1. **Shared memory creation fails** → Disable monitoring
2. **Thread creation fails** → Disable monitoring
3. **Process tracking fails** → Continue with reduced functionality

### Profile Index Initialization

Critical to prevent sub-make crashes:

- **`profile_idx`** is initialized to **-1** when `struct file` objects are created
- This prevents sub-makes from attempting to release memory from profile index 0
- **Two locations** for initialization:
  - `src/file.c`: `enter_file()` uses `xcalloc` which zero-initializes, then explicitly sets `profile_idx = -1`
  - `src/implicit.c`: `pattern_search()` stack-allocates `struct file` with `memset` and then sets `profile_idx = -1`

## Performance Optimizations

### Process Discovery Optimization

The system uses an **optimized process discovery algorithm** that dramatically improves performance:

**Before (Naive Approach):**
- Scanned **ALL** of `/proc` (potentially thousands of processes)
- Did this multiple times (up to 5 iterations)
- Checked every single process to see if it was a descendant

**After (Optimized Approach):**
- Starts with make's existing `children` list (only direct children)
- For each child, does targeted scanning to find its descendants
- Only scans processes that are direct children of processes we're already tracking
- Recursively finds descendants of descendants

**Performance Impact:**
- **10-100x faster** depending on system size
- **Scales with build size** rather than total system processes
- **Surgical precision** - only scans relevant processes

**Why This Matters:**
- **Large systems** with thousands of processes see dramatic speedup
- **Build performance** improves as monitoring overhead is reduced
- **Resource efficiency** - only uses CPU cycles on relevant processes
- **Scalability** - works well on both small and large systems

## Performance Considerations

### Overhead

The memory monitoring system adds minimal overhead:

- **Background thread** runs independently
- **Optimized process scanning** - Only scans children and their descendants (not all `/proc`)
- **Targeted discovery** - Uses make's existing `children` list as starting point
- **Minimal memory usage** for tracking data
- **10-100x faster** than naive `/proc` scanning approach

### Scalability

The system scales well with:

- **Large numbers of total compilations** (unlimited due to slot reuse)
- **Many parallel compilations** (up to 100 concurrent)
- **Long compilation sessions** (slots are reused as compilations finish)

### User Experience

The visual memory bar provides several benefits:

1. **Real-time feedback** - Users can see memory usage at a glance
2. **Build progress** - Visual indication of compilation progress
3. **Memory awareness** - Clear understanding of system resource usage
4. **Debugging aid** - Helps identify memory-intensive compilation phases
5. **Professional appearance** - Modern, informative build output

## Debugging

### Debug Output

Enable debug output with:

```c
#define DEBUG_MEMORY_MONITOR 1
```

This provides detailed logging of:

- Process discovery and PID tracking
- Memory tracking with filename context
- Slot allocation/deallocation with PID counting
- Thread synchronization
- TTY-safe display operations

### Common Debug Messages

**Memory Reservation Messages:**
```
[MEMORY] Reserved memory: 0 MB -> 15 MB (+15 MB) for src/main.c (PID=12345, makelevel=0)
[MEMORY] Released 15MB reservation for src/main.c (job completed) (PID=12345, makelevel=0)
```

**Memory Tracking Messages:**
```
[MEMORY] PID=12345 Compilation exited, final peak for src/main.c: 15MB -> 25MB
[MEMORY] Released 15MB reservation for src/main.c (job completed) (PID=12345, makelevel=0)
```

**Process Discovery Messages:**
```
[DEBUG] New descendant[2] PID=534794 makelevel=0 rss=3408KB, profile_peak=15MB (file: src/main.c)
[DEBUG] New irrelevant descendant[-1] PID=534776 makelevel=0 rss=3396KB (first time seen)
```

**Monitor Status Messages:**
```
[MONITOR] Using private fd=5 (dup of stderr=2), term_width=80
[MONITOR] Could not obtain terminal width, disabling memory display
[DEBUG] Total jobs found: 5, total make memory: 512MB
```

**Reap Children Messages:**
```
[MEMORY] reap_children: PID=12345 makelevel=0 profile_idx=5, memory_profiles=0x7f8a4c001000
[MEMORY] Released 128MB reservation for src/main.c (job completed) (PID=12345, makelevel=0)
[MEMORY] ERROR: reap_children: memory_profiles is NULL but profile_idx=3 (PID=12345, makelevel=0)
```

## Future Improvements

### Potential Enhancements

1. **Machine learning** for better memory predictions
2. **Network-aware** monitoring for distributed builds
3. **GPU memory** tracking for CUDA/OpenCL builds
4. **Container-aware** monitoring for Docker builds

### Known Limitations

1. **Linux-specific** (relies on `/proc` filesystem)
2. **Single-machine** (doesn't work with distributed builds)
3. **Compiler-specific** (relies on command line parsing)

## Sub-Make Memory Handling

### Initialization

Sub-makes (makelevel > 0) do NOT start memory monitoring threads, but they DO participate in shared memory:

- **Shared memory** is initialized via `init_shared_memory()` when `reserve_memory_mb()` or `get_imminent_memory_mb()` is first called
- **Existing shared memory** is reused from the parent make process
- **No reset** - Sub-makes do NOT clear shared memory counters (only top-level make resets them)
- **Process-safe mutexes** allow sub-makes to safely access shared counters from separate processes

### Memory Reservations

Sub-makes participate in memory reservations:

1. **Before starting a job** - Sub-make calls `reserve_memory_mb()` with the predicted peak memory
2. **Shared counter updated** - `reserved_memory_mb` is incremented via process-safe mutex
3. **When job exits** - Sub-make's `reap_children()` calls `reserve_memory_mb()` with negative value
4. **Shared counter decremented** - Memory is released for the parent make to see

### Profile Handling

Sub-makes handle memory profiles correctly:

- **Profile lookup** - `get_file_profile_idx()` looks up filename in `memory_profiles` array
- **Lazy loading** - Profiles are loaded from cache file on first lookup if not already loaded
- **Assignment** - `profile_idx` is assigned to `child->file->profile_idx` when job starts
- **Release** - `reap_children()` uses `profile_idx` to release the correct reservation amount
- **No saves** - Only top-level make saves profiles to avoid race conditions

### Error Prevention

Critical safeguards to prevent sub-make crashes:

- **`profile_idx = -1`** - Prevents using uninitialized profile index
- **NULL checks** - Verifies `memory_profiles != NULL` before accessing array
- **Debug logging** - PID and makelevel included in all debug messages
- **Graceful degradation** - Falls back to estimation if profile lookup fails

## Conclusion

The memory-aware make system provides intelligent job pausing based on real-time memory usage monitoring. By using an optimized process discovery algorithm that starts with make's existing `children` list and performs targeted scanning, it efficiently tracks descendant processes and their memory requirements. This prevents system memory exhaustion while maintaining the user's requested parallel job count. The system includes a TTY-safe visual memory bar that provides real-time feedback on memory usage and build progress.

**Key Benefits:**

1. **Imminent Memory Prediction** - Uses `reserved_peak - current_usage` to predict additional memory needs
2. **Thread-Safe Display** - No mutexes in rendering; all values pre-calculated by monitor thread
3. **Sub-Make Cooperation** - Separate processes safely share counters via process-safe mutexes
4. **Profile-Based Predictions** - Historical data improves accuracy over time
5. **Graceful Error Handling** - Extensive debug logging with PID and makelevel for troubleshooting
6. **Optimized Discovery** - Uses existing `children` list for 10-100x faster process scanning

The system is designed to be robust, scalable, and minimally intrusive, providing significant benefits for large-scale software builds without requiring changes to existing build systems. It gracefully handles OOM conditions and provides clear diagnostic information when problems occur.
