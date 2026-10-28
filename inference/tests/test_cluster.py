import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("cluster", Path(__file__).parents[1] / "cluster.py")
cluster = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cluster)


class LauncherTests(unittest.TestCase):
    def test_invalid_workers(self):
        for value in ("pc:50052", "a:1,a:1", "a:0,b:1", "a:70000,b:1", "a:1,b"):
            with self.subTest(value=value), self.assertRaises((ValueError, cluster.argparse.ArgumentTypeError)):
                cluster.endpoints(value)

    def test_distributed_command_and_paths_with_spaces(self):
        with tempfile.TemporaryDirectory(prefix="model space ") as folder:
            model = Path(folder) / "model.gguf"
            model.touch()
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = cluster.main(["serve", "--bin-dir", folder, "--model", str(model),
                                     "--workers", "pc1:50052,pc2:50052", "--tensor-split", "1,2", "--dry-run"])
            cmd = cluster.json.loads(out.getvalue())
            self.assertEqual(code, 0)
            self.assertEqual(cmd[cmd.index("--model") + 1], str(model))
            self.assertEqual(cmd[cmd.index("--rpc") + 1], "pc1:50052,pc2:50052")
            self.assertEqual(cmd[cmd.index("--n-gpu-layers") + 1], "999")

    def test_worker_cpu_only(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(cluster.main(["worker", "--bin-dir", ".", "--dry-run"]), 0)
        cmd = cluster.json.loads(out.getvalue())
        self.assertEqual(cmd[cmd.index("--device") + 1], "CPU")
        self.assertEqual(cmd[cmd.index("--host") + 1], "127.0.0.1")

    def test_unreachable_worker_prevents_server_launch(self):
        with tempfile.TemporaryDirectory() as folder:
            model = Path(folder) / "model.gguf"
            model.touch()
            executable = Path(folder) / ("llama-server.exe" if cluster.os.name == "nt" else "llama-server")
            executable.touch()
            with patch.object(cluster.socket, "create_connection", side_effect=OSError("unreachable")), patch.object(cluster.subprocess, "call") as run, contextlib.redirect_stderr(io.StringIO()):
                code = cluster.main(["serve", "--bin-dir", folder, "--model", str(model), "--workers", "a:1,b:2"])
                self.assertEqual(code, 2)
                run.assert_not_called()

    def test_missing_model_fails_without_starting_process(self):
        with patch.object(cluster.subprocess, "call") as run, contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(cluster.main(["serve", "--bin-dir", ".", "--model", "/missing/model.gguf", "--workers", "a:1,b:2"]), 2)
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
