"""Regression coverage for native worker lifecycle and RPC endpoint handling."""
import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

INFERENCE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("native_launcher", INFERENCE / "cluster.py")
cluster = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cluster)


class EndpointRegressions(unittest.TestCase):
    def test_invalid_rpc_ipv6_and_url_fail_before_starting_runtime(self):
        for value in ("http://pc:50052,pc2:50052", "[::1]:50052,pc2:50052", "pc/x:50052,pc2:50052"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                cluster.endpoints(value)

    def test_duplicate_worker_case_and_port_notation_cannot_bypass_check(self):
        with self.assertRaisesRegex(ValueError, "takrorlanmasligi"):
            cluster.endpoints("PC:050052,pc:50052")
        self.assertEqual(cluster.endpoints("PC1:50052, PC2:50052"), ["pc1:50052", "pc2:50052"])

    def test_child_signal_exit_code_normalized(self):
        if os.name == "nt":
            self.skipTest("POSIX signal exit codes")
        code = cluster.run_child([sys.executable, "-c", "import os,signal;os.kill(os.getpid(),signal.SIGTERM)"])
        self.assertEqual(code, 143)

    @unittest.skipIf(os.name == "nt", "POSIX service-stop signal integration")
    def test_stopping_launcher_also_stops_and_reaps_runtime(self):
        for number in (signal.SIGTERM, signal.SIGINT):
            with self.subTest(number=number), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                pidfile = root / "pid"
                binary = root / "ggml-rpc-server"
                binary.write_text("#!/usr/bin/env python3\nimport os,time\nfrom pathlib import Path\n"
                                  f"Path({str(pidfile)!r}).write_text(str(os.getpid()))\ntime.sleep(60)\n")
                binary.chmod(0o755)
                launcher = subprocess.Popen([sys.executable, str(INFERENCE / "cluster.py"), "worker", "--bin-dir", folder],
                                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                child = None
                try:
                    deadline = time.monotonic() + 5
                    while not pidfile.exists() and time.monotonic() < deadline:
                        if launcher.poll() is not None:
                            self.fail("launcher exited before worker started")
                        time.sleep(0.05)
                    child = int(pidfile.read_text())
                    launcher.send_signal(number)
                    self.assertEqual(launcher.wait(timeout=8), 128 + number)
                    with self.assertRaises(ProcessLookupError):
                        os.kill(child, 0)
                finally:
                    if launcher.poll() is None:
                        launcher.kill()
                        launcher.wait()
                    if child:
                        try:
                            os.kill(child, signal.SIGKILL)
                        except ProcessLookupError:
                            pass


if __name__ == "__main__":
    unittest.main()
