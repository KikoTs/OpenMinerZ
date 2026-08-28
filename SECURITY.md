# Security policy

## Reporting

Report vulnerabilities privately through this repository's GitHub Security
Advisories. Do not open a normal issue or attach an exploit, packet capture,
save, Steam identifier, access token, or proprietary game file to a discussion.

Include only the minimum sanitized information needed to reproduce the issue:

- affected commit and build configuration
- attack preconditions and trust boundary
- expected and observed behavior
- a minimal source-level reproducer when possible
- impact and any known mitigation

The maintainers will acknowledge the report, reproduce it, assess affected
branches, and coordinate a fix before disclosure. Because the project is
private and pre-release, public disclosure timing is decided case by case.

## Security-sensitive areas

Give extra scrutiny to changes involving:

- Steam lobby/session admission and peer identity
- packet framing, decompression, payload codecs, and ownership checks
- host authority, late-join bootstrap, and replay ordering
- named pipes and the x64/x86 Steam bridge boundary
- world saves, imported content, and filesystem path handling
- original-client interoperability tooling

All external input is untrusted. Parsers must remain bounded, fail closed, and
avoid partial state mutation on rejection.

## Supported version

Only the current `main` branch is supported during pre-release development.
Older commits and locally modified builds may receive no security fixes.
