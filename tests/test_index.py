"""Exercise index refreshes through one persistent native Windows daemon."""

from pathlib import Path
import subprocess
import tempfile
import unittest


BINARY = Path(__file__).resolve().parents[1] / "obj" / "gitstatusd.exe"


class IndexRefreshTest(unittest.TestCase):
    def test_rebase(self):
        for skip_hash in (False, True):
            for reset_author_date in (False, True):
                with self.subTest(skip_hash=skip_hash, reset_author_date=reset_author_date):
                    with tempfile.TemporaryDirectory(prefix="gitstatusd-") as temporary:
                        root = Path(temporary)

                        def git(*args):
                            return subprocess.check_output(
                                ["git", "-c", "user.name=Test", "-c",
                                 "user.email=test@example.com", *args],
                                cwd=root,
                                stderr=subprocess.STDOUT,
                            ).decode()

                        git("init", "-b", "main")
                        git("config", "feature.manyFiles", "true")
                        git("config", "index.skipHash", str(skip_hash).lower())
                        git("config", "core.fsmonitor", "false")
                        git("config", "core.untrackedCache", "false")
                        directory = root / "nested" / "files"
                        directory.mkdir(parents=True)
                        for i in range(14):
                            (directory / f"old_{i}.txt").write_text(f"file {i}\n")
                        (root / "feature.txt").write_text("base\n")
                        git("add", ".")
                        git("commit", "-m", "base")
                        git("switch", "-c", "feature")
                        (root / "feature.txt").write_text("feature\n")
                        git("commit", "-am", "feature")
                        git("switch", "main")
                        for i in range(14):
                            git("mv", f"nested/files/old_{i}.txt", f"nested/files/new_{i}.txt")
                        git("commit", "-m", "rename")
                        git("switch", "feature")
                        checksum = (root / ".git" / "index").read_bytes()[-20:]
                        self.assertEqual(checksum == bytes(20), skip_hash)

                        daemon = subprocess.Popen(
                            [str(BINARY), "-s", "-1", "-u", "-1", "-c", "-1", "-d", "-1",
                             "-t", "8", "-v", "ERROR", "-U", "-W", "-D"],
                            stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE,
                        )

                        def counts():
                            daemon.stdin.write(f"1\x1f{root.as_posix()}\x1e".encode())
                            daemon.stdin.flush()
                            response = bytearray()
                            while not response.endswith(b"\x1e"):
                                byte = daemon.stdout.read(1)
                                self.assertTrue(byte, "daemon closed its output")
                                response.extend(byte)
                            fields = response.decode().rstrip("\x1e").split("\x1f")
                            self.assertEqual(len(fields), 29)
                            self.assertEqual(fields[1], "1")
                            return tuple(map(int, fields[10:14]))

                        try:
                            self.assertEqual(counts(), (0, 0, 0, 0))
                            args = ["rebase", "main"]
                            if reset_author_date:
                                args.append("--reset-author-date")
                            git(*args)
                            self.assertEqual(git("status", "--porcelain"), "")
                            self.assertEqual(counts(), (0, 0, 0, 0))
                            self.assertEqual(counts(), (0, 0, 0, 0))

                            # The refreshed index must still detect real changes, not just go clean.
                            (directory / "new_0.txt").write_text("staged\n")
                            git("add", ".")
                            (directory / "new_1.txt").write_text("unstaged\n")
                            (directory / "untracked.txt").write_text("untracked\n")
                            self.assertEqual(counts(), (1, 1, 0, 1))
                        finally:
                            daemon.stdin.close()
                            try:
                                daemon.wait(timeout=10)
                            except subprocess.TimeoutExpired:
                                daemon.kill()
                                daemon.wait()
                                raise
                            finally:
                                daemon.stdout.close()
                        self.assertEqual(daemon.returncode, 0)


if __name__ == "__main__":
    unittest.main()
