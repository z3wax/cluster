"""Native format tests create tiny local models; no model downloads are needed."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

import numpy as np

spec = importlib.util.spec_from_file_location("stage_adapters", Path(__file__).parents[1] / "adapters.py")
adapters = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapters)


def installed(name):
    return importlib.util.find_spec(name) is not None


class AdapterContractTests(unittest.TestCase):
    def test_invalid_config_rejected_before_loading(self):
        cases = [None, {}, {"engine": "anything", "inputs": ["x"], "outputs": ["y"]},
                 {"engine": "python", "inputs": ["x", "x"], "outputs": ["y"]},
                 {"engine": "python", "inputs": ["x"], "outputs": []}]
        for config in cases:
            with self.subTest(config=config), self.assertRaises(ValueError):
                adapters.load_stage(config, Path("."))

    def test_python_factory_contract_and_tensor_validation(self):
        module = types.ModuleType("test_local_adapter")
        received = {}
        def create(**kwargs):
            received.update(kwargs)
            return types.SimpleNamespace(run=lambda values: {"y": values["x"] * 2})
        module.create = create
        config = {"engine": "python", "factory": "test_local_adapter:create", "inputs": ["x"], "outputs": ["y"]}
        with patch.dict(sys.modules, {"test_local_adapter": module}):
            stage = adapters.load_stage(config, Path("."), threads=2)
        self.assertEqual(received["config"], config)
        self.assertEqual(received["threads"], 2)
        self.assertIsInstance(received["root"], Path)
        np.testing.assert_equal(stage.run({"x": np.array([1.0, 2.0])})["y"], [2, 4])
        with self.assertRaises(ValueError):
            stage.run({"unexpected": np.array([1])})
        with self.assertRaises(ValueError):
            stage.run({"x": np.array([object()], dtype=object)})

    def test_output_contract(self):
        config = {"inputs": ["x"], "outputs": ["a", "b"]}
        stage = adapters._Stage(config, lambda values: [values["x"]])
        with self.assertRaises(ValueError):
            stage.run({"x": np.array([1])})
        stage = adapters._Stage(config, lambda values: {"a": values["x"]})
        with self.assertRaises(ValueError):
            stage.run({"x": np.array([1])})

    def test_missing_dependency_message(self):
        with patch.object(adapters, "import_module", side_effect=ImportError("missing")):
            with self.assertRaisesRegex(ValueError, "pip install onnxruntime"):
                adapters._dependency("onnxruntime", "onnxruntime")


@unittest.skipUnless(installed("onnx") and installed("onnxruntime"), "install requirements-onnx.txt plus onnx")
class ONNXTests(unittest.TestCase):
    def test_real_onnx_cpu_inference_and_names(self):
        import onnx
        from onnx import TensorProto, helper
        x = helper.make_tensor_value_info("x", TensorProto.FLOAT, [None, 2])
        y = helper.make_tensor_value_info("y", TensorProto.FLOAT, [None, 2])
        graph = helper.make_graph([helper.make_node("Relu", ["x"], ["y"])], "tiny", [x], [y])
        model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
        model.ir_version = 9
        with tempfile.TemporaryDirectory() as folder:
            onnx.save(model, str(Path(folder) / "tiny.onnx"))
            config = {"engine": "onnx", "model": "tiny.onnx", "inputs": ["x"], "outputs": ["y"]}
            stage = adapters.load_stage(config, folder)
            actual = stage.run({"x": np.array([[-1, 3]], dtype=np.float32)})
            np.testing.assert_array_equal(actual["y"], [[0, 3]])
            config["inputs"] = ["wrong"]
            with self.assertRaisesRegex(ValueError, "ONNX nomlari"):
                adapters.load_stage(config, folder)


@unittest.skipUnless(installed("torch"), "install requirements-torch.txt")
class TorchScriptTests(unittest.TestCase):
    def test_real_torchscript_cpu_inference(self):
        import torch
        class Block(torch.nn.Module):
            def forward(self, x, residual):
                return torch.relu(x + residual), x - residual
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "stage.pt"
            torch.jit.trace(Block().eval(), (torch.zeros(1, 2), torch.zeros(1, 2))).save(str(path))
            config = {"engine": "torchscript", "model": str(path), "inputs": ["x", "residual"], "outputs": ["y", "delta"]}
            stage = adapters.load_stage(config, folder)
            actual = stage.run({"x": np.array([[-2, 3]], dtype=np.float32), "residual": np.ones((1, 2), dtype=np.float32)})
            np.testing.assert_array_equal(actual["y"], [[0, 4]])
            np.testing.assert_array_equal(actual["delta"], [[-3, 2]])

    def test_raw_state_dict_not_implicitly_unpickled(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "weights.pt"
            path.write_bytes(b"not a TorchScript archive")
            with self.assertRaisesRegex(ValueError, "state_dict"):
                adapters.load_stage({"engine": "torchscript", "model": str(path), "inputs": ["x"], "outputs": ["y"]}, folder)


@unittest.skipUnless(installed("tensorflow"), "install requirements-tensorflow.txt")
class TensorFlowTests(unittest.TestCase):
    def test_real_savedmodel_cpu_inference(self):
        import tensorflow as tf
        class Block(tf.Module):
            @tf.function(input_signature=[tf.TensorSpec(shape=[None, 2], dtype=tf.float32, name="x")])
            def run(self, x):
                return {"y": tf.nn.relu(x)}
        with tempfile.TemporaryDirectory() as folder:
            model = Block()
            path = Path(folder) / "saved_model"
            tf.saved_model.save(model, str(path), signatures={"serving_default": model.run})
            stage = adapters.load_stage({"engine": "tensorflow", "model": "saved_model", "inputs": ["x"], "outputs": ["y"]}, folder)
            np.testing.assert_array_equal(stage.run({"x": np.array([[-1, 3]], dtype=np.float32)})["y"], [[0, 3]])

    def test_real_keras_archive_and_hdf5_cpu_inference(self):
        import tensorflow as tf
        model = tf.keras.Sequential([tf.keras.Input(shape=(2,)), tf.keras.layers.Activation("relu")])
        with tempfile.TemporaryDirectory() as folder:
            for suffix in (".keras", ".h5"):
                with self.subTest(suffix=suffix):
                    path = Path(folder) / ("tiny" + suffix)
                    model.save(str(path))
                    stage = adapters.load_stage({"engine": "keras", "model": path.name, "inputs": ["x"], "outputs": ["y"]}, folder)
                    np.testing.assert_array_equal(stage.run({"x": np.array([[-1, 3]], dtype=np.float32)})["y"], [[0, 3]])


if __name__ == "__main__":
    unittest.main()
