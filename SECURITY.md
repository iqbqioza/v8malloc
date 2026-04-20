# Security Policy

## Supported versions

v8malloc has not yet reached a 1.0 release. While the API surface
stabilizes, only the latest commit on `main` is supported.

| Version | Supported |
| ------- | --------- |
| `main`  | ✅        |
| < 0.1   | ❌        |

Once the project ships its first stable release, this table will be
updated to list every supported branch and the date its support window
ends.

## Reporting a vulnerability

**Do not open a public GitHub issue for security vulnerabilities.**

Instead, please use GitHub's
[private vulnerability reporting](https://docs.github.com/en/code-security/security-advisories/guidance-on-reporting-and-writing-information-about-vulnerabilities/privately-reporting-a-security-vulnerability)
on this repository, or email the maintainers directly at the address
listed in the project metadata.

When reporting, please include:

- A description of the vulnerability and its impact (memory safety,
  privilege escalation, denial of service, …)
- Steps to reproduce, ideally with a minimal proof-of-concept
- The version (`v8m_version()` output or commit SHA) and platform
  (architecture, kernel version, libc) where it was observed
- Whether the issue is already public elsewhere

## Disclosure timeline

We aim to:

1. Acknowledge the report within **72 hours**.
2. Provide an initial assessment within **7 days**.
3. Coordinate a fix and disclosure window with the reporter; default
   embargo is **90 days** from acknowledgment.

Credit is given to reporters in the release notes unless they request
otherwise.

## Out of scope

- Reports against unsupported versions.
- Issues that require an attacker to already control the process (e.g.
  a malicious LD_PRELOAD).
- Performance regressions, unless they constitute a denial of service
  triggerable from untrusted input.
