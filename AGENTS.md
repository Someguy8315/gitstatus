# Versioning and releases

- The daemon version is `gitstatus_version` in `build.info`. Shell bindings check this version too; release the bindings and daemon from the same source revision.
- Bump once per intended release that changes daemon behavior, including fixes, performance changes, build/runtime requirements, or a libgit2 submodule update that changes the resulting daemon. Do not bump for every development commit or for documentation/test-only changes.
- Use a patch bump for compatible fixes and optimizations, a minor bump for backward-compatible features, and a major bump for incompatible protocol or supported-client changes. A platform-specific fix still increments the shared daemon version; do not introduce per-platform version counters.
- Include the bump in the implementation commit when practical. Amend only the latest unpublished commit and only when requested; never rewrite published history to add a bump.

## Platforms

- Development and verification happen on native Linux only. Keep the Windows-specific code (CMake build, `_WIN32` paths) intact and compiling in principle, but it cannot be built or tested here; say so explicitly when a change touches it rather than claiming it was verified.

## Release tags and publication

- Version bumps, tags, pushes, and GitHub releases are separate actions. Do not infer permission for one from permission for another. Create or publish tags/releases only when explicitly requested.
- Before tagging, inspect existing tags and remote refs to avoid collisions. Use an annotated `v<version>` tag for a source release, matching `build.info` exactly. Preserve upstream tags; do not overwrite a tag just because this fork uses the same version namespace.
- Before a release, require a clean source tree, a Linux `./build`, `python tests/test_index.py`, and `node --test tests/gitstatus.test.js` (1- and 8-thread protocol tests). Check `usrbin/gitstatusd --version` against `build.info`. Explain any skipped check before proceeding; do not silently waive a failed check.
- Publish any new libgit2 commit on an appropriate fork branch before pushing the gitstatus commit that pins it. Publish its fork release tag when requested. A detached submodule commit must become remotely reachable, not merely exist locally.
- Push only the explicitly selected branch and tags, not `git push --tags`. Published tags are immutable; use a new version/tag for corrections. Do not force-push or move published tags.
- The dotfiles `setup.sh` pins this fork by `GITSTATUS_REVISION` and builds it with `./build`. Update that pin only when deploying the release is in scope; a local fix or version bump alone does not authorize it.
