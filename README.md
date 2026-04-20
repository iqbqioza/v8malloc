# C23 Dev Environment

A devcontainer-based development environment for C23 projects.

## Prerequisites

- Docker
- VS Code with the Dev Containers extension

## Getting Started

1. Clone this repository
2. Open in VS Code
3. Select "Reopen in Container"

## Build

```bash
# Development (Clang + Debug)
make dev

# Release dynamic (GCC + Release)
make release

# Release static (GCC + Release)
make release-static

# Clean build artifacts
make clean
```

## Tools

### Compilers

| Tool  | Usage            |
|-------|------------------|
| Clang | Development      |
| GCC   | Release (Dynamic / Static) |

### Code Quality

```bash
# Format (overwrite)
make format

# Format check (dry run)
make format-check

# Static analysis (clang-tidy)
make tidy

# Static analysis (cppcheck)
make cppcheck

# Run all checks (format-check + tidy + cppcheck)
make lint
```

### Debugging

- gdb
- valgrind
- strace
- ltrace

## Git Hooks

Hooks are located in `.githooks/` and configured automatically via `postCreateCommand`.

- **pre-commit** - Checks clang-format on staged `.c` and `.h` files
- **commit-msg** - Enforces [Conventional Commits](https://www.conventionalcommits.org/) format

## Project Structure

```
.
├── .clang-format        # Clang-format config (Linux style)
├── .clang-tidy          # Clang-tidy config
├── .devcontainer/       # Dev container config
│   ├── Dockerfile
│   ├── devcontainer.json
│   └── post-create.sh
├── .editorconfig        # Editor config
├── .githooks/           # Git hooks
│   ├── commit-msg
│   └── pre-commit
├── CMakeLists.txt       # CMake build config
├── CMakePresets.json    # CMake presets
├── Makefile             # Build wrapper
└── src/                 # Source files
```

## License

This project is licensed under the [MIT License](LICENSE).
