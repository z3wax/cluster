#!/usr/bin/env python3
"""SDrive CPU inference: GGUF RPC and native multi-format tensor pipelines."""
import argparse
import json
import os
import re
import signal
from pathlib import Path
import socket
import subprocess
import sys
import threading


def positive(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("musbat son kerak")
    return number


def port(value):
    number = positive(value)
    if number > 65535:
        raise argparse.ArgumentTypeError("port 1..65535 bo'lishi kerak")
    return number


def rpc_host(value):
    # The pinned ggml RPC parser splits on the first colon, so IPv6/URLs do not work.
    if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]*", value):
        raise ValueError("GGUF RPC uchun IPv4 yoki hostname kerak; URL/IPv6 qo'llanmaydi")
    return value.lower()


def endpoints(value):
    result = []
    for endpoint in value.split(","):
        host, separator, service = endpoint.strip().rpartition(":")
        if not separator:
            raise ValueError("worker manzili HOST:PORT formatida bo'lishi kerak")
        result.append(f"{rpc_host(host)}:{port(service)}")
    if len(result) < 2:
        raise ValueError("kamida ikkita CPU worker manzilini kiriting")
    if len(set(result)) != len(result):
        raise ValueError("worker manzillari takrorlanmasligi kerak")
    return result


class _ChildSignal(BaseException):
    def __init__(self, number):
        self.number = number


def run_child(cmd):
    """Stop and reap the native runtime when the launcher receives a stop signal."""
    options = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == "nt" else {"start_new_session": True}
    child = subprocess.Popen(cmd, **options)
    previous = {}

    def received(number, frame):
        raise _ChildSignal(number)

    def stop(number):
        if child.poll() is not None:
            return
        try:
            if os.name == "nt":
                child.send_signal(signal.CTRL_BREAK_EVENT if number == signal.SIGINT else number)
            else:
                os.killpg(child.pid, number)
        except ProcessLookupError:
            pass
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            if os.name == "nt":
                child.kill()
            else:
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            child.wait()

    try:
        if threading.current_thread() is threading.main_thread():
            for number in (signal.SIGINT, signal.SIGTERM):
                previous[number] = signal.signal(number, received)
        try:
            code = child.wait()
            return code if code >= 0 else 128 - code
        except _ChildSignal as exc:
            stop(exc.number)
            return 128 + exc.number
        except BaseException:
            stop(signal.SIGTERM)
            raise
    finally:
        for number, handler in previous.items():
            signal.signal(number, handler)


def command(args):
    if not args.bin_dir:
        raise ValueError("GGUF backend uchun --bin-dir kerak")
    if args.mode == "serve" and (not args.model or not args.workers):
        raise ValueError("GGUF serve uchun --model va --workers kerak")
    binary = "ggml-rpc-server" if args.mode == "worker" else "llama-server"
    executable = Path(args.bin_dir).expanduser().resolve() / (binary + (".exe" if os.name == "nt" else ""))
    if args.mode == "worker":
        return [str(executable), "--host", rpc_host(args.host), "--port", str(args.port), "--device", "CPU", "--threads", str(args.threads)]
    workers = endpoints(args.workers)
    model = Path(args.model).expanduser().resolve()
    if not model.is_file():
        raise ValueError(f"GGUF model fayli topilmadi: {model}")
    result = [str(executable), "--model", str(model), "--rpc", ",".join(workers),
              "--n-gpu-layers", "999", "--split-mode", "layer", "--host", args.host,
              "--port", str(args.port), "--ctx-size", str(args.context),
              "--threads", str(args.threads), "--parallel", "1", "--verbosity", "4"]
    if args.tensor_split:
        weights = args.tensor_split.split(",")
        if len(weights) != len(workers) or any(not w.isdigit() or int(w) <= 0 for w in weights):
            raise ValueError("tensor-split har bir worker uchun bitta musbat butun son talab qiladi")
        result += ["--tensor-split", args.tensor_split]
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    for name in ("worker", "serve"):
        sub = modes.add_parser(name)
        sub.add_argument("--backend", choices=("gguf", "pipeline"), default="gguf")
        sub.add_argument("--bin-dir", help="GGUF: llama.cpp build/bin katalogi")
        sub.add_argument("--manifest", help="pipeline: worker/pipeline JSON manifesti")
        sub.add_argument("--host", default="127.0.0.1")
        sub.add_argument("--port", type=port, default=50052 if name == "worker" else 8081)
        sub.add_argument("--threads", type=positive, default=max(1, (os.cpu_count() or 2) // 2))
        sub.add_argument("--dry-run", action="store_true", help="buyruqni JSON ko'rinishida chiqarish")
        if name == "serve":
            sub.add_argument("--model")
            sub.add_argument("--workers", help="HOST:PORT,HOST:PORT")
            sub.add_argument("--timeout", type=positive, default=120, help="pipeline worker so'rovi timeout (soniya)")
            sub.add_argument("--tokenizer", help="pipeline text API uchun lokal Hugging Face tokenizer katalogi")
            sub.add_argument("--context", type=positive, default=2048)
            sub.add_argument("--tensor-split", help="worker RAM nisbatlari, masalan 1,2")
    args = parser.parse_args(argv)
    try:
        if args.backend == "pipeline":
            if not args.manifest or not Path(args.manifest).expanduser().is_file():
                raise ValueError("pipeline uchun mavjud --manifest JSON fayli kerak")
            if args.bin_dir or getattr(args, "model", None) or getattr(args, "workers", None) or getattr(args, "tensor_split", None):
                raise ValueError("pipeline model va worker'lari manifest orqali belgilanadi")
            if args.dry_run:
                print(json.dumps({"backend": "pipeline", "mode": args.mode,
                                  "manifest": str(Path(args.manifest).expanduser().resolve())}))
                return 0
            try:
                from .pipeline import launch
            except ImportError:
                from pipeline import launch
            return launch(args)
        if args.manifest or getattr(args, "tokenizer", None):
            raise ValueError("--manifest va --tokenizer uchun --backend pipeline tanlang")
        cmd = command(args)
        if args.dry_run:
            print(json.dumps(cmd))
            return 0
        if not Path(cmd[0]).is_file():
            raise ValueError(f"binary topilmadi: {cmd[0]}; avval llama.cpp ni yig'ing")
        if args.mode == "serve":
            for endpoint in endpoints(args.workers):
                host, _, service = endpoint.rpartition(":")
                try:
                    with socket.create_connection((host.strip("[]"), int(service)), timeout=5):
                        pass
                except OSError as exc:
                    raise ValueError(f"worker bilan aloqa yo'q: {endpoint}: {exc}") from exc
        return run_child(cmd)
    except (ValueError, TypeError, KeyError, RuntimeError, ImportError, argparse.ArgumentTypeError, OSError) as exc:
        print(f"Xato: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
