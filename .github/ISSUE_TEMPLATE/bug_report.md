---
name: Bug report
about: Report a crash, incorrect result, or other defect
title: ''
labels: bug
assignees: ''
---

## What happened

A clear, concise description of the bug.

## Reproduction

Minimal program, command, or workload that triggers the bug. If the
issue only reproduces under `LD_PRELOAD`, please include the exact
command line.

```c
// minimal repro here
```

## Expected behavior

What you expected to happen instead.

## Environment

- v8malloc version (`v8m_version()` output or commit SHA):
- Architecture (`uname -m`):
- Kernel (`uname -r`):
- libc (e.g. `ldd --version | head -1`):
- Compiler (`cc --version`):
- Build configuration (debug / release, env vars `V8M_*` set):

## Additional context

Logs, stack traces, sanitizer output, `numastat` snapshots — anything
that helps diagnose the issue.

> Security-sensitive issues should follow [SECURITY.md](../SECURITY.md)
> instead of being reported here.
