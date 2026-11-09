# libgit2 dependency

This independent repository preserves the history of `romkatv/gitstatus`; it is
not a GitHub fork. `upstream` points to that project and `origin` points to
`Someguy8315/gitstatus`.

`build.info` pins the **`1.9.7-gitstatus`** tag archive from
[Someguy8315/libgit2](https://github.com/Someguy8315/libgit2/tree/1.9.7-gitstatus)
and its SHA-256. That repository's `gitstatus` branch contains the read-only
gitstatusd extensions as ordinary commits on top of upstream **libgit2 1.9.7**,
the latest stable release when checked on 2026-11-09. The build verifies the
archive checksum and compiles it directly: there is no local patch file or
submodule. The tag belongs to the libgit2 repository, not this one.

The original dependency was `romkatv/libgit2` at
`2ecf33948a4df9ef45a66c68b8ef24a5e60eaac6`, based on libgit2 1.1.0.

For future upgrades, update the libgit2 branch, publish a new tag without moving
an existing one, and update the tag and archive checksum in `build.info`.

## Build and test

```sh
./build -w
make test
```

Build prerequisites: a C/C++ toolchain, CMake, Make, Git, tar, wget, and a
SHA-256 utility. The tests additionally require Node.js and Git. They use only
Node's standard library. `TMPDIR` can select a scratch directory for both steps.

The executable is `usrbin/gitstatusd`. It remains ignored by Git; source changes,
rather than a machine-specific binary, are committed. The daemon's
protocol version remains `v1.5.5`. No installer, shell configuration, or p10k
configuration needs to be changed to build or test it.

The protocol tests exercise long-lived daemons with **1 and 8 threads**:

- Non-repositories, unborn branches, clean and dirty repositories, detached HEAD.
- Staged additions/deletions, unstaged changes/deletions, intent-to-add, conflicts.
- Index invalidation after staging/committing, index v4, `index.skipHash`.
- Default bounded counts and unlimited counts, including parallel diff shards
  over 1,200 files and repeated cached queries.
- Ignore negations across directories, empty/ignored-only directories, directory
  versus file typechanges, and lexicographic range boundaries.
- Tracking/push remotes, URL/config refresh, ahead/behind, stashes, packed tags.
- Linked worktrees, submodules, symlinks, direct `.git` queries, shallow clones,
  case-insensitive index ordering, skip-worktree and assume-unchanged bits.
- Native support for `extensions.noop` and `extensions.preciousObjects`.

`GITSTATUS_TEST_DAEMON=/absolute/path/to/gitstatusd make test` selects a different
binary for comparisons. The original binary passes the original core tests but
fails the new repeated `index.skipHash` update test.

## Retained / adapted patches

These are private extensions for a **read-only daemon**, not a general-purpose
libgit2 ABI. Do not install this patched library as a system libgit2 or use it
for repository mutations.

| Area | Why it remains |
| --- | --- |
| Diff range bounds | `range_start` / `range_end` split staged and workdir scans among threads. |
| Exemplar notifications | Per-delta-type early termination preserves independently bounded status counts without constructing result vectors. |
| Diff callback correctness | Notify only after classifying directory emptiness and file/tree typechanges. Upstream's provisional notifications are unsuitable when callbacks discard deltas. |
| Intent-to-add | Exclude index intent-to-add entries from staged counts and report their workdir additions. |
| Read-only diff fast path | Different file sizes do not require hashing just to refresh index stat data that the daemon never writes. |
| Index read indicator | `git_index_read_ex` tells the daemon when cached entry pointers and staged results must be invalidated. File stamps also detect changes when checksums are zero (`index.skipHash`). |
| Immutable index snapshots | Borrow sorted entries rather than copying the entire index per shard. The request thread sorts before workers run; differing comparison orders still receive private copies. |
| Direct index access | `git_index_get_byindex_no_sort` avoids mutating sort state from parallel readers. |
| Index range seek | Binary-search each shard's start rather than scanning all preceding index entries. |
| Index validation switches | Preserve the old daemon's checksum/path-validation opt-outs. Options are configured before workers use the library. They are unsafe for a library that writes repositories. |
| Zero nanoseconds | Preserve the truncated-index-timestamp workaround on both sides of the daemon/library boundary. |
| Discovery | Expose Git directory and workdir together without allocating a repository on every cached query; use upstream's current discovery machinery. |
| Push remote resolution | Preserve `branch.*.pushRemote`, `remote.pushDefault`, local `.` remotes and refspec mapping. Upstream has no equivalent combined API. |
| Refdb lookup | Export the existing internal lookup for TagDB. |
| Packed tag optimization | TagDB parses packed tags itself. Stop libgit2's redundant scan only when the packed-refs header explicitly declares sorted order. |
| Ignore semantics | Do not discard negations that override a parent ignore file. Initialize default rules under the cache lock. |
| Directory scanning | Preserve lazy ignore loading, basename-only sorting, deeper directory traversal, `d_type` directory detection and POSIX `fstatat` access. |
| Remaining synchronization | Atomic packed-object last-hit and attribute-cache pointer access; do not mutate shared attribute file stamps during concurrent reads. Use upstream atomic primitives. |
| Static linking | Preserve disabled DNS/password-database lookups, avoiding glibc NSS runtime dependencies. The daemon never connects to remotes. |

## Removed / replaced patches

- Custom index capability getters are replaced by `git_index_caps`; Unicode
  precomposition is read from the upstream configuration API.
- The custom tracking-remote API is replaced by `git_branch_upstream`,
  `git_branch_upstream_remote`, and `git_remote_lookup`.
- Custom config refresh/backend hooks and relaxed live-config string access are
  replaced by upstream's auto-refreshing configuration snapshots. This keeps
  current configuration parsing, entry ownership, and synchronization intact.
- The old `noop` / `preciousobjects` allowlist patch is unnecessary: upstream
  recognizes both extensions.
- Executable mode detection already uses Git's owner-execute bit (`0100`).
- Multiline config parsing already passes the quote state correctly to
  `strip_comments`.
- Packfile index/open publication and pack-cache refcount patches are replaced
  by upstream's current packfile locks and atomic refcount access. The separate
  last-hit pointer race still requires the retained patch above.
- The old shallow-revwalk missing-object workaround is replaced by upstream's
  shallow/graft handling; shallow ahead/behind is covered by the daemon tests.
- Historical HTTP, stash, index-v4 writing, merge-metric and other upstream bug
  backports are not reapplied to the current release. HTTP and writes are not
  daemon operations.
- Old OID parsing, sorted-cache/string-map and integer/mode parser micro-patches
  are not transplanted into the rewritten upstream hash/OID/map implementations.
- Old repository traversal/canonicalization rewrites are replaced by the current
  upstream discovery code, rather than overwriting newer ownership and format
  checks. Worktrees, submodules, symlinks and direct `.git` paths are tested.
- Removed upstream tests, fuzzers and CI files from the old fork are not removed
  from the release archive.

## Verification scope

Verified locally on Arch Linux x86-64, with a static-PIE executable, CMake 4.4.5
and GCC 16.2.1. The build's own smoke checks and the protocol regression suite
pass. Standalone Bash and Zsh bindings also passed start/query/stop checks with
`GITSTATUS_DAEMON` pointing to this checkout's binary, without sourcing user
startup files (Zsh requires a terminal for its job-control setup).

Other platforms and the full upstream libgit2 test suite have not been verified.
Some upstream tests permit index mutation during iteration, which intentionally
does not match this daemon's immutable-snapshot contract.
