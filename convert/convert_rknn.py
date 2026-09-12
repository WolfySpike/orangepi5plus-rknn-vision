import argparse
from pathlib import Path

from rknn.api import RKNN


def parse_size(text):
    raw = str(text).lower().replace(",", "x").split("x")
    if len(raw) == 1:
        h = w = int(raw[0])
    elif len(raw) == 2:
        h, w = int(raw[0]), int(raw[1])
    else:
        raise argparse.ArgumentTypeError("size must be like 320 or 320x320")
    if h <= 0 or w <= 0:
        raise argparse.ArgumentTypeError("size must be positive")
    return h, w


def default_output_path(onnx_path):
    return onnx_path.with_suffix(".rknn")


def main():
    parser = argparse.ArgumentParser(description="Convert ONNX to RKNN for RK3588.")
    parser.add_argument("--onnx", required=True, help="Input ONNX model path")
    parser.add_argument("--out", default=None, help="Output RKNN path")
    parser.add_argument("--target", default="rk3588", help="RKNN target platform")
    parser.add_argument("--input-size", type=parse_size, default=None, help="Override ONNX input size, e.g. 320 or 320x320")
    parser.add_argument("--dataset", default=None, help="Quantization dataset txt")
    parser.add_argument("--no-quant", action="store_true", help="Disable INT8 quantization")
    parser.add_argument("--verbose", action="store_true", help="Verbose RKNN logs")
    args = parser.parse_args()

    onnx_path = Path(args.onnx)
    out_path = Path(args.out) if args.out else default_output_path(onnx_path)
    do_quant = not args.no_quant

    if not onnx_path.exists():
        raise SystemExit(f"ONNX not found: {onnx_path}")
    if do_quant:
        if not args.dataset:
            raise SystemExit("INT8 quantization needs --dataset. Use --no-quant for a debug conversion.")
        dataset_path = Path(args.dataset)
        if not dataset_path.exists():
            raise SystemExit(f"Dataset txt not found: {dataset_path}")
    else:
        dataset_path = None

    rknn = RKNN(verbose=args.verbose)
    try:
        print("--> Config")
        rknn.config(
            mean_values=[[0, 0, 0]],
            std_values=[[255, 255, 255]],
            target_platform=args.target,
            quantized_dtype="asymmetric_quantized-8",
            quantized_algorithm="normal",
        )

        print("--> Load ONNX")
        load_kwargs = {}
        if args.input_size:
            h, w = args.input_size
            load_kwargs["input_size_list"] = [[1, 3, h, w]]
            print(f"    input_size_list={load_kwargs['input_size_list']}")
            print("    note: this only works for dynamic or shape-compatible ONNX models.")
        ret = rknn.load_onnx(model=str(onnx_path), **load_kwargs)
        if ret != 0:
            raise SystemExit(f"Load ONNX failed: {ret}")

        print("--> Build")
        ret = rknn.build(do_quantization=do_quant, dataset=str(dataset_path) if dataset_path else None)
        if ret != 0:
            raise SystemExit(f"Build RKNN failed: {ret}")

        print("--> Export")
        out_path.parent.mkdir(parents=True, exist_ok=True)
        ret = rknn.export_rknn(str(out_path))
        if ret != 0:
            raise SystemExit(f"Export RKNN failed: {ret}")
        print(f"OK: {out_path}")
    finally:
        rknn.release()


if __name__ == "__main__":
    main()
