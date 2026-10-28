#!/usr/bin/env python3
"""SDrive CPU inference: GGUF RPC and native multi-format tensor pipelines."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys


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


def endpoints(value):
    result = value.split(",")
    if len(result) < 2:
        raise ValueError("kamida ikkita CPU worker manzilini kiriting")
    for endpoint in result:
        host, separator, service = endpoint.rpartition(":")
        if not separator or not host or any(c.isspace() for c in host):
            raise ValueError("worker manzili HOST:PORT formatida bo'lishi kerak")
        port(service)
    if len(set(result)) != len(result):
        raise ValueError("worker manzillari takrorlanmasligi kerak")
    return result


def command(args):
    if not args.bin_dir:
        raise ValueError("GGUF backend uchun --bin-dir kerak")
    if args.mode == "serve" and (not args.model or not args.workers):
        raise ValueError("GGUF serve uchun --model va --workers kerak")
    binary = "ggml-rpc-server" if args.mode == "worker" else "llama-server"
    executable = Path(args.bin_dir).expanduser().resolve() / (binary + (".exe" if os.name == "nt" else ""))
    if args.mode == "worker":
        return [str(executable), "--host", args.host, "--port", str(args.port), "--device", "CPU", "--threads", str(args.threads)]
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
        return subprocess.call(cmd)
    except (ValueError, TypeError, KeyError, RuntimeError, ImportError, argparse.ArgumentTypeError, OSError) as exc:
        print(f"Xato: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
