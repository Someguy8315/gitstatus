# Native Windows build (clang-cl, MSVC ABI)

`gitstatusd.exe` can be built as a native Windows program with clang-cl and the MSVC
toolchain (Windows SDK and static MSVC C runtime). It has no runtime dependencies beyond
system DLLs. `gitstatus.psm1` provides PowerShell bindings for it.

This is separate from the Cygwin/MSYS2 builds made by `./build`, which use the POSIX code
paths and the zsh/bash bindings.

## What differs from POSIX

The platform-specific code is in `src/platform.h` (POSIX: inline wrappers around the
original syscalls) and `src/platform_win.cc`:

- Directories are NT handles. Relative opens use `NtCreateFile` with a root directory handle,
  which is the Windows equivalent of `openat()`. Directory listing uses `NtQueryDirectoryFile`.
- File metadata comes from `GetFileInformationByHandle`. Mtimes are converted from `FILETIME`
  exactly as libgit2 and Git for Windows do, so they compare equal to index entries.
  Symbolic links and junctions are reported as symlinks, as in Git for Windows.
- Paths are UTF-8 internally, as in libgit2. The embedded manifest
  (`src/gitstatusd.manifest`) makes narrow Win32/CRT calls use UTF-8 and enables long paths.
- Requests are read by a background thread because `select()` doesn't work on pipes.
- `--parent-pid` uses a process handle. `--lock-fd` isn't supported.
- Request directories may use `\` or `/`. Response workdirs use `/` (libgit2's format).

libgit2 needs no changes: the gitstatus patches already have `GIT_WIN32` guards.

## Building on Windows

Prerequisites: Visual Studio 2022 (or Build Tools) with the "Desktop development with C++"
workload and the "C++ Clang Compiler for Windows" and "C++ CMake tools for Windows"
components, plus Git.

In an "x64 Native Tools Command Prompt for VS 2022":

```bat
git clone --depth 1 --branch 1.9.7-gitstatus https://github.com/Someguy8315/libgit2
set CFLAGS=/DGIT_ZERO_NSEC
cmake -S libgit2 -B libgit2\build -G Ninja ^
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_INSTALL_PREFIX=%CD%\libgit2-install ^
  -DBUILD_SHARED_LIBS=OFF -DSTATIC_CRT=ON -DUSE_THREADS=ON -DUSE_BUNDLED_ZLIB=ON ^
  -DREGEX_BACKEND=builtin -DUSE_HTTP_PARSER=builtin -DUSE_SSH=OFF -DUSE_HTTPS=OFF ^
  -DUSE_GSSAPI=OFF -DUSE_NTLMCLIENT=OFF -DBUILD_TESTS=OFF -DBUILD_CLI=OFF
cmake --build libgit2\build --target install

cmake -S gitstatus -B gitstatus\build -G Ninja -DCMAKE_CXX_COMPILER=clang-cl ^
  -DCMAKE_BUILD_TYPE=Release -DLIBGIT2_PREFIX=%CD%\libgit2-install
cmake --build gitstatus\build
copy gitstatus\build\gitstatusd.exe gitstatus\usrbin\
```

The libgit2 options match the ones `./build` uses on other platforms.

## Cross-compiling from Linux

This needs clang (for `clang-cl`), lld (`lld-link`), llvm (`llvm-lib`, `llvm-rc`, `llvm-mt`),
CMake and Ninja. The MSVC CRT and Windows SDK come from
[xwin](https://github.com/Jake-Shadle/xwin), which downloads them from Microsoft. Running it
with `--accept-license` accepts Microsoft's license terms.

```sh
cargo install --locked xwin
xwin --accept-license --arch x86_64 splat --output ~/.local/share/xwin
export XWIN_DIR=~/.local/share/xwin

git clone --depth 1 --branch 1.9.7-gitstatus https://github.com/Someguy8315/libgit2
CFLAGS=-DGIT_ZERO_NSEC cmake -S libgit2 -B libgit2/build-win -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/gitstatus/cmake/clang-cl-xwin.cmake \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PWD/libgit2-win \
  -DBUILD_SHARED_LIBS=OFF -DSTATIC_CRT=ON -DUSE_THREADS=ON -DUSE_BUNDLED_ZLIB=ON \
  -DREGEX_BACKEND=builtin -DUSE_HTTP_PARSER=builtin -DUSE_SSH=OFF -DUSE_HTTPS=OFF \
  -DUSE_GSSAPI=OFF -DUSE_NTLMCLIENT=OFF -DBUILD_TESTS=OFF -DBUILD_CLI=OFF
ninja -C libgit2/build-win install

cmake -S gitstatus -B gitstatus/build-win -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/gitstatus/cmake/clang-cl-xwin.cmake \
  -DCMAKE_BUILD_TYPE=Release -DLIBGIT2_PREFIX=$PWD/libgit2-win
ninja -C gitstatus/build-win
```

Pass extra C flags for libgit2 through `CFLAGS` rather than `-DCMAKE_C_FLAGS`. The latter
replaces the toolchain's include flags.

## Testing with Wine

`tests/wine-gitstatusd` runs the exe under Wine and translates paths between `/x` and `Z:/x`.
This lets the protocol suite run against the Windows binary unchanged:

```sh
export WINEPREFIX=/tmp/gitstatus-wine   # optional: keep ~/.wine untouched
GITSTATUS_WINE_EXE=$PWD/build-win/gitstatusd.exe GITSTATUS_TEST_DAEMON=tests/wine-gitstatusd make test
```

The same wrapper works as `-Daemon` for `gitstatus.psm1` under `pwsh` on Linux.

Wine is not Windows. NTFS-specific behavior (directory mtime updates, reparse points,
case-insensitive lookups, file sharing) still has to be checked on real Windows.

## PowerShell

```powershell
Import-Module C:\path\to\gitstatus\gitstatus.psm1
Start-GitStatus                # or: Start-GitStatus -Daemon C:\path\to\gitstatusd.exe
Get-GitStatus                  # status of the current directory, or $null outside a repo
Stop-GitStatus
```

`Get-GitStatus` returns an object with the same fields as the zsh bindings' `VCS_STATUS_*`
parameters (`LocalBranch`, `NumStaged`, `NumUnstaged`, `NumUntracked`, `CommitsAhead`, ...)
plus `HasDirty`. `Start-GitStatus` accepts the same limits as the daemon (`-MaxNumStaged`,
`-MaxNumUnstaged`, `-MaxNumConflicted`, `-MaxNumUntracked`, `-DirtyMaxIndexSize`); `-1` means
unlimited. Queries are synchronous with a timeout (`-TimeoutMilliseconds`, default 5000). A
late reply is skipped by the next query. Example prompt:

```powershell
function prompt {
  $s = Get-GitStatus
  $git = ''
  if ($s) {
    $git = " $($s.LocalBranch)"
    if ($s.NumStaged)    { $git += " +$($s.NumStaged)" }
    if ($s.NumUnstaged)  { $git += " !$($s.NumUnstaged)" }
    if ($s.NumUntracked) { $git += " ?$($s.NumUntracked)" }
    if ($s.CommitsAhead) { $git += " ⇡$($s.CommitsAhead)" }
    if ($s.CommitsBehind){ $git += " ⇣$($s.CommitsBehind)" }
  }
  "PS $($executionContext.SessionState.Path.CurrentLocation)$git> "
}
```
