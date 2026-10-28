"""Native CPU model stages for a configured distributed tensor pipeline.

``load_stage(config, root, threads)`` returns an object implementing
``run(dict[str, numpy.ndarray]) -> dict[str, numpy.ndarray]``.  A stage names
its engine, local model path and ordered ``inputs`` / ``outputs``. Optional
``options`` currently supports TensorFlow's SavedModel ``signature``.

Only trusted, administrator-supplied manifests may choose models or factories.
TorchScript/Keras are executable artifacts, not safe upload formats. This module
never loads an arbitrary PyTorch pickle/state_dict as an executable model.

A ``python`` stage specifies ``factory: 'package.module:function'``. The factory
is called as ``factory(config=config, root=root, threads=threads)`` and returns
an object with ``run(inputs)``. This is the extension point for architecture-
specific Hugging Face/Safetensors/state_dict or other framework runtimes.
Dependencies are imported only for the selected engine.
"""
from importlib import import_module
from pathlib import Path
import re

import numpy as np


def _dependency(module, requirement):
    try:
        return import_module(module)
    except ImportError as exc:
        raise ValueError(f"{module} o'rnatilmagan; pip install {requirement}") from exc


def _names(config, key):
    names = config.get(key)
    if not isinstance(names, list) or not names or any(not isinstance(name, str) or not name for name in names):
        raise ValueError(f"stage {key}: bo'sh bo'lmagan nomlar ro'yxati kerak")
    if len(set(names)) != len(names):
        raise ValueError(f"stage {key}: nomlar takrorlanmasin")
    return names


def _path(config, root):
    value = config.get("model")
    if not isinstance(value, str) or not value:
        raise ValueError("stage model: lokal model yo'li kerak")
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = root / path
    path = path.resolve()
    if not path.exists():
        raise ValueError(f"model topilmadi: {path}")
    return path


def _array(value):
    if hasattr(value, "detach"):
        value = value.detach().cpu().numpy()
    elif hasattr(value, "numpy"):
        value = value.numpy()
    array = np.asarray(value)
    if array.dtype.kind not in "biuf":
        raise ValueError(f"faqat boolean, integer va floating tensorlar qo'llanadi: {array.dtype}")
    return array


class _Stage:
    def __init__(self, config, execute):
        self.inputs = _names(config, "inputs")
        self.outputs = _names(config, "outputs")
        self.execute = execute

    def run(self, inputs):
        if not isinstance(inputs, dict) or set(inputs) != set(self.inputs):
            raise ValueError(f"stage kirish nomlari {self.inputs} bo'lishi kerak")
        values = self.execute({name: _array(inputs[name]) for name in self.inputs})
        if isinstance(values, dict):
            if not set(self.outputs).issubset(values):
                raise ValueError(f"model chiqishlari {self.outputs} nomlarini bermadi")
            return {name: _array(values[name]) for name in self.outputs}
        if len(self.outputs) == 1 and not isinstance(values, (tuple, list)):
            values = [values]
        if not isinstance(values, (tuple, list)) or len(values) != len(self.outputs):
            raise ValueError("model chiqishlari soni manifest bilan mos emas")
        return {name: _array(value) for name, value in zip(self.outputs, values)}


def load_stage(config, root, threads=1):
    """Load one trusted local stage. ``root`` resolves relative model paths."""
    if not isinstance(config, dict):
        raise ValueError("stage JSON object bo'lishi kerak")
    if isinstance(threads, bool) or not isinstance(threads, int) or threads < 1:
        raise ValueError("threads musbat butun son bo'lishi kerak")
    inputs = _names(config, "inputs")
    outputs = _names(config, "outputs")
    root = Path(root).expanduser().resolve()
    engine = config.get("engine")
    if engine == "python":
        factory_path = config.get("factory", "")
        if not isinstance(factory_path, str) or not re.fullmatch(r"[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*:[A-Za-z_]\w*", factory_path):
            raise ValueError("python factory package.module:function formatida bo'lishi kerak")
        module_name, function_name = factory_path.split(":")
        try:
            factory = getattr(import_module(module_name), function_name)
        except (ImportError, AttributeError) as exc:
            raise ValueError(f"python factory topilmadi: {factory_path}") from exc
        if not callable(factory):
            raise ValueError("python factory callable bo'lishi kerak")
        stage = factory(config=config, root=root, threads=threads)
        if not callable(getattr(stage, "run", None)):
            raise ValueError("python factory run(inputs) metodli obyekt qaytarishi kerak")
        return _Stage(config, stage.run)
    if engine not in ("onnx", "torchscript", "tensorflow", "keras"):
        raise ValueError(f"noma'lum engine: {engine}; onnx, torchscript, tensorflow, keras yoki python ishlating")
    path = _path(config, root)
    if engine == "onnx":
        ort = _dependency("onnxruntime", "onnxruntime")
        options = ort.SessionOptions()
        options.intra_op_num_threads = threads
        options.inter_op_num_threads = 1
        session = ort.InferenceSession(str(path), sess_options=options, providers=["CPUExecutionProvider"])
        available_inputs = {value.name for value in session.get_inputs()}
        available_outputs = {value.name for value in session.get_outputs()}
        if set(inputs) != available_inputs or not set(outputs).issubset(available_outputs):
            raise ValueError(f"ONNX nomlari mos emas: inputs={sorted(available_inputs)}, outputs={sorted(available_outputs)}")
        return _Stage(config, lambda values: session.run(outputs, values))
    if engine == "torchscript":
        torch = _dependency("torch", "torch")
        torch.set_num_threads(threads)
        try:
            model = torch.jit.load(str(path), map_location="cpu")
        except Exception as exc:
            raise ValueError("TorchScript model kerak (torch.jit.save); oddiy state_dict yoki .bin uchun python adapter ishlating") from exc
        model.eval()
        def execute(values):
            with torch.inference_mode():
                return model(*(torch.from_numpy(np.array(values[name], copy=True)) for name in inputs))
        return _Stage(config, execute)
    tf = _dependency("tensorflow", "tensorflow-cpu")
    try:
        tf.config.set_visible_devices([], "GPU")
    except RuntimeError as exc:
        if tf.config.get_visible_devices("GPU"):
            raise ValueError("TensorFlow GPU allaqachon ishga tushgan; CPU worker'ni alohida jarayonda boshlang") from exc
    # Multiple stages may share this runtime after TensorFlow initializes.
    try:
        tf.config.threading.set_intra_op_parallelism_threads(threads)
        tf.config.threading.set_inter_op_parallelism_threads(1)
    except RuntimeError:
        pass
    with tf.device("/CPU:0"):
        if engine == "tensorflow":
            model = tf.saved_model.load(str(path))
            options = config.get("options", {})
            if not isinstance(options, dict):
                raise ValueError("stage options object bo'lishi kerak")
            signature = options.get("signature", "serving_default")
            if signature not in model.signatures:
                raise ValueError(f"SavedModel signature topilmadi: {signature}")
            call = model.signatures[signature]
            def execute(values):
                with tf.device("/CPU:0"):
                    return call(**{name: tf.convert_to_tensor(values[name]) for name in inputs})
        else:
            model = tf.keras.models.load_model(str(path), compile=False, safe_mode=True)
            def execute(values):
                with tf.device("/CPU:0"):
                    tensors = [tf.convert_to_tensor(values[name]) for name in inputs]
                    return model(tensors[0] if len(tensors) == 1 else tensors, training=False)
    return _Stage(config, execute)
