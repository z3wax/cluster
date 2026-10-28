#!/usr/bin/env python3
"""Create a small two-stage ONNX model and copy example manifests beside it."""
import argparse
from pathlib import Path
import shutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("onnx-demo"))
    args = parser.parse_args()
    import onnx
    from onnx import TensorProto, helper
    args.output.mkdir(parents=True, exist_ok=True)
    for name, source, target, operation, value in (
        ("encoder", "x", "hidden", "Mul", 2.0),
        ("head", "hidden", "y", "Add", 1.0),
    ):
        graph = helper.make_graph(
            [helper.make_node(operation, [source, "constant"], [target])],
            name,
            [helper.make_tensor_value_info(source, TensorProto.FLOAT, ["batch", 2])],
            [helper.make_tensor_value_info(target, TensorProto.FLOAT, ["batch", 2])],
            [helper.make_tensor("constant", TensorProto.FLOAT, [1], [value])],
        )
        model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
        model.ir_version = 8
        onnx.checker.check_model(model)
        onnx.save(model, args.output / f"{name}.onnx")
    for name in ("worker-encoder.json", "worker-head.json", "pipeline.json"):
        shutil.copyfile(Path(__file__).with_name(name), args.output / name)
    print(f"Model va manifestlar: {args.output.resolve()}")


if __name__ == "__main__":
    main()
