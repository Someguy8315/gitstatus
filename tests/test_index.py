"""Exercise a persistent native Windows daemon as a repository changes between queries."""

import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import unittest


BINARY = Path(__file__).resolve().parents[1] / "obj" / "gitstatusd.exe"


def git(cwd, *args, input=None):
    return subprocess.check_output(
        ["git", "-c", "user.name=Test", "-c", "user.email=test@example.com",
         "-c", "protocol.file.allow=always", *args],
        cwd=cwd,
        input=input,
        stderr=subprocess.STDOUT,
    ).decode()


def init(root, *config):
    git(root, "init", "-b", "main", str(root))
    for key, value in (("core.fsmonitor", "false"), ("core.untrackedCache", "false"), *config):
        git(root, "config", key, value)


def git_counts(root):
    """Staged, unstaged, conflicted and untracked counts as reported by git status."""
    out = git(root, "--no-optional-locks", "-c", "status.renames=false", "status",
              "--porcelain=v1", "-z", "--untracked-files=normal", "--ignore-submodules=dirty")
    counts = [0, 0, 0, 0]
    for record in out.split("\0"):
        xy = record[:2]
        if len(xy) < 2:
            continue
        if xy == "??":
            counts[3] += 1
        elif xy in ("DD", "AU", "UD", "UA", "DU", "AA", "UU"):
            counts[2] += 1
        else:
            counts[0] += xy[0] != " "
            counts[1] += xy[1] != " "
    return tuple(counts)


def rmtree(path):
    def writable(func, p, _):
        Path(p).chmod(stat.S_IWRITE)
        func(p)
    shutil.rmtree(path, onerror=writable)


class Daemon:
    def __init__(self, test, flags=("-U", "-W", "-D")):
        self.test = test
        self.process = subprocess.Popen(
            [str(BINARY), "-s", "-1", "-u", "-1", "-c", "-1", "-d", "-1",
             "-t", "8", "-v", "ERROR", *flags],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
        )

    def fields(self, root):
        self.process.stdin.write(f"1\x1f{Path(root).as_posix()}\x1e".encode())
        self.process.stdin.flush()
        response = bytearray()
        while not response.endswith(b"\x1e"):
            byte = self.process.stdout.read(1)
            self.test.assertTrue(byte, "daemon closed its output")
            response.extend(byte)
        fields = response.decode().rstrip("\x1e").split("\x1f")
        self.test.assertEqual(len(fields), 29)
        self.test.assertEqual(fields[1], "1")
        return fields

    def counts(self, root):
        return tuple(map(int, self.fields(root)[10:14]))

    def check(self, root, expected):
        """Asserts the daemon agrees with git status and the expected counts."""
        self.test.assertEqual(git_counts(root), expected)
        self.test.assertEqual(self.counts(root), expected)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise
        finally:
            self.process.stdout.close()
        self.test.assertEqual(self.process.returncode, 0)


class IndexRefreshTest(unittest.TestCase):
    def test_rebase(self):
        for skip_hash in (False, True):
            for reset_author_date in (False, True):
                with self.subTest(skip_hash=skip_hash, reset_author_date=reset_author_date):
                    with tempfile.TemporaryDirectory(prefix="gitstatusd-") as temporary:
                        root = Path(temporary)
                        init(root, ("feature.manyFiles", "true"),
                             ("index.skipHash", str(skip_hash).lower()))
                        directory = root / "nested" / "files"
                        directory.mkdir(parents=True)
                        for i in range(14):
                            (directory / f"old_{i}.txt").write_text(f"file {i}\n")
                        (root / "feature.txt").write_text("base\n")
                        git(root, "add", ".")
                        git(root, "commit", "-m", "base")
                        git(root, "switch", "-c", "feature")
                        (root / "feature.txt").write_text("feature\n")
                        git(root, "commit", "-am", "feature")
                        git(root, "switch", "main")
                        for i in range(14):
                            git(root, "mv", f"nested/files/old_{i}.txt", f"nested/files/new_{i}.txt")
                        git(root, "commit", "-m", "rename")
                        git(root, "switch", "feature")
                        checksum = (root / ".git" / "index").read_bytes()[-20:]
                        self.assertEqual(checksum == bytes(20), skip_hash)

                        with Daemon(self) as daemon:
                            daemon.check(root, (0, 0, 0, 0))
                            args = ["rebase", "main"]
                            if reset_author_date:
                                args.append("--reset-author-date")
                            git(root, *args)
                            self.assertEqual(git(root, "status", "--porcelain"), "")
                            daemon.check(root, (0, 0, 0, 0))
                            daemon.check(root, (0, 0, 0, 0))

                            # The refreshed index must still detect real changes, not just go clean.
                            (directory / "new_0.txt").write_text("staged\n")
                            git(root, "add", ".")
                            (directory / "new_1.txt").write_text("unstaged\n")
                            (directory / "untracked.txt").write_text("untracked\n")
                            daemon.check(root, (1, 1, 0, 1))


class ChangesTest(unittest.TestCase):
    def test_touched_files(self):
        with tempfile.TemporaryDirectory(prefix="gitstatusd-") as temporary:
            root = Path(temporary)
            init(root)
            files = [root / f"{i}.txt" for i in range(3)]
            for f in files:
                f.write_text("same\n")
            git(root, "add", ".")
            git(root, "commit", "-m", "base")
            # Content matches the index but the stat doesn't, as after a checkout elsewhere.
            for f in files:
                old = f.stat().st_mtime - 3600
                os.utime(f, (old, old))

            with Daemon(self) as daemon:
                daemon.check(root, (0, 0, 0, 0))
                daemon.check(root, (0, 0, 0, 0))
                files[0].write_text("diff\n")
                daemon.check(root, (0, 1, 0, 0))
                # A new index entry for an unchanged file.
                blob = git(root, "hash-object", "-w", "--stdin", input=b"staged\n").strip()
                git(root, "update-index", "--cacheinfo", f"100644,{blob},1.txt")
                daemon.check(root, (1, 2, 0, 0))

    def test_many_changes_in_one_directory(self):
        with tempfile.TemporaryDirectory(prefix="gitstatusd-") as temporary:
            root = Path(temporary)
            init(root)
            directory = root / "dir"
            directory.mkdir()
            for i in range(30):
                (directory / f"{i:02}.txt").write_text(f"{i}\n")
            git(root, "add", ".")
            git(root, "commit", "-m", "base")

            with Daemon(self) as daemon:
                daemon.check(root, (0, 0, 0, 0))
                for i in range(0, 30, 3):
                    (directory / f"{i:02}.txt").write_text("changed\n")
                for i in range(1, 30, 6):
                    (directory / f"{i:02}.txt").unlink()
                for i in range(7):
                    (directory / f"new_{i}.txt").write_text("new\n")
                daemon.check(root, (0, 15, 0, 7))


class ConfigTest(unittest.TestCase):
    def test_symlinked_config_changes(self):
        with tempfile.TemporaryDirectory(prefix="gitstatusd-") as temporary:
            root = Path(temporary) / "root"
            root.mkdir()
            init(root)
            (root / "untracked.txt").write_text("untracked\n")
            config = Path(temporary) / "config"
            shutil.move(root / ".git" / "config", config)
            (root / ".git" / "config").symlink_to(config)

            with Daemon(self, flags=()) as daemon:
                self.assertEqual(daemon.counts(root), (0, 0, 0, 1))
                git(root, "config", "status.showUntrackedFiles", "no")
                self.assertTrue((root / ".git" / "config").is_symlink())
                self.assertEqual(daemon.counts(root), (0, 0, 0, 0))


class UpstreamTest(unittest.TestCase):
    def test_ahead_behind(self):
        with tempfile.TemporaryDirectory(prefix="gitstatusd-") as temporary:
            upstream = Path(temporary) / "upstream"
            root = Path(temporary) / "root"
            upstream.mkdir()
            init(upstream)
            (upstream / "file.txt").write_text("base\n")
            git(upstream, "add", ".")
            git(upstream, "commit", "-m", "base")
            git(Path(temporary), "clone", "-q", upstream.as_posix(), "root")
            git(root, "config", "remote.pushDefault", "origin")

            def commit(repo, message):
                (repo / f"{message}.txt").write_text(f"{message}\n")
                git(repo, "add", ".")
                git(repo, "commit", "-m", message)

            with Daemon(self) as daemon:
                def check(ahead, behind):
                    self.assertEqual(git(root, "rev-list", "--count", "@{u}..HEAD").strip(), str(ahead))
                    self.assertEqual(git(root, "rev-list", "--count", "HEAD..@{u}").strip(), str(behind))
                    fields = daemon.fields(root)
                    self.assertEqual((fields[14], fields[15]), (str(ahead), str(behind)))
                    self.assertEqual((fields[23], fields[24]), (str(ahead), str(behind)))

                check(0, 0)
                commit(root, "local")
                check(1, 0)
                commit(upstream, "remote")
                git(root, "fetch", "-q")
                check(1, 1)
                git(root, "reset", "-q", "--hard", "@{u}")
                check(0, 0)


class SubmoduleTest(unittest.TestCase):
    def test_head_changes(self):
        with tempfile.TemporaryDirectory(prefix="gitstatusd-") as temporary:
            upstream = Path(temporary) / "upstream"
            root = Path(temporary) / "root"
            upstream.mkdir()
            root.mkdir()
            init(upstream)
            for i in range(2):
                (upstream / "file.txt").write_text(f"{i}\n")
                git(upstream, "add", ".")
                git(upstream, "commit", "-m", str(i))
            init(root)
            git(root, "submodule", "add", upstream.as_posix(), "sub")
            git(root, "commit", "-m", "add sub")
            sub = root / "sub"

            with Daemon(self) as daemon:
                daemon.check(root, (0, 0, 0, 0))
                # Changes to the submodule's HEAD show without reopening it.
                git(sub, "checkout", "-q", "HEAD~1")
                daemon.check(root, (0, 1, 0, 0))
                git(sub, "checkout", "-q", "main")
                daemon.check(root, (0, 0, 0, 0))

                # Replacing .git with a file pointing to a different directory.
                git(root, "submodule", "absorbgitdirs")
                self.assertTrue((sub / ".git").is_file())
                daemon.check(root, (0, 0, 0, 0))
                git(sub, "checkout", "-q", "HEAD~1")
                daemon.check(root, (0, 1, 0, 0))

                # A fresh clone in place of the submodule.
                rmtree(sub)
                daemon.check(root, (0, 1, 0, 0))
                git(root, "clone", "-q", upstream.as_posix(), "sub")
                daemon.check(root, (0, 0, 0, 0))
                git(sub, "checkout", "-q", "HEAD~1")
                daemon.check(root, (0, 1, 0, 0))


if __name__ == "__main__":
    unittest.main()
