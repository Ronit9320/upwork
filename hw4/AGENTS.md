# AGENTS.md - Coding Guidelines for hw4 Project

## Build Commands
- **Build release**: `make`
- **Build debug**: `make debug`
- **Clean**: `make clean`
- **Single file compile**: `gcc -Wall -Werror -Wno-unused-function -MMD -D_DEFAULT_SOURCE -std=gnu11 -I include -c -o build/file.o src/file.c`

## Test Commands
- **Run tests**: Tests are currently commented out in Makefile. To enable: uncomment lines 36 and 50-52 in Makefile, then `make`
- **Single test**: Not currently supported (tests use Criterion framework but are disabled)

## Code Style Guidelines

### Language & Standards
- **Language**: C with GNU11 standard (`-std=gnu11`)
- **Compiler**: GCC with flags: `-Wall -Werror -Wno-unused-function -MMD -D_DEFAULT_SOURCE`

### Naming Conventions
- **Functions**: snake_case (`ccheck`, `die`, `parse_args`, `spawn_display_if_needed`)
- **Variables**: snake_case (`g_disp_pid`, `cfg`, `line`, `mover`)
- **Global variables**: snake_case with `g_` prefix (`g_disp_pid`, `g_eng_in`, `g_tx`)
- **Structs/Types**: PascalCase (`Config`, `Board`, `Player`, `Move`)
- **Constants**: ALL_CAPS with `#define` (`X`, `O`, `MAXPLY`, `MAXEVAL`)
- **Macros**: ALL_CAPS (`DEBUG`, `INFO`, `WARN`, `ERROR`, `SUCCESS`)

### Formatting & Structure
- **Indentation**: Tabs (not spaces)
- **Braces**: Opening brace on same line as function/struct declaration
- **Includes**: System headers first, then local headers (blank line separator)
- **Function attributes**: Use `__attribute__((format(printf,1,2)))` for printf-like functions
- **Comments**: Multi-line `/* */` for function docs, `//` for inline comments

### Types & Declarations
- **Custom types**: Use `typedef` for clarity (`typedef unsigned int Player;`)
- **Structs**: Define with typedef, no trailing semicolon after closing brace
- **Enums**: Use `#define` for constants rather than enum
- **Function pointers**: Declare with full signature

### Error Handling
- **Fatal errors**: Use `die()` function which prints to stderr and calls `exit(EXIT_FAILURE)`
- **Non-fatal**: Use `info()`, `warn()`, `error()` macros (defined in debug.h)
- **Signal handling**: Use `volatile sig_atomic_t` for signal flags
- **Resource cleanup**: Always clean up file descriptors, pipes, and child processes

### Memory & Resources
- **Memory allocation**: Use standard C functions (`malloc`, `free`, `fmemopen`)
- **File handling**: Always check return values, use `fclose()` for cleanup
- **Process management**: Use `fork()`, `exec*()`, `waitpid()` with proper error checking
- **Signal safety**: Avoid non-async-signal-safe functions in signal handlers

### Imports & Dependencies
- **Local headers**: Use `#include "header.h"` with relative paths
- **System headers**: Use `#include <header.h>`
- **Order**: System headers first, then local headers, separated by blank line
- **Guards**: Use `#ifndef HEADER_H` / `#define HEADER_H` / `#endif` in headers

### Best Practices
- **Const correctness**: Use `const` for read-only parameters and pointers
- **Static functions**: Use `static` for internal functions not exported from TU
- **Global variables**: Minimize use, prefer function parameters
- **Magic numbers**: Define as named constants
- **Buffer safety**: Always check buffer sizes, use `sizeof()` appropriately
- **IPC**: Use line-buffered FILE* streams for inter-process communication