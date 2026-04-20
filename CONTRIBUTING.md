# Contributing to v8malloc

Thanks for your interest. v8malloc is an early-stage project and
contributions of any size are welcome — bug reports, design feedback,
benchmark runs on architectures we don't yet have CI for, and code.

## Ground rules

- Be respectful. The [Code of Conduct](CODE_OF_CONDUCT.md) applies to
  every interaction in the project's spaces.
- Security issues do **not** belong in public GitHub issues. See
  [SECURITY.md](SECURITY.md) for the disclosure path.
- The design specification lives under `.claude/docs/`. If your change
  contradicts a design doc, update the doc in the same change.

## Development environment

The project ships a devcontainer that pins the toolchain we test
against (GCC 13+, Clang 16+, CMake 3.25+, cppcheck 2.x, clang-tidy
matching the Clang version).

```bash
# In VS Code: "Reopen in Container" — done.
# Otherwise:
docker build -t v8malloc-dev .devcontainer
docker run --rm -it -v "$PWD":/workspaces/v8malloc -w /workspaces/v8malloc \
    v8malloc-dev bash
```

## Build, test, lint

```bash
make            # debug build
make test       # run tests via ctest
make lint       # format-check + clang-tidy + cppcheck
```

Every patch must pass `make lint` and `make test` before review.

## Coding style

- Linux-kernel style, enforced by `clang-format`. Run `make format`
  before each commit (a pre-commit hook checks staged files).
- C23 standard, no GCC/Clang extensions unless wrapped behind the
  architecture abstraction layer in `src/v8m_arch.h`.
- Identifier conventions: `lower_snake_case` for functions and
  variables, `UPPER_SNAKE_CASE` for macros, `v8m_` prefix on every
  internal symbol, `V8M_` prefix on every macro. Public headers add the
  `V8M_EXPORT` visibility attribute.
- No object headers; metadata lives in per-page structures (see
  `.claude/docs/architecture.md` §4).
- Don't write comments that describe **what** the code does; describe
  **why** when it isn't obvious. The design docs are the canonical
  reference for the **what**.

## Commit messages

Commits follow the [Conventional Commits](https://www.conventionalcommits.org/)
specification, validated by the `commit-msg` hook in `.githooks/`. The
hook activates automatically when the devcontainer's
`postCreateCommand` runs. Outside the devcontainer:

```bash
git config core.hooksPath .githooks
```

Subject line conventions:

- ≤72 characters
- Format `type(optional-scope)!: Capitalized description`
- No trailing period
- Body, when present, separated by a blank line and wrapped at 72
  columns

## Pull requests

- Branch from `main`, keep PRs focused.
- Link the design doc section your change relates to.
- For performance changes, attach `make test` output and at least one
  benchmark run from `bench/` (or describe why the existing harness
  doesn't yet cover the affected path).
- Update `CHANGELOG.md` under `## [Unreleased]`.

## Architecture support

Tier 1 architectures (x86_64, aarch64) are tested in CI on every PR.
Tier 2 (riscv64, ppc64le) and Tier 3 (s390x, loongarch64) run on a
slower cadence. If your change touches arch-specific code, please test
on the affected architecture or call it out in the PR description so
reviewers know to schedule a manual run.
