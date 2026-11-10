# Contributing to Kirton

Thanks for helping improve Kirton. Keep changes focused and include a test or a short explanation when a behavior cannot be tested automatically.

## Before opening a change

1. Search existing issues and discussions before starting a large feature.
2. Use a focused branch and describe the user-visible behavior in the change summary.
3. Keep generated build directories, executables, archives, and local credentials out of commits.

## Build and test

Follow [`BUILDING.md`](BUILDING.md) to configure the Windows or Linux build. For a normal change, run the relevant CTest suite:

```sh
ctest --test-dir build --output-on-failure
```

On Windows with a multi-configuration generator, add `-C Release` (or the configuration you built). Clang is required for the full Native compiler test set. GitHub Actions runs the Windows and Linux build/test jobs for pushes and pull requests.

When changing language behavior, add a small `.kir` regression case under `kirton-c/tests/` and update the relevant reference documentation. Preserve AST behavior unless the change explicitly targets another execution mode; add parity coverage where practical.

## Code and documentation

- Follow the surrounding C11 style and use existing project utilities.
- Keep platform-specific code guarded and document new platform requirements.
- Add or retain `SPDX-License-Identifier: MPL-2.0` notices for new Kirton source files.
- Preserve the upstream notices for vendored components. Do not relabel third-party files as MPL-2.0.
- Update `kirton-c/CHANGELOG.md` and release documentation for user-visible changes.
- Do not commit generated binaries, build caches, logs, or release staging directories.

## Pull requests

Explain the motivation, user impact, compatibility implications, and verification performed. Link related issues and call out any known limitations. Do not include secrets, private keys, personal contact details, or unrelated local build output.

Contributions to Kirton source files are distributed under MPL-2.0. The full terms are in [`LICENSE`](LICENSE); third-party components retain their own terms in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). The root license and notice files mirror the copies in `kirton-c/`; keep those copies synchronized when updating license text or third-party inventory.
