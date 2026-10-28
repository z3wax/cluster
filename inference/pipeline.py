"""CPU tensor pipelines: fixed local model stages, transported between workers."""
import base64
import binascii
import hmac
import json
import logging
import math
import os
from pathlib import Path
import re
import threading
from http.client import HTTPException
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

import numpy as np

MAX_BODY = 64 * 1024 * 1024
ID = re.compile(r"^[A-Za-z0-9_-]{1,64}$")


class WorkerError(RuntimeError):
    """A configured worker failed or returned an invalid protocol response."""


def read_manifest(path):
    path = Path(path).expanduser().resolve()
    with path.open(encoding="utf-8") as source:
        config = json.load(source)
    if not isinstance(config, dict) or config.get("version") != 1:
        raise ValueError("manifest version=1 bo'lishi kerak")
    return config, path.parent


def names(value, label):
    if not isinstance(value, list) or not value or any(not isinstance(x, str) or not x for x in value):
        raise ValueError(f"{label}: bo'sh bo'lmagan tensor nomlari ro'yxati kerak")
    if len(set(value)) != len(value):
        raise ValueError(f"{label}: tensor nomi takrorlangan")
    return value


def encode_tensors(tensors, binary=True):
    result = {}
    size = 0
    for name, value in tensors.items():
        array = np.asarray(value)
        if array.dtype.kind not in "biufc" or array.dtype.hasobject:
            raise ValueError("faqat sonli va boolean tensorlar uzatiladi")
        if not binary and array.dtype.kind == "c":
            raise ValueError("complex tensor uchun base64 formatidan foydalaning")
        size += array.nbytes
        if size > MAX_BODY // 2:
            raise ValueError("tensorlar hajmi limitdan oshdi")
        entry = {"dtype": array.dtype.str, "shape": list(array.shape)}
        if binary:
            entry["base64"] = base64.b64encode(array.tobytes(order="C")).decode("ascii")
        else:
            entry["data"] = array.tolist()
        result[name] = entry
    return result


def validate_data(value, shape):
    """Check shape before NumPy can allocate a differently sized nested list."""
    if shape:
        if not isinstance(value, list) or len(value) != shape[0]:
            raise ValueError("tensor data shakli shape bilan mos emas")
        for item in value:
            validate_data(item, shape[1:])
    elif type(value) not in (bool, int, float):
        raise ValueError("tensor data sonli scalar bo'lishi kerak")


def decode_tensors(tensors):
    if not isinstance(tensors, dict) or not tensors:
        raise ValueError("inputs/outputs tensorlar obyekti bo'lishi kerak")
    result = {}
    size = 0
    for name, entry in tensors.items():
        if not isinstance(name, str) or not name or not isinstance(entry, dict):
            raise ValueError("tensor nomi yoki formati noto'g'ri")
        dtype = np.dtype(entry["dtype"])
        shape = entry["shape"]
        if dtype.kind not in "biufc" or dtype.hasobject or dtype.itemsize > 16:
            raise ValueError("xavfsiz sonli dtype kerak; object/pickle qo'llanmaydi")
        if not isinstance(shape, list) or len(shape) > 32 or any(type(x) is not int or x < 0 for x in shape):
            raise ValueError("tensor shape musbat yoki nol butun sonlardan iborat bo'lishi kerak")
        expected = math.prod(shape) * dtype.itemsize
        size += expected
        if size > MAX_BODY // 2:
            raise ValueError("tensorlar hajmi limitdan oshdi")
        if ("base64" in entry) == ("data" in entry):
            raise ValueError("tensor uchun faqat data yoki base64 kiriting")
        if "base64" in entry:
            raw = base64.b64decode(entry["base64"], validate=True)
            if len(raw) != expected:
                raise ValueError("tensor baytlari shape/dtype bilan mos emas")
            array = np.frombuffer(raw, dtype=dtype).copy().reshape(shape)
        else:
            validate_data(entry["data"], shape)
            array = np.asarray(entry["data"], dtype=dtype).reshape(shape)
        result[name] = array
    return result


def worker_url(value):
    if not isinstance(value, str):
        raise ValueError("worker HTTP URL bo'lishi kerak")
    parsed = urlsplit(value)
    if (parsed.scheme not in ("http", "https") or not parsed.hostname or parsed.username or parsed.password
            or parsed.path not in ("", "/") or parsed.query or parsed.fragment):
        raise ValueError(f"worker URL noto'g'ri: {value}")
    _ = parsed.port  # validate port before any request
    return value.rstrip("/")


def remote_json(url, token, payload=None, timeout=120):
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = "Bearer " + token
    body = None if payload is None else json.dumps(payload, allow_nan=False).encode()
    if body is not None and len(body) > MAX_BODY:
        raise ValueError("so'rov hajmi limitdan oshdi")
    request = Request(url, data=body, headers=headers)
    try:
        with urlopen(request, timeout=timeout) as response:
            declared = response.headers.get("Content-Length")
            try:
                declared = None if declared is None else int(declared)
            except ValueError as exc:
                raise WorkerError(f"worker Content-Length noto'g'ri: {url}") from exc
            if declared is not None and (declared < 0 or declared > MAX_BODY):
                raise WorkerError(f"worker javobi limitdan oshdi: {url}")
            data = response.read(MAX_BODY + 1)
            if declared is not None and len(data) != declared:
                raise WorkerError(f"worker javobi to'liq kelmadi: {url}")
        if len(data) > MAX_BODY:
            raise WorkerError(f"worker javobi limitdan oshdi: {url}")
        try:
            result = json.loads(data)
        except (ValueError, UnicodeError) as exc:
            raise WorkerError(f"worker yaroqli JSON qaytarmadi: {url}") from exc
        if not isinstance(result, dict):
            raise WorkerError(f"worker javobi JSON obyekt bo'lishi kerak: {url}")
        return result
    except HTTPError as exc:
        # Preserve useful model/runtime diagnostics without copying a whole proxy
        # error page or an unbounded response into the coordinator's error.
        detail = ""
        try:
            data = json.loads(exc.read(4096))
            if isinstance(data, dict) and isinstance(data.get("error"), str):
                detail = ": " + data["error"][:512]
        except (ValueError, OSError, HTTPException):
            pass
        finally:
            exc.close()
        raise WorkerError(f"worker HTTP xatosi {exc.code}: {url}{detail}") from exc
    except (URLError, OSError, HTTPException) as exc:
        reason = exc.reason if isinstance(exc, URLError) else exc
        raise WorkerError(f"worker bilan aloqa yo'q: {url}: {reason}") from exc


class Worker:
    def __init__(self, manifest, threads):
        try:
            from .adapters import load_stage
        except ImportError:
            from adapters import load_stage
        config, root = read_manifest(manifest)
        stages = config.get("stages")
        if not isinstance(stages, list) or not stages:
            raise ValueError("worker manifestida stages kerak")
        self.stages = {}
        self.locks = {}
        for stage in stages:
            if not isinstance(stage, dict):
                raise ValueError("stage JSON obyekt bo'lishi kerak")
            key = stage.get("id", "")
            if not ID.fullmatch(key) or key in self.stages:
                raise ValueError("stage id noto'g'ri yoki takrorlangan")
            self.stages[key] = load_stage(stage, root, threads=threads)
            self.locks[key] = threading.Lock()

    def health(self):
        return {"status": "ok", "stages": list(self.stages)}

    def run(self, key, inputs):
        if key not in self.stages:
            raise ValueError(f"stage topilmadi: {key}")
        with self.locks[key]:
            outputs = self.stages[key].run(inputs)
            if sum(array.nbytes for array in outputs.values()) > MAX_BODY // 2:
                raise ValueError("tensorlar hajmi limitdan oshdi")
            # A custom/native runtime can reuse its output buffers on the next
            # call. Give this response ownership before another request runs.
            return {name: np.array(array, copy=True) for name, array in outputs.items()}


class Pipeline:
    def __init__(self, manifest, token="", timeout=120):
        config, _ = read_manifest(manifest)
        self.max_sequence_length = config.get("max_sequence_length")
        if self.max_sequence_length is not None and (type(self.max_sequence_length) is not int or self.max_sequence_length < 1):
            raise ValueError("max_sequence_length musbat butun son bo'lishi kerak")
        self.eos_token_ids = config.get("eos_token_ids", [])
        if not isinstance(self.eos_token_ids, list) or any(type(x) is not int or x < 0 for x in self.eos_token_ids):
            raise ValueError("eos_token_ids musbat yoki nol butun sonlar ro'yxati bo'lishi kerak")
        self.inputs = names(config.get("inputs"), "pipeline inputs")
        self.outputs = names(config.get("outputs"), "pipeline outputs")
        self.stages = config.get("stages")
        if not isinstance(self.stages, list) or len(self.stages) < 2:
            raise ValueError("pipeline uchun kamida ikkita stage kerak")
        available = set(self.inputs)
        for stage in self.stages:
            if not isinstance(stage, dict):
                raise ValueError("pipeline stage JSON obyekt bo'lishi kerak")
            stage["worker"] = worker_url(stage["worker"])
            if not ID.fullmatch(stage.get("stage", "")):
                raise ValueError("pipeline stage id noto'g'ri")
            for field in ("inputs", "outputs"):
                mapping = stage.get(field)
                if not isinstance(mapping, dict) or not mapping or any(not isinstance(k, str) or not k or not isinstance(v, str) or not v for k, v in mapping.items()):
                    raise ValueError(f"stage {field} mapping kerak")
                if field == "outputs" and len(set(mapping.values())) != len(mapping):
                    raise ValueError("stage output'lari bir xil pipeline tensoriga yozilmasligi kerak")
            if not set(stage["inputs"].values()) <= available:
                raise ValueError("stage hali hisoblanmagan tensorni talab qilyapti")
            available.update(stage["outputs"].values())
        if len({stage["worker"] for stage in self.stages}) < 2:
            raise ValueError("modelni taqsimlash uchun kamida ikkita worker URL kerak")
        if not set(self.outputs) <= available:
            raise ValueError("pipeline output tensor topilmadi")
        self.token = token
        self.timeout = timeout
        # Release each activation after its final consumer. A pipeline whose
        # stages use different tensor names must not retain every layer's RAM.
        self.last_use = {}
        for index, stage in enumerate(self.stages):
            for name in stage["inputs"].values():
                self.last_use[name] = index

    def check_workers(self, timeout=None):
        health = {}
        for stage in self.stages:
            url = stage["worker"]
            if url not in health:
                health[url] = remote_json(url + "/health", self.token, timeout=self.timeout if timeout is None else timeout)
            status = health[url]
            advertised = status.get("stages")
            if status.get("status") != "ok" or not isinstance(advertised, list) or any(not isinstance(key, str) for key in advertised):
                raise WorkerError(f"worker health javobi noto'g'ri: {url}")
            if stage["stage"] not in advertised:
                raise ValueError(f"worker'da stage yo'q: {url}/{stage['stage']}")

    def health(self):
        self.check_workers(timeout=min(self.timeout, 5))
        return {"status": "ok", "stages": len(self.stages), "workers": len({s["worker"] for s in self.stages})}

    def run(self, inputs):
        if set(inputs) != set(self.inputs):
            raise ValueError(f"pipeline inputs kerak: {self.inputs}")
        if self.max_sequence_length is not None and "input_ids" in inputs:
            ids = inputs["input_ids"]
            if ids.ndim != 2 or ids.shape[1] < 1 or ids.shape[1] > self.max_sequence_length:
                raise ValueError("input_ids sequence model context limitiga mos emas")
        context = dict(inputs)
        for index, stage in enumerate(self.stages):
            values = {key: context[value] for key, value in stage["inputs"].items()}
            response = remote_json(stage["worker"] + "/v1/stages/" + stage["stage"] + "/run",
                                   self.token, {"inputs": encode_tensors(values)}, timeout=self.timeout)
            try:
                outputs = decode_tensors(response["outputs"])
            except (KeyError, TypeError, ValueError, OverflowError) as exc:
                raise WorkerError(f"worker tensor javobi noto'g'ri: {stage['stage']}: {exc}") from exc
            if set(outputs) != set(stage["outputs"]):
                raise WorkerError(f"worker output nomlari mos emas: {stage['stage']}")
            for key, value in stage["outputs"].items():
                context[value] = outputs[key]
            del values, outputs, response
            for name in list(context):
                if name not in self.outputs and self.last_use.get(name, -1) <= index:
                    del context[name]
        return {name: context[name] for name in self.outputs}


class TextGeneration:
    """Greedy full-context decoding; model KV caches are deliberately not assumed."""
    def __init__(self, pipeline, tokenizer_path, max_context=2048):
        from transformers import AutoTokenizer
        self.pipeline = pipeline
        self.tokenizer = AutoTokenizer.from_pretrained(tokenizer_path, local_files_only=True, trust_remote_code=False)
        self.max_context = min(max_context, pipeline.max_sequence_length or max_context)
        self.eos_token_ids = set(pipeline.eos_token_ids)
        if not self.eos_token_ids and self.tokenizer.eos_token_id is not None:
            self.eos_token_ids.add(self.tokenizer.eos_token_id)
        if pipeline.inputs != ["input_ids"] or pipeline.outputs != ["logits"]:
            raise ValueError("text API uchun pipeline input_ids -> logits bo'lishi kerak")

    def generate(self, body, chat=False):
        limit = body.get("max_tokens", 64)
        if type(limit) is not int or limit < 1 or limit > 256:
            raise ValueError("max_tokens 1..256 bo'lishi kerak")
        if body.get("stream"):
            raise ValueError("pipeline text API hozir streaming'ni qo'llamaydi")
        if body.get("temperature", 0) != 0:
            raise ValueError("pipeline text API hozir temperature=0 (greedy) ishlatadi")
        if chat:
            messages = body.get("messages")
            if not isinstance(messages, list) or not messages:
                raise ValueError("messages kerak")
            if not self.tokenizer.chat_template:
                raise ValueError("bu tokenizer'da chat_template yo'q; /v1/completions ishlating")
            ids = self.tokenizer.apply_chat_template(messages, tokenize=True, add_generation_prompt=True)
        else:
            prompt = body.get("prompt")
            if not isinstance(prompt, str) or not prompt:
                raise ValueError("prompt bo'sh bo'lmagan matn bo'lishi kerak")
            ids = self.tokenizer.encode(prompt)
        if not ids or len(ids) + limit > self.max_context:
            raise ValueError("prompt + max_tokens context limitidan oshdi")
        prompt_tokens = len(ids)
        generated = []
        reason = "length"
        for _ in range(limit):
            logits = self.pipeline.run({"input_ids": np.asarray([ids], dtype=np.int64)})["logits"]
            if logits.ndim != 3 or (logits.shape[0] != 1 or logits.shape[1] not in (1, len(ids))) or logits.shape[2] < 1:
                raise WorkerError("logits shakli [1, 1 yoki sequence, vocab] bo'lishi kerak")
            scores = logits[0, -1]
            if scores.dtype.kind not in "fiu" or np.isnan(scores).any() or not np.isfinite(scores).any():
                raise WorkerError("model yaroqli logits qaytarmadi (NaN yoki bo'sh taqsimot)")
            token = int(np.argmax(scores))
            generated.append(token)
            ids.append(token)
            if token in self.eos_token_ids:
                reason = "stop"
                break
        text = self.tokenizer.decode(generated, skip_special_tokens=True)
        choice = {"index": 0, "finish_reason": reason}
        choice["message" if chat else "text"] = {"role": "assistant", "content": text} if chat else text
        return {"object": "chat.completion" if chat else "text_completion", "model": "sdrive-pipeline",
                "choices": [choice], "usage": {"prompt_tokens": prompt_tokens, "completion_tokens": len(generated),
                                                "total_tokens": len(ids)}}


def make_server(host, port, runtime, token="", text=None):
    class Handler(BaseHTTPRequestHandler):
        def setup(self):
            super().setup()
            self.connection.settimeout(120)

        def log_message(self, fmt, *args):
            logging.info("%s %s", self.client_address[0], fmt % args)

        def send_json(self, code, body):
            data = json.dumps(body, allow_nan=False).encode()
            if len(data) > MAX_BODY:
                raise ValueError("javob hajmi limitdan oshdi")
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def authorized(self):
            if token and not hmac.compare_digest(self.headers.get("Authorization", "").encode("utf-8"), ("Bearer " + token).encode("utf-8")):
                self.send_json(401, {"error": "authorization kerak"})
                return False
            return True

        def do_GET(self):
            if not self.authorized():
                return
            if self.path != "/health":
                self.send_json(404, {"error": "topilmadi"})
                return
            try:
                self.send_json(200, runtime.health())
            except (WorkerError, ValueError) as exc:
                self.send_json(503, {"status": "error", "error": str(exc)})

        def do_POST(self):
            if not self.authorized():
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if length <= 0 or length > MAX_BODY:
                    self.send_json(413, {"error": "Content-Length 1..64MiB bo'lishi kerak"})
                    return
                body = json.loads(self.rfile.read(length))
                if not isinstance(body, dict):
                    raise ValueError("JSON obyekt kerak")
                if isinstance(runtime, Worker):
                    match = re.fullmatch(r"/v1/stages/([A-Za-z0-9_-]{1,64})/run", self.path)
                    if not match:
                        self.send_json(404, {"error": "topilmadi"})
                        return
                    result = runtime.run(match[1], decode_tensors(body["inputs"]))
                elif self.path == "/v1/infer":
                    result = runtime.run(decode_tensors(body["inputs"]))
                elif text is not None and self.path in ("/v1/completions", "/v1/chat/completions"):
                    self.send_json(200, text.generate(body, chat=self.path == "/v1/chat/completions"))
                    return
                else:
                    self.send_json(404, {"error": "topilmadi"})
                    return
                self.send_json(200, {"outputs": encode_tensors(result, binary=isinstance(runtime, Worker) or body.get("encoding") == "base64")})
            except WorkerError as exc:
                self.send_json(502, {"error": str(exc)})
            except (ValueError, TypeError, KeyError, OverflowError, RecursionError, binascii.Error) as exc:
                self.send_json(400, {"error": str(exc)})
            except Exception as exc:
                logging.exception("inference xatosi")
                self.send_json(502, {"error": str(exc)})
    return ThreadingHTTPServer((host, port), Handler)


def launch(args):
    token = os.environ.get("SDRIVE_INFERENCE_TOKEN", "")
    if args.mode == "worker":
        runtime = Worker(args.manifest, args.threads)
        text = None
    else:
        runtime = Pipeline(args.manifest, token=token, timeout=args.timeout)
        runtime.check_workers()
        text = TextGeneration(runtime, args.tokenizer, args.context) if args.tokenizer else None
    server = make_server(args.host, args.port, runtime, token=token, text=text)
    print(f"SDrive CPU {args.mode}: http://{args.host}:{args.port}", flush=True)
    try:
        server.serve_forever()
    finally:
        server.server_close()
    return 0
