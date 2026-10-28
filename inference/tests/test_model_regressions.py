"""Regressions for native dtype/name binding and atomic checkpoint export."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import numpy as np

ROOT = Path(__file__).resolve().parents[1]

def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / (name + ".py"))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result

adapters = module("adapters")
export_hf = module("export_hf")


@unittest.skipUnless(importlib.util.find_spec("torch"), "install torch")
class TorchDtypeRegressionTests(unittest.TestCase):
    def test_torchscript_accepts_non_native_byte_order_values(self):
        import torch
        class Increment(torch.nn.Module):
            def forward(self, value):
                return value + 1
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            torch.jit.trace(Increment(), torch.ones((1, 2))).save(str(root / "increment.pt"))
            stage = adapters.load_stage({"engine": "torchscript", "model": "increment.pt", "inputs": ["x"], "outputs": ["y"]}, root)
            for dtype in (">f4", ">i8"):
                values = np.array([[2, -4]], dtype=dtype)
                with self.subTest(dtype=dtype):
                    np.testing.assert_array_equal(stage.run({"x": values})["y"], [[3, -3]])

    def test_bfloat16_stage_output_is_losslessly_transported_as_float32(self):
        import torch
        class BFloatOutput(torch.nn.Module):
            def forward(self, value):
                return value.to(torch.bfloat16)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            torch.jit.trace(BFloatOutput(), torch.ones((1, 2))).save(str(path / "bf16.pt"))
            stage = adapters.load_stage({"engine": "torchscript", "model": "bf16.pt", "inputs": ["x"], "outputs": ["y"]}, path)
            values = np.array([[1.25, -2.5]], dtype=np.float32)
            result = stage.run({"x": values})["y"]
            self.assertEqual(result.dtype, np.dtype("float32"))
            np.testing.assert_array_equal(result, values)


@unittest.skipUnless(importlib.util.find_spec("tensorflow"), "install tensorflow")
class TensorFlowRegressionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import tensorflow as tf
        tf.config.set_visible_devices([], "GPU")
        try:
            tf.config.threading.set_intra_op_parallelism_threads(1)
            tf.config.threading.set_inter_op_parallelism_threads(1)
        except RuntimeError:
            pass

    def test_keras_input_names_bind_independently_of_manifest_order(self):
        import tensorflow as tf
        for structure in ("dict", "list"):
            with self.subTest(structure=structure), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                left = tf.keras.Input(shape=(1,), name="left")
                right = tf.keras.Input(shape=(1,), name="right")
                inputs = {"left": left, "right": right} if structure == "dict" else [left, right]
                model = tf.keras.Model(inputs, tf.keras.layers.Subtract()([left, right]))
                model.save(root / "subtract.keras")
                config = {"engine": "keras", "model": "subtract.keras", "inputs": ["right", "left"], "outputs": ["y"]}
                stage = adapters.load_stage(config, root)
                result = stage.run({"left": np.array([[10]], dtype=np.float32), "right": np.array([[2]], dtype=np.float32)})
                np.testing.assert_array_equal(result["y"], [[8]])
                config["inputs"] = ["left", "typo"]
                with self.assertRaisesRegex(ValueError, "Keras input"):
                    adapters.load_stage(config, root)

    def test_savedmodel_rejects_wrong_signature_names_at_startup(self):
        import tensorflow as tf
        class Model(tf.Module):
            @tf.function(input_signature=[tf.TensorSpec([None, 1], tf.float32, name="x")])
            def __call__(self, x):
                return {"y": x + 1}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tf.saved_model.save(Model(), str(root / "model"))
            for inputs, outputs in ((["typo"], ["y"]), (["x"], ["typo"])):
                with self.subTest(inputs=inputs, outputs=outputs), self.assertRaisesRegex(ValueError, "SavedModel"):
                    adapters.load_stage({"engine": "tensorflow", "model": "model", "inputs": inputs, "outputs": outputs}, root)

    def test_savedmodel_bfloat16_output_is_float32(self):
        import tensorflow as tf
        class Model(tf.Module):
            @tf.function(input_signature=[tf.TensorSpec([None, 2], tf.float32, name="x")])
            def __call__(self, x):
                return {"y": tf.cast(x, tf.bfloat16)}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tf.saved_model.save(Model(), str(root / "model"))
            stage = adapters.load_stage({"engine": "tensorflow", "model": "model", "inputs": ["x"], "outputs": ["y"]}, root)
            result = stage.run({"x": np.array([[1.25, -2.5]], dtype=np.float32)})["y"]
            self.assertEqual(result.dtype, np.dtype("float32"))
            np.testing.assert_array_equal(result, [[1.25, -2.5]])


@unittest.skipUnless(all(importlib.util.find_spec(name) for name in ("torch", "transformers")), "install HF requirements")
class ExportTransactionRegressionTests(unittest.TestCase):
    def test_failed_export_leaves_destination_clean_and_retry_works(self):
        import torch
        from transformers import GPT2Config, GPT2LMHeadModel
        torch.set_num_threads(1)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            model = GPT2LMHeadModel(GPT2Config(vocab_size=16, n_positions=16, n_embd=8, n_layer=2, n_head=2))
            model.save_pretrained(source)
            destination = root / "export"
            write_text = Path.write_text
            def fail_second_manifest(path, *args, **kwargs):
                if path.name == "worker-1.json":
                    raise OSError("simulated disk write failure")
                return write_text(path, *args, **kwargs)
            with patch.object(Path, "write_text", fail_second_manifest), self.assertRaisesRegex(OSError, "disk write failure"):
                export_hf.export_model(source, destination, ["http://pc1:50052", "http://pc2:50052"])
            self.assertFalse(destination.exists(), "a failed export must not leave ready-looking partial model files")
            path = export_hf.export_model(source, destination, ["http://pc1:50052", "http://pc2:50052"])
            self.assertEqual(path, destination / "pipeline.json")
            self.assertEqual(len(json.loads(path.read_text())["stages"]), 2)
