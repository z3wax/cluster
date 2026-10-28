#!/usr/bin/env python3
"""Real two-worker inference on loopback. Requires built binaries and a GGUF model."""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_ready(process, check, timeout=120):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"child exited: {process.returncode}")
        try:
            check()
            return
        except OSError:
            time.sleep(0.2)
    raise TimeoutError("startup timed out")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", required=True)
    parser.add_argument("--model", required=True)
    args = parser.parse_args()
    launcher = str(Path(__file__).with_name("cluster.py"))
    processes = []
    with tempfile.TemporaryDirectory() as directory:
        logs = []
        try:
            ports = []
            for index in range(2):
                worker_port = free_port()
                ports.append(worker_port)
                log = open(Path(directory) / f"worker{index}.log", "w+")
                logs.append(log)
                process = subprocess.Popen([sys.executable, launcher, "worker", "--bin-dir", args.bin_dir,
                                            "--port", str(worker_port), "--threads", "2"], stdout=log, stderr=log, start_new_session=sys.platform != "win32")
                processes.append(process)
                def check(port=worker_port):
                    socket.create_connection(("127.0.0.1", port), timeout=1).close()
                wait_ready(process, check)
            api_port = free_port()
            log = open(Path(directory) / "server.log", "w+")
            logs.append(log)
            process = subprocess.Popen([sys.executable, launcher, "serve", "--bin-dir", args.bin_dir,
                                        "--model", args.model, "--workers", ",".join(f"127.0.0.1:{p}" for p in ports),
                                        "--tensor-split", "1,1", "--context", "256", "--threads", "2", "--port", str(api_port)],
                                       stdout=log, stderr=log, start_new_session=sys.platform != "win32")
            processes.append(process)
            base = f"http://127.0.0.1:{api_port}"
            def health():
                with urllib.request.urlopen(base + "/health", timeout=2) as response:
                    response.read()
            wait_ready(process, health)
            request = urllib.request.Request(base + "/completion", data=json.dumps({
                "prompt": "Once upon a time", "n_predict": 16, "temperature": 0, "seed": 1
            }).encode(), headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(request, timeout=120) as response:
                result = json.load(response)
            if not result.get("content"):
                raise AssertionError("empty completion")
            log.flush()
            log.seek(0)
            server_log = log.read()
            for worker_port in ports:
                if not any(f"127.0.0.1:{worker_port}" in line and "model buffer size" in line for line in server_log.splitlines()):
                    raise AssertionError(f"no model buffer on worker {worker_port}")
            print(json.dumps({"workers": 2, "completion": result["content"], "status": "passed"}))
        except Exception:
            for log in logs:
                log.flush()
                log.seek(0)
                print(log.read(), file=sys.stderr)
            raise
        finally:
            # Launcher waits for its child. Stop children directly as well, cross-platform.
            for process in reversed(processes):
                if process.poll() is None:
                    if sys.platform == "win32":
                        subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], capture_output=True)
                    else:
                        # All test subprocesses get their own process group below.
                        import os
                        import signal
                        os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=10)
            for log in logs:
                log.close()


if __name__ == "__main__":
    main()
