## Summary

What does this PR change, and why?

## Design reference

Which section(s) of `.claude/docs/` does this implement, change, or
challenge? If you're modifying behavior that contradicts a doc, update
the doc in this PR.

## Checklist

- [ ] `make lint` (format-check + clang-tidy + cppcheck) passes
- [ ] `make test` passes
- [ ] If performance-sensitive: benchmark numbers attached or
      explained why the existing harness doesn't yet cover this path
- [ ] PR title follows Conventional Commits and a category label
      is set (`feat`, `fix`, `perf`, `docs`, `refactor`, `test`,
      `build`, `ci`, `chore`, `security`) — release notes are
      auto-generated from these
- [ ] Public API additions appear in
      `include/v8malloc/v8malloc.h` **and** `src/v8malloc.map`
- [ ] No new compiler warnings under `-Wall -Wextra -Wpedantic`

## Architectures touched

If this changes anything in `src/v8m_arch.h` or arch-specific code,
list which architectures you tested on and which still need coverage.

## Notes for reviewers

Anything that isn't obvious from the diff — design decisions, places
you'd particularly like a second opinion, follow-ups deferred to later
PRs.
