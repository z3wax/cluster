"""Operational regressions for live workers, upstream failures and activation RAM."""
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch
from types import SimpleNamespace
from urllib.error import HTTPError
from urllib.request import Request, urlopen
import weakref

import numpy as np

spec = importlib.util.spec_from_file_location("pipeline_regressions_runtime", Path(__file__).parents[1] / "pipeline.py")
pipeline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pipeline)


def manifest(urls, chain=("hidden", "y")):
    stages = []
    previous = "x"
    for index, output in enumerate(chain):
        stages.append({"worker": urls[index % len(urls)], "stage": f"stage{index}",
                       "inputs": {previous: previous}, "outputs": {output: output}})
        previous = output
    return {"version": 1, "inputs": ["x"], "outputs": [chain[-1]], "stages": stages}


@contextmanager
def configured_pipeline(config):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "pipeline.json"
        path.write_text(json.dumps(config), encoding="utf-8")
        yield pipeline.Pipeline(path, timeout=1)


@contextmanager
def serving(server):
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


def request(url, body=None):
    data = None if body is None else json.dumps(body).encode()
    with urlopen(Request(url, data=data, headers={"Content-Type": "application/json"}), timeout=3) as response:
        return json.load(response)


def stub_worker(body=b"{}", status=200, partial=False):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            data = json.dumps({"status": "ok", "stages": ["stage0", "stage1"]}).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_POST(self):
            self.rfile.read(int(self.headers.get("Content-Length", "0")))
            self.send_response(status)
            self.send_header("Content-Length", str(len(body) + (10 if partial else 0)))
            self.end_headers()
            self.wfile.write(body)
    return ThreadingHTTPServer(("127.0.0.1", 0), Handler)


class LiveWorkerTests(unittest.TestCase):
    def test_health_returns_503_when_a_configured_worker_disappears(self):
        worker = stub_worker()
        with serving(worker) as first, serving(stub_worker()) as second:
            with configured_pipeline(manifest([first, second])) as runtime:
                with serving(pipeline.make_server("127.0.0.1", 0, runtime)) as base:
                    self.assertEqual(request(base + "/health")["status"], "ok")
                    worker.shutdown()
                    worker.server_close()
                    with self.assertRaises(HTTPError) as raised:
                        request(base + "/health")
                    self.assertEqual(raised.exception.code, 503)
                    self.assertEqual(json.load(raised.exception)["status"], "error")

    def test_bad_worker_json_is_502_rather_than_bad_client_input(self):
        for body in (b"<html>upstream proxy error</html>", b"[]", b'{"outputs": []}'):
            with self.subTest(body=body), serving(stub_worker(body)) as first, serving(stub_worker()) as second:
                with configured_pipeline(manifest([first, second])) as runtime:
                    with serving(pipeline.make_server("127.0.0.1", 0, runtime)) as base:
                        with self.assertRaises(HTTPError) as raised:
                            request(base + "/v1/infer", {"inputs": pipeline.encode_tensors({"x": np.ones((1,), dtype=np.float32)})})
                        self.assertEqual(raised.exception.code, 502)
                        self.assertIn("worker", json.load(raised.exception)["error"])

    def test_worker_http_error_keeps_the_model_diagnostic(self):
        with serving(stub_worker(b'{"error":"input x must have rank 2"}', status=400)) as url:
            with self.assertRaisesRegex(pipeline.WorkerError, "input x must have rank 2"):
                pipeline.remote_json(url + "/v1/stages/stage0/run", "", {"inputs": {}})

    def test_truncated_worker_response_is_an_upstream_error(self):
        with serving(stub_worker(b"{}", partial=True)) as url:
            with self.assertRaises(pipeline.WorkerError):
                pipeline.remote_json(url + "/v1/stages/stage0/run", "", {"inputs": {}})

    def test_health_rejects_bad_status_and_stage_metadata(self):
        with configured_pipeline(manifest(["http://worker1:1", "http://worker2:2"])) as runtime:
            for status in ({"status": "error", "stages": ["stage0", "stage1"]},
                           {"status": "ok", "stages": "stage0 stage1"}):
                with self.subTest(status=status), patch.object(pipeline, "remote_json", return_value=status):
                    with self.assertRaises(pipeline.WorkerError):
                        runtime.check_workers()


class ActivationLifetimeTests(unittest.TestCase):
    def test_intermediate_tensor_released_after_its_last_consumer(self):
        config = manifest(["http://worker1:1", "http://worker2:2"], chain=("a", "b", "y"))
        tracked = []
        original_decode = pipeline.decode_tensors
        def decode(value):
            tensors = original_decode(value)
            tracked.append(weakref.ref(next(iter(tensors.values()))))
            return tensors
        def remote(url, token, payload, timeout):
            index = int(url.rsplit("/stage", 1)[1].split("/")[0])
            if index == 2:
                self.assertIsNone(tracked[0](), "stage0 activation retained after stage1 consumed it")
            name = ("a", "b", "y")[index]
            return {"outputs": pipeline.encode_tensors({name: np.array([index], dtype=np.float32)})}
        with configured_pipeline(config) as runtime, patch.object(pipeline, "decode_tensors", side_effect=decode), patch.object(pipeline, "remote_json", side_effect=remote):
            result = runtime.run({"x": np.array([1], dtype=np.float32)})
        self.assertIsNone(tracked[0]())
        self.assertIsNone(tracked[1]())
        np.testing.assert_array_equal(result["y"], [2])

    def test_output_alias_cannot_silently_discard_one_model_result(self):
        config = manifest(["http://worker1:1", "http://worker2:2"])
        config["stages"][0]["outputs"] = {"first": "hidden", "second": "hidden"}
        with self.assertRaises(ValueError):
            with configured_pipeline(config):
                pass


class ResponseOwnershipTests(unittest.TestCase):
    def test_later_stage_call_cannot_overwrite_an_earlier_response(self):
        class ReusingStage:
            buffer = np.zeros((1,), dtype=np.float32)
            def run(self, inputs):
                self.buffer[:] = inputs["x"]
                return {"y": self.buffer}
        worker = pipeline.Worker.__new__(pipeline.Worker)
        worker.stages = {"stage0": ReusingStage()}
        worker.locks = {"stage0": threading.Lock()}
        first = worker.run("stage0", {"x": np.array([1], dtype=np.float32)})
        second = worker.run("stage0", {"x": np.array([2], dtype=np.float32)})
        np.testing.assert_array_equal(first["y"], [1])
        np.testing.assert_array_equal(second["y"], [2])

    def test_invalid_logits_cannot_silently_select_a_token(self):
        for scores in ([0, np.nan, 10], [-np.inf, -np.inf, -np.inf], []):
            generator = pipeline.TextGeneration.__new__(pipeline.TextGeneration)
            logits = np.asarray([[scores]], dtype=np.float32)
            generator.pipeline = SimpleNamespace(run=lambda values: {"logits": logits})
            generator.tokenizer = SimpleNamespace(encode=lambda prompt: [1], decode=lambda ids, **kwargs: "wrong")
            generator.max_context = 8
            generator.eos_token_ids = {1}
            with self.subTest(scores=scores), self.assertRaises(pipeline.WorkerError):
                generator.generate({"prompt": "hello", "max_tokens": 1})


if __name__ == "__main__":
    unittest.main()
