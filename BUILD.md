# GNU Make Build Guide

This is the comprehensive build reference for GNU Make. It covers all build scenarios from released packages to Git development builds.

**When to use this guide:**
- Building from released packages (gnu.org downloads)
- Building from Git/development source
- Troubleshooting build issues
- Understanding autotools workflow

## Quick Commands

### Released Package (from gnu.org)
```bash
./configure && make && make install
```

### Git/Development Build
```bash
./bootstrap && ./configure && make && make install
```

## When to Regenerate Autotools

You need to regenerate autotools files when:
- Building from Git source
- After modifying `configure.ac`
- When configure script is missing
- Getting autotools-related errors

### Methods:
```bash
# For Git builds (recommended):
./bootstrap

# Alternative method:
autoreconf -fiv
```

## Common Issues

### Configure Fails
- Ensure you ran `./bootstrap` first (Git builds)
- Check prerequisites are installed
- Try `./configure --help` for options

### Make Fails  
- Try `sh build.sh` instead of `make`
- For Git builds: `make MAKE_MAINTAINER_MODE=`
- Check your make program is compatible

### Autotools Errors
- Run `./bootstrap` (Git) or `autoreconf -fiv`
- Ensure correct autotools versions

## Build Options

### Debug Build
```bash
./configure CFLAGS=-g && make
```

### Optimized Build  
```bash
./configure CFLAGS=-O2 && make
```

### Test Build
```bash
make check
```

## Prerequisites

### Released Packages
- C compiler (GCC recommended)
- Standard build tools

### Git Builds (additional)
- autoconf >= 2.69
- automake >= 1.16.1
- autopoint (gettext)
- texinfo
- pkg-config
- GNU Make

## Full Process

1. **Regenerate autotools** (Git builds only): `./bootstrap`
2. **Configure**: `./configure`
3. **Build**: `make`
4. **Test** (optional): `make check`
5. **Install**: `make install`
