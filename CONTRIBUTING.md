# Contributing to OpenMinerZ

OpenMinerZ is a private compatibility project. Repository access does not grant
permission to redistribute source, builds, imported content, protocol evidence,
or proprietary dependencies outside the approved team.

## Development setup

Follow the source-only quick start in `README.md`. To run the game, create an
ignored `CMakeUserPresets.json` from `CMakeUserPresets.example.json` and point
it at your own legally obtained Classic and Modern installations.

Never add local paths to `CMakeLists.txt` or `CMakePresets.json`. Machine-local
configuration belongs in `CMakeUserPresets.json` or environment variables.

## Change workflow

1. Create a focused branch from `main`.
2. Keep behavior changes separate from broad mechanical formatting.
3. Add deterministic coverage for gameplay, persistence, or protocol changes.
4. Build with warnings enabled and run the relevant CTest preset.
5. Manually verify renderer/UI work at more than one resolution.
6. Open a pull request using the repository template and explain the behavior,
   validation, compatibility risk, and rollback path.

Prefer small commits with imperative subjects, for example:

```text
Validate pre-presence gameplay packets
Fix remote avatar held-item transform
Document owned-content preset setup
```

## Code conventions

- C++ is C++20 and follows the checked-in `.clang-format` file.
- Use four spaces in C/C++; do not introduce tabs.
- Keep public contracts under `include/openminerz/` and implementation details
  under `src/`.
- Preserve deterministic fixed-step behavior. Rendering must not become a
  gameplay authority.
- Treat all network, save, imported-content, and IPC input as untrusted.
- Validate lengths, enums, ownership, authority, ordering, and spatial bounds
  before mutating state.
- Avoid platform-specific logic in the simulation when it belongs in an adapter.
- Do not silently weaken protocol checks to make one captured session pass.

Format touched C++ files before committing:

```powershell
clang-format -i path\to\changed_file.cpp path\to\changed_file.hpp
```

## Testing expectations

At minimum, run:

```powershell
cmake --build --preset native-dev --parallel
ctest --preset native-dev
```

Changes involving locally imported content or Steam interoperability must also
be verified with the appropriate owned-content preset. Live interoperability
tests use private sessions, owned clients, and consenting testers only.

For regressions, add a test that fails before the fix and passes after it when
the behavior can be expressed deterministically.

## Content and evidence policy

Do not commit:

- `steam_api.dll`, executables, PDBs, or other compiled third-party binaries
- CastleMiner Z `Content` trees, XNB/XACT files, textures, models, or audio
- imported or upscaled derivatives of proprietary content
- avatar exports or Microsoft/Xbox avatar resources
- world saves, settings, screenshots, crash dumps, logs, or packet captures
- Steam IDs, lobby IDs, usernames, IP addresses, machine names, or secrets
- files copied from another project without a compatible license and attribution

Keep raw evidence under ignored `artifacts/`, `captures/`, `traces/`, or `out/`
directories. Document the conclusion and reproducible method in source or docs;
do not commit the proprietary input.

Before staging a change, inspect it:

```powershell
git status --short
git diff --check
git diff --cached --stat
```

## Pull-request review

A pull request is ready for review when it has:

- a clear user-visible or technical outcome
- tests and manual verification proportional to risk
- no generated, proprietary, private, or machine-specific files
- updated documentation for new configuration or behavior
- an explicit note for save/protocol compatibility changes
- no unrelated worktree changes

Security-sensitive findings follow `SECURITY.md` and should not be filed as a
normal issue.
