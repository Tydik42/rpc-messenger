import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time
import unittest


BINARY = str(Path(sys.argv.pop(1)).resolve())
TIMEOUT = 4


class Terminal:
    def __init__(self, env, *args):
        self.process = subprocess.Popen(
            [BINARY, *args],
            env=env,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        self.pending = b""

    def send(self, line):
        self.process.stdin.write(f"{line}\n".encode())
        self.process.stdin.flush()

    def line(self):
        fd = self.process.stdout.fileno()
        deadline = time.monotonic() + TIMEOUT
        while b"\n" not in self.pending:
            remain = max(0, deadline - time.monotonic())
            if not select.select([fd], [], [], remain)[0]:
                raise AssertionError(f"Не получена строка из {self.process.args}: {self.pending!r}")
            part = os.read(fd, 4096)
            if not part:
                raise AssertionError(f"Процесс завершился: {self.process.args} {self.pending!r}")
            self.pending += part
        line, self.pending = self.pending.split(b"\n", 1)
        return line.decode()

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=TIMEOUT)
        self.process.stdin.close()
        self.process.stdout.close()


class ChatTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.env = dict(os.environ, XDG_RUNTIME_DIR=directory.name, TMPDIR=directory.name)

    def start(self, *args):
        terminal = Terminal(self.env, *args)
        self.addCleanup(terminal.close)
        return terminal

    def host(self, *flags):
        terminal = self.start("listen", *flags)
        self.assertIn("Комната открыта", terminal.line())
        return terminal

    def join(self, host, *flags):
        client = self.start("connect", *flags)
        self.assertIn("Подключение установлено", client.line())
        self.assertIn("подключился", host.line())
        return client

    def failed(self, *args):
        result = subprocess.run(
            [BINARY, *args], env=self.env, input=b"", capture_output=True, timeout=TIMEOUT
        )
        self.assertEqual(result.returncode, 1, result)
        return result.stderr.decode()

    def test_three_participants_exchange_messages(self):
        host = self.host("--nick", "Анна")
        first = self.join(host, "--nick=Илья")
        second = self.join(host, "--nick=Оля")

        first.send("Привет")
        second.send("Здравствуй 🙂")
        self.assertEqual({host.line(), host.line()}, {"[Илья] Привет", "[Оля] Здравствуй 🙂"})
        self.assertEqual({first.line(), first.line()}, {"[я] Привет", "[Оля] Здравствуй 🙂"})
        self.assertEqual({second.line(), second.line()}, {"[Илья] Привет", "[я] Здравствуй 🙂"})

        host.send("Добрый день")
        self.assertEqual(host.line(), "[я] Добрый день")
        self.assertEqual(first.line(), "[Анна] Добрый день")
        self.assertEqual(second.line(), "[Анна] Добрый день")

    def test_client_eof_and_host_quit(self):
        host = self.host()
        first = self.join(host, "--nick=Оля")
        second = self.join(host)

        first.process.stdin.close()
        self.assertEqual(first.process.wait(timeout=TIMEOUT), 0)
        self.assertIn("Оля вышел", host.line())
        self.join(host, "--nick=Оля")
        second.send("Ещё здесь")
        self.assertIn("Ещё здесь", host.line())
        self.assertEqual(second.line(), "[я] Ещё здесь")

        host.send("Последнее сообщение")
        host.send("/quit")
        self.assertEqual(second.line(), "[ведущий] Последнее сообщение")
        self.assertIn("Комната закрыта", second.line())
        self.assertEqual(host.process.wait(timeout=TIMEOUT), 0)
        self.assertEqual(second.process.wait(timeout=TIMEOUT), 0)

    def test_color_and_rooms(self):
        main = self.host("--room=lab", "--color=never")
        self.assertIn("Комната не открыта", self.failed("connect"))
        guest = self.join(main, "--room", "lab", "--color=always", "--nick=Гость")

        guest.send("один")
        self.assertEqual(main.line(), "[Гость] один")
        self.assertEqual(guest.line(), "\x1b[32m[я]\x1b[0m один")
        main.send("два")
        self.assertEqual(main.line(), "[я] два")
        self.assertEqual(guest.line(), "\x1b[36m[ведущий]\x1b[0m два")

    def test_duplicate_host_and_nickname(self):
        host = self.host("--nick", "Анна")
        self.assertIn("Комната уже открыта", self.failed("listen"))
        self.assertIn("Этот ник уже занят", self.failed("connect", "--nick", "Анна"))
        self.join(host, "--nick", "Илья")

    def test_runtime_fallback_and_restart(self):
        self.env.pop("XDG_RUNTIME_DIR")
        path = Path(self.env["TMPDIR"]) / f"rpc-messenger-{os.geteuid()}-default"
        host = self.host()
        self.assertEqual(path.stat().st_mode & 0o777, 0o700)
        self.assertTrue((path / "join").is_fifo())
        host.process.kill()
        self.assertEqual(host.process.wait(timeout=TIMEOUT), -signal.SIGKILL)
        self.assertIn("Комната не открыта", self.failed("connect"))

        restarted = self.host()
        self.join(restarted)
        restarted.process.send_signal(signal.SIGINT)
        self.assertEqual(restarted.process.wait(timeout=TIMEOUT), 0)
        self.assertFalse((path / "join").exists())

    def test_invalid_options(self):
        self.assertIn("Имя комнаты", self.failed("listen", "--room=../lab"))
        self.assertIn("--color", self.failed("listen", "--color=random"))
        self.assertIn("Ник должен", self.failed("listen", "--nick=[Анна]"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
