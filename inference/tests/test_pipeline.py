"""Tensor transport, manifest validation, and real two-process ONNX pipeline tests."""
import base64
import binascii
import importlib.util
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

import numpy as np

INFERENCE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("sdrive_pipeline", INFERENCE / "pipeline.py")
pipeline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pipeline)


def manifest():
    return {
        "version": 1,
        "inputs": ["x"],
        "outputs": ["y"],
        "stages": [
            {"worker": "http://127.0.0.1:50052", "stage": "encoder",
             "inputs": {"x": "x"}, "outputs": {"hidden": "hidden"}},
            {"worker": "http://127.0.0.1:50053", "stage": "head",
             "inputs": {"hidden": "hidden"}, "outputs": {"y": "y"}},
        ],
    }


class TensorCodecTests(unittest.TestCase):
    def test_binary_roundtrip_preserves_values_shapes_and_dtypes(self):
        values = {
            "float": np.array([[1.25, -3.5]], dtype=np.float32),
            "integer": np.array([-1, 2**40], dtype=np.int64),
            "bool": np.array([True, False], dtype=np.bool_),
            "complex": np.array([1 + 2j], dtype=np.complex64),
            "scalar": np.array(4, dtype=np.int32),
            "empty": np.empty((2, 0), dtype=np.float64),
        }
        result = pipeline.decode_tensors(pipeline.encode_tensors(values))
        for key, expected in values.items():
            with self.subTest(tensor=key):
                np.testing.assert_array_equal(result[key], expected)
                self.assertEqual(result[key].dtype, expected.dtype)
                self.assertEqual(result[key].shape, expected.shape)

    def test_json_roundtrip_and_noncontiguous_input(self):
        values = {"x": np.arange(12, dtype=np.float32).reshape(3, 4)[:, ::2]}
        encoded = json.loads(json.dumps(pipeline.encode_tensors(values, binary=False)))
        np.testing.assert_array_equal(pipeline.decode_tensors(encoded)["x"], values["x"])
        np.testing.assert_array_equal(pipeline.decode_tensors(pipeline.encode_tensors(values))["x"], values["x"])

    def test_rejects_unsafe_or_unsupported_dtypes(self):
        for dtype in ("object", "U5", "S5", "V8"):
            with self.subTest(dtype=dtype), self.assertRaises(ValueError):
                pipeline.decode_tensors({"x": {"dtype": dtype, "shape": [0], "data": []}})
        with self.assertRaises(ValueError):
            pipeline.encode_tensors({"x": np.array([object()], dtype=object)})
        with self.assertRaises(ValueError):
            pipeline.encode_tensors({"x": np.array([1j])}, binary=False)

    def test_rejects_byte_and_shape_mismatches(self):
        invalid = [
            {"dtype": "float32", "shape": [2], "base64": base64.b64encode(b"1234").decode()},
            {"dtype": "float32", "shape": [2], "base64": "not base64!"},
            {"dtype": "float32", "shape": [2, 2], "data": [1, 2, 3, 4]},
            {"dtype": "float32", "shape": [2], "data": [1, 2], "base64": ""},
            {"dtype": "float32", "shape": [2]},
        ]
        for value in invalid:
            with self.subTest(value=value), self.assertRaises((ValueError, binascii.Error)):
                pipeline.decode_tensors({"x": value})

    def test_invalid_json_shape_rejected_before_numpy_allocation(self):
        for shape, data in (([1], [1, 2]), ([2, 1], [[1], [2, 3]]), ([], [1])):
            with self.subTest(shape=shape, data=data), patch.object(pipeline.np, "asarray") as allocate:
                with self.assertRaises(ValueError):
                    pipeline.decode_tensors({"x": {"dtype": "float32", "shape": shape, "data": data}})
                allocate.assert_not_called()

    def test_rejects_invalid_dimensions_and_oversized_tensors_before_allocation(self):
        for shape in ([-1], [True], [1.5], [1] * 33, [2**50]):
            with self.subTest(shape=shape), self.assertRaises(ValueError):
                pipeline.decode_tensors({"x": {"dtype": "float64", "shape": shape, "data": []}})
        # The byte limit applies to all tensors together, not separately.
        with patch.object(pipeline, "MAX_BODY", 16), self.assertRaises(ValueError):
            pipeline.decode_tensors({
                "a": {"dtype": "float32", "shape": [2], "data": [1, 2]},
                "b": {"dtype": "float32", "shape": [2], "data": [1, 2]},
            })


class ManifestTests(unittest.TestCase):
    def load(self, value):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "pipeline.json"
            path.write_text(json.dumps(value), encoding="utf-8")
            return pipeline.Pipeline(path)

    def test_requires_connected_graph_and_distinct_workers(self):
        invalid = []
        value = manifest()
        value["stages"][0]["inputs"] = {"x": "future"}
        invalid.append(value)
        value = manifest()
        value["outputs"] = ["never-produced"]
        invalid.append(value)
        value = manifest()
        value["stages"][1]["worker"] = value["stages"][0]["worker"]
        invalid.append(value)
        value = manifest()
        value["stages"] = value["stages"][:1]
        invalid.append(value)
        for value in invalid:
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.load(value)

    def test_missing_inputs_never_calls_remote_workers(self):
        runtime = self.load(manifest())
        with patch.object(pipeline, "remote_json") as remote, self.assertRaises(ValueError):
            runtime.run({"wrong": np.ones((1,), dtype=np.float32)})
        remote.assert_not_called()

    def test_worker_health_must_advertise_configured_stage(self):
        runtime = self.load(manifest())
        with patch.object(pipeline, "remote_json", return_value={"status": "ok", "stages": ["other"]}), self.assertRaises(ValueError):
            runtime.check_workers()

    def test_rejects_worker_output_name_mismatch(self):
        runtime = self.load(manifest())
        output = {"outputs": pipeline.encode_tensors({"wrong": np.ones((1,), dtype=np.float32)})}
        with patch.object(pipeline, "remote_json", return_value=output), self.assertRaises(RuntimeError):
            runtime.run({"x": np.ones((1,), dtype=np.float32)})

    def test_rejects_non_http_credentials_paths_and_invalid_ports(self):
        for url in ("file:///tmp/model", "http://user:pass@localhost:80", "http://localhost/path", "http://localhost:99999", "http://localhost?secret=1"):
            with self.subTest(url=url), self.assertRaises(ValueError):
                pipeline.worker_url(url)


class TextGenerationTests(unittest.TestCase):
    def runtime(self, model_context=32, eos=(), selected=3):
        logits = np.full((1, 1, 8), -1.0, dtype=np.float32)
        logits[0, 0, selected] = 1.0
        return SimpleNamespace(inputs=["input_ids"], outputs=["logits"],
                               max_sequence_length=model_context, eos_token_ids=list(eos),
                               run=Mock(return_value={"logits": logits}))

    def tokenizer(self):
        return SimpleNamespace(eos_token_id=1, chat_template="template",
                               encode=Mock(return_value=[2, 3]),
                               apply_chat_template=Mock(return_value=[7, 2]),
                               decode=Mock(return_value="generated"))

    def generator(self, runtime, tokenizer, max_context=32):
        auto = Mock()
        auto.from_pretrained.return_value = tokenizer
        with patch.dict(sys.modules, {"transformers": SimpleNamespace(AutoTokenizer=auto)}):
            return pipeline.TextGeneration(runtime, "local-tokenizer", max_context=max_context)

    def test_chat_uses_tokenized_template_without_encoding_again(self):
        runtime = self.runtime()
        tokenizer = self.tokenizer()
        generator = self.generator(runtime, tokenizer)
        messages = [{"role": "user", "content": "hello"}]
        result = generator.generate({"messages": messages, "max_tokens": 1}, chat=True)
        tokenizer.apply_chat_template.assert_called_once_with(messages, tokenize=True, add_generation_prompt=True)
        tokenizer.encode.assert_not_called()
        np.testing.assert_array_equal(runtime.run.call_args.args[0]["input_ids"], [[7, 2]])
        self.assertEqual(result["usage"]["prompt_tokens"], 2)

    def test_text_context_cannot_exceed_manifest_or_user_limit(self):
        for model_context, user_context, expected in ((8, 16, 8), (32, 8, 8), (None, 8, 8)):
            with self.subTest(model_context=model_context, user_context=user_context):
                runtime = self.runtime(model_context=model_context)
                tokenizer = self.tokenizer()
                tokenizer.encode.return_value = [2] * 6
                generator = self.generator(runtime, tokenizer, max_context=user_context)
                self.assertEqual(generator.max_context, expected)
                with self.assertRaises(ValueError):
                    generator.generate({"prompt": "too long", "max_tokens": 3})
                runtime.run.assert_not_called()

    def test_manifest_eos_list_stops_at_non_tokenizer_eos(self):
        runtime = self.runtime(eos=[5, 7], selected=7)
        tokenizer = self.tokenizer()
        generator = self.generator(runtime, tokenizer)
        result = generator.generate({"prompt": "hello", "max_tokens": 4})
        self.assertNotEqual(tokenizer.eos_token_id, 7)
        self.assertEqual(result["choices"][0]["finish_reason"], "stop")
        self.assertEqual(result["usage"]["completion_tokens"], 1)
        self.assertEqual(runtime.run.call_count, 1)


class ProcessFixture(unittest.TestCase):
    TOKEN = "sdrive-inference-test-token"

    @classmethod
    def reserve_ports(cls, count):
        sockets = []
        try:
            for _ in range(count):
                sock = socket.socket()
                sock.bind(("127.0.0.1", 0))
                sockets.append(sock)
            return [sock.getsockname()[1] for sock in sockets]
        finally:
            for sock in sockets:
                sock.close()

    @classmethod
    def start(cls, mode, path, port, extra=()):
        log = open(cls.root / f"{mode}-{port}.log", "w+", encoding="utf-8")
        cls.logs.append(log)
        env = dict(os.environ, SDRIVE_INFERENCE_TOKEN=cls.TOKEN)
        process = subprocess.Popen([
            sys.executable, str(INFERENCE / "cluster.py"), mode, "--backend", "pipeline",
            "--manifest", str(path), "--port", str(port), "--threads", "1",
        ] + list(extra), cwd=INFERENCE.parent, env=env, stdout=log, stderr=log)
        cls.processes.append(process)
        return process

    @classmethod
    def wait_ready(cls, process, base):
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f"inference process exited ({process.returncode}): {cls.log_text()}")
            try:
                cls.request(base + "/health")
                return
            except (URLError, TimeoutError):
                time.sleep(0.1)
        raise TimeoutError(f"inference process startup timed out: {cls.log_text()}")

    @classmethod
    def request(cls, url, body=None, authorized=True):
        headers = {"Content-Type": "application/json"}
        if authorized:
            headers["Authorization"] = "Bearer " + cls.TOKEN
        data = None if body is None else json.dumps(body).encode()
        with urlopen(Request(url, data=data, headers=headers), timeout=5) as response:
            return json.load(response)

    @classmethod
    def log_text(cls):
        result = []
        for log in cls.logs:
            log.flush()
            log.seek(0)
            result.append(log.read())
        return "\n".join(result)

    @classmethod
    def cleanup(cls):
        for process in reversed(cls.processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        for log in cls.logs:
            log.close()
        cls.temp.cleanup()

    @classmethod
    def tearDownClass(cls):
        cls.cleanup()



@unittest.skipUnless(importlib.util.find_spec("onnx") and importlib.util.find_spec("onnxruntime"), "ONNX integration requires onnx and onnxruntime")
class NativePipelineProcessTests(ProcessFixture):
    """The model is partitioned into Mul and Add ONNX graphs on separate workers."""
    TOKEN = "sdrive-inference-test-token"

    @classmethod
    def setUpClass(cls):
        import onnx
        from onnx import TensorProto, helper, numpy_helper

        cls.temp = tempfile.TemporaryDirectory(prefix="sdrive-native-pipeline-")
        cls.root = Path(cls.temp.name)
        cls.processes = []
        cls.logs = []
        try:
            for index, (operation, input_name, output_name, scalar) in enumerate((
                    ("Mul", "x", "hidden", 2.0), ("Add", "hidden", "y", 1.0))):
                graph = helper.make_graph(
                    [helper.make_node(operation, [input_name, "constant"], [output_name])],
                    f"stage-{index}",
                    [helper.make_tensor_value_info(input_name, TensorProto.FLOAT, [None, 2])],
                    [helper.make_tensor_value_info(output_name, TensorProto.FLOAT, [None, 2])],
                    [numpy_helper.from_array(np.array(scalar, dtype=np.float32), name="constant")],
                )
                model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)], ir_version=9)
                onnx.checker.check_model(model)
                onnx.save(model, cls.root / f"stage-{index}.onnx")
            ports = cls.reserve_ports(3)
            cls.worker_urls = [f"http://127.0.0.1:{value}" for value in ports[:2]]
            cls.base = f"http://127.0.0.1:{ports[2]}"
            config = manifest()
            for index, stage in enumerate(config["stages"]):
                stage["worker"] = cls.worker_urls[index]
                worker = {"version": 1, "stages": [{
                    "id": stage["stage"], "engine": "onnx", "model": f"stage-{index}.onnx",
                    "inputs": list(stage["inputs"]), "outputs": list(stage["outputs"]),
                }]}
                path = cls.root / f"worker-{index}.json"
                path.write_text(json.dumps(worker), encoding="utf-8")
                process = cls.start("worker", path, ports[index])
                cls.wait_ready(process, cls.worker_urls[index])
            path = cls.root / "pipeline.json"
            path.write_text(json.dumps(config), encoding="utf-8")
            process = cls.start("serve", path, ports[2])
            cls.wait_ready(process, cls.base)
        except Exception:
            cls.cleanup()
            raise

    def test_native_two_worker_inference_matches_unsplit_formula(self):
        for value in (np.array([[1, 2], [-3, 0]], dtype=np.float32), np.array([[0.25, 8]], dtype=np.float32)):
            with self.subTest(shape=value.shape):
                body = {"inputs": pipeline.encode_tensors({"x": value}, binary=False)}
                result = self.request(self.base + "/v1/infer", body)
                np.testing.assert_array_equal(pipeline.decode_tensors(result["outputs"])["y"], value * 2 + 1)
                self.assertIn("data", result["outputs"]["y"])
        self.assertEqual(self.request(self.base + "/health"), {"status": "ok", "stages": 2, "workers": 2})
        self.assertEqual(self.request(self.worker_urls[0] + "/health")["stages"], ["encoder"])
        self.assertEqual(self.request(self.worker_urls[1] + "/health")["stages"], ["head"])

    def test_binary_output_encoding(self):
        value = np.array([[3, 4]], dtype=np.float32)
        result = self.request(self.base + "/v1/infer", {"inputs": pipeline.encode_tensors({"x": value}), "encoding": "base64"})
        self.assertIn("base64", result["outputs"]["y"])
        np.testing.assert_array_equal(pipeline.decode_tensors(result["outputs"])["y"], [[7, 9]])

    def test_worker_and_coordinator_reject_missing_authorization(self):
        for base in (self.base, *self.worker_urls):
            with self.subTest(base=base), self.assertRaises(HTTPError) as raised:
                self.request(base + "/health", authorized=False)
            self.assertEqual(raised.exception.code, 401)
        with self.assertRaises(HTTPError) as raised:
            self.request(self.base + "/v1/infer", {"inputs": {}}, authorized=False)
        self.assertEqual(raised.exception.code, 401)

    def test_unicode_authorization_returns_401(self):
        headers = {"Authorization": "Bearer \u00e9"}
        with self.assertRaises(HTTPError) as raised:
            urlopen(Request(self.base + "/health", headers=headers), timeout=5)
        self.assertEqual(raised.exception.code, 401)
        self.assertEqual(self.request(self.base + "/health")["status"], "ok")

    def test_numeric_overflow_is_a_client_error(self):
        with self.assertRaises(HTTPError) as raised:
            self.request(self.base + "/v1/infer", {
                "inputs": {"x": {"dtype": "int64", "shape": [1], "data": [10**100]}},
            })
        self.assertEqual(raised.exception.code, 400)
        self.assertEqual(self.request(self.base + "/health")["status"], "ok")

    def test_invalid_input_fails_without_crashing_server(self):
        with self.assertRaises(HTTPError) as raised:
            self.request(self.base + "/v1/infer", {"inputs": {"x": {"dtype": "float32", "shape": [2], "data": [1]}}})
        self.assertEqual(raised.exception.code, 400)
        self.assertEqual(self.request(self.base + "/health")["status"], "ok")


@unittest.skipUnless(all(importlib.util.find_spec(name) for name in ("torch", "transformers", "tokenizers")), "HF integration requires torch, transformers and tokenizers")
class HuggingFacePipelineProcessTests(ProcessFixture):
    """Native HF Safetensors -> TorchScript partitions -> HTTP text generation."""
    @classmethod
    def setUpClass(cls):
        import torch
        from tokenizers import Tokenizer
        from tokenizers.models import WordLevel
        from tokenizers.pre_tokenizers import Whitespace
        from transformers import GPT2Config, GPT2LMHeadModel, PreTrainedTokenizerFast

        cls.temp = tempfile.TemporaryDirectory(prefix="sdrive-hf-pipeline-")
        cls.root = Path(cls.temp.name)
        cls.processes = []
        cls.logs = []
        try:
            torch.set_num_threads(1)
            torch.manual_seed(7)
            cls.model = GPT2LMHeadModel(GPT2Config(
                vocab_size=8, n_positions=32, n_embd=16, n_layer=2, n_head=2,
                eos_token_id=1, bos_token_id=1,
            )).eval()
            source = cls.root / "source"
            cls.model.save_pretrained(source, safe_serialization=True)
            tokenizer = Tokenizer(WordLevel({
                "<unk>": 0, "<eos>": 1, "hello": 2, "world": 3,
                "one": 4, "two": 5, "three": 6, "four": 7,
            }, unk_token="<unk>"))
            tokenizer.pre_tokenizer = Whitespace()
            tokenizer = PreTrainedTokenizerFast(tokenizer_object=tokenizer, unk_token="<unk>", eos_token="<eos>")
            tokenizer.chat_template = "{% for message in messages %}{{ message['content'] }} {% endfor %}"
            tokenizer.save_pretrained(source)
            export_spec = importlib.util.spec_from_file_location("sdrive_export_hf", INFERENCE / "export_hf.py")
            exporter = importlib.util.module_from_spec(export_spec)
            export_spec.loader.exec_module(exporter)
            ports = cls.reserve_ports(3)
            cls.worker_urls = [f"http://127.0.0.1:{value}" for value in ports[:2]]
            cls.base = f"http://127.0.0.1:{ports[2]}"
            output = cls.root / "export"
            path = exporter.export_model(source, output, cls.worker_urls)
            for index in range(2):
                process = cls.start("worker", output / f"worker-{index}.json", ports[index])
                cls.wait_ready(process, cls.worker_urls[index])
            process = cls.start("serve", path, ports[2], extra=["--tokenizer", str(output / "tokenizer"), "--context", "32"])
            cls.wait_ready(process, cls.base)
        except Exception:
            cls.cleanup()
            raise

    def test_hf_logits_match_unsplit_model_across_http_workers(self):
        import torch
        ids = np.array([[2, 3, 4]], dtype=np.int64)
        result = self.request(self.base + "/v1/infer", {"inputs": pipeline.encode_tensors({"input_ids": ids}, binary=False)})
        actual = pipeline.decode_tensors(result["outputs"])["logits"]
        with torch.inference_mode():
            expected = self.model(torch.from_numpy(ids), use_cache=False).logits[:, -1:, :].numpy()
        self.assertEqual(actual.shape, (1, 1, 8))
        np.testing.assert_allclose(actual, expected, rtol=2e-4, atol=2e-5)
        self.assertEqual(self.request(self.base + "/health")["workers"], 2)

    def test_hf_text_and_chat_completions(self):
        for route, prompt in (("/v1/completions", {"prompt": "hello world"}),
                              ("/v1/chat/completions", {"messages": [{"role": "user", "content": "hello world"}]})):
            with self.subTest(route=route):
                result = self.request(self.base + route, dict(prompt, max_tokens=2, temperature=0))
                self.assertIn(result["choices"][0]["finish_reason"], ("stop", "length"))
                text = result["choices"][0].get("text", result["choices"][0].get("message", {}).get("content"))
                self.assertIsInstance(text, str)
                self.assertEqual(result["usage"]["prompt_tokens"], 2)
                self.assertGreaterEqual(result["usage"]["completion_tokens"], 1)
                self.assertLessEqual(result["usage"]["completion_tokens"], 2)
                self.assertEqual(result["usage"]["total_tokens"], 2 + result["usage"]["completion_tokens"])


if __name__ == "__main__":
    unittest.main()
