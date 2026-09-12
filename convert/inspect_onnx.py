import argparse
from pathlib import Path

import onnx


def _dim_to_text(dim):
    if dim.dim_value:
        return str(dim.dim_value)
    if dim.dim_param:
        return dim.dim_param
    return "?"


def main():
    parser = argparse.ArgumentParser(description="Print ONNX input/output shapes.")
    parser.add_argument("onnx", nargs="+", help="ONNX model path(s)")
    args = parser.parse_args()

    for item in args.onnx:
        path = Path(item)
        model = onnx.load(str(path))
        print(f"===== {path} =====")
        print("inputs:")
        for value in model.graph.input:
            shape = [_dim_to_text(d) for d in value.type.tensor_type.shape.dim]
            print(f"  {value.name}: {shape}")
        print("outputs:")
        for value in model.graph.output:
            shape = [_dim_to_text(d) for d in value.type.tensor_type.shape.dim]
            print(f"  {value.name}: {shape}")


if __name__ == "__main__":
    main()
