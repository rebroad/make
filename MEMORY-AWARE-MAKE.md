# Memory-Aware Make System Documentation

## Overview

This document describes the memory-aware job pausing system implemented in GNU Make. The system monitors memory usage of compilation processes and pauses job starts when memory is low, preventing system memory exhaustion while maintaining the user's requested parallel job count (`-j`).

## Architecture

### Process Hierarchy

```
Main Make Process (makelevel=0)
├── Starts memory monitoring thread
├── Tracks ALL descendant PIDs (compilers, sub-makes, etc.)
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
3. **PID Tracking** - Tracks descendant processes and their memory usage
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
  volatile unsigned long reserved_memory_mb;
  volatile int source_count;
  volatile int memory_profiles_dirty;
  struct {
    pid_t pid;
    char file_hash[41];     /* SHA1 hex string (40 chars + null) */
    unsigned long peak_mb;
    unsigned long current_mb;
  } source_files[MAX_TRACKED_COMPILATIONS];
  pthread_mutex_t source_mutex;
};
```

### Hash-to-Filename Mapping

```c
struct hash_filename_map {
  char hash[41];
  char *filename;
};
```

## How It Works

### 1. Initialization

The memory monitoring system is initialized only in the **top-level make process** (`makelevel == 0`):

```c
if (auto_adjust_jobs_flag && makelevel == 0 && job_slots > 0) {
    start_memory_monitor();
}
```

### 2. Memory Monitoring Thread

The background thread (`memory_monitor_thread_func`) continuously:

1. **Scans `/proc`** for descendant processes
2. **Tracks new processes** by parsing `/proc/PID/cmdline` and `/proc/PID/status`
3. **Monitors memory usage** via `/proc/PID/status` (RSS)
4. **Records source files** by extracting filenames from command lines
5. **Cleans up exited processes** and frees their slots

### 3. Process Discovery

The system discovers descendant processes by:

1. **Walking `/proc`** directory
2. **Reading `/proc/PID/status`** to find parent-child relationships
3. **Parsing `/proc/PID/cmdline`** to identify compilation processes
4. **Extracting source filenames** from compiler command lines

### 4. Memory Tracking

For each tracked process:

1. **Current memory usage** is read from `/proc/PID/status`
2. **Peak memory usage** is predicted based on historical data
3. **Memory reservations** are made to prevent overallocation
4. **Final memory usage** is recorded when processes exit

### 5. Source File Tracking

Source files are tracked by:

1. **Generating SHA1 hash** of the filename
2. **Storing hash-to-filename mapping** in main process
3. **Recording memory requirements** per source file
4. **Reusing slots** when processes exit

### 6. Job Pausing Logic

The system pauses job starts when memory is low and resumes when memory is available:

1. **Critical memory** (< 12% free) → Pause new job starts
2. **High memory** (< 20% free) → Continue with current jobs
3. **Comfortable memory** (> 70% free) → Resume normal job starts
4. **Emergency pause** (< 1GB free) → Pause all new jobs (`-j0`)

**Key Insight**: Instead of adjusting the `-j` value, the system maintains the user's requested parallel job count and simply pauses/resumes job starts based on available memory. This provides more predictable behavior and better performance.

### 7. Visual Memory Display

The system provides a real-time visual memory bar that shows:

```
⠋ ████████████░░░░░░░░ 62% (5905MB) -j1 5 procs
```

**Display Components:**
- **Spinner** (`⠋`) - Indicates active monitoring
- **Progress bar** (`████████████░░░░░░░░`) - Visual memory usage
- **Percentage** (`62%`) - Current memory usage
- **Memory amount** (`5905MB`) - Available memory
- **Job count** (`-j1`) - Current parallel jobs
- **Process count** (`5 procs`) - Active compilation processes

**Color Coding:**
- **Green** (`█`) - Used memory
- **Gray** (`.`) - Available memory
- **Dynamic updates** - Refreshes every few seconds

## Memory Management

### Memory Reservation System

The system maintains a **reserved memory counter** to prevent overallocation:

```c
volatile unsigned long reserved_memory_mb;
```

- **Reservations** are made when processes start
- **Releases** are made when processes exit
- **Total reservations** should not exceed available memory

### Memory Profile Persistence

Memory usage data is saved to disk for future predictions:

1. **File-based storage** of memory requirements per source file
2. **Historical data** used to predict peak memory usage
3. **Cross-session learning** improves predictions over time

## Thread Safety

### Shared Memory Access

All access to shared memory is protected by mutexes:

```c
pthread_mutex_lock(&shared_data->source_mutex);
// ... access shared data ...
pthread_mutex_unlock(&shared_data->source_mutex);
```

### Process-Safe Mutexes

The shared memory mutex is configured for process sharing:

```c
pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
```

## Configuration

### Environment Variables

The system can be configured via environment variables:

- **`MAKE_MEMORY_AWARE`**: Enable memory-aware job pausing
- **`MAKE_MEMORY_LIMIT`**: Maximum memory to use (MB)
- **`MAKE_MEMORY_PROFILES_DIR`**: Directory for memory profile storage

### Command Line Options

- **`--memory-aware`**: Enable memory-aware mode
- **`--memory-limit=N`**: Set memory limit to N MB
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

## Performance Considerations

### Overhead

The memory monitoring system adds minimal overhead:

- **Background thread** runs independently
- **Periodic scanning** of `/proc` (every few seconds)
- **Minimal memory usage** for tracking data

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

- Process discovery
- Memory tracking
- Slot allocation/deallocation
- Thread synchronization

### Common Debug Messages

```
[MEMORY] Tracking descendant PID 12345 -> src/main.c (current: 25MB, predicted peak: 50MB)
[MEMORY] Descendant PID 12345 exited, final peak for src/main.c: 45MB
[MEMORY] Released 50MB reservation for src/main.c (process exited)
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

## Conclusion

The memory-aware make system provides intelligent job pausing based on real-time memory usage monitoring. By tracking descendant processes and their memory requirements, it prevents system memory exhaustion while maintaining the user's requested parallel job count. The system includes a visual memory bar that provides real-time feedback on memory usage and build progress.

**Key Benefits of Job Pausing vs. Job Adjustment:**

1. **Predictable Behavior** - Users get the `-j` value they requested
2. **Better Performance** - Can still use high `-j` values when memory allows
3. **Simpler Logic** - No need to track and adjust job counts
4. **Cleaner Architecture** - Memory monitoring just controls when jobs start

The system is designed to be robust, scalable, and minimally intrusive, providing significant benefits for large-scale software builds without requiring changes to existing build systems.
