import argparse
from pathlib import Path

from rknn.api import RKNN


# This script is intentionally kept as a simple "edit and run" converter.
# It tries to convert the static 640 ONNX into a 320 RKNN by overriding
# RKNN load_onnx input_size_list. If RKNN reports shape mismatch, this ONNX
# cannot be safely forced to 320 and needs to be exported from the original
# model at imgsz=320.

SCRIPT_DIR = Path(__file__).resolve().parent

# Same-folder mode:
# Put convert11.py, your .onnx, dataset.txt and images/ in the same directory:
#   ~/rknn_model_zoo/examples/yolo11/python/
# Then run:
#   python3 convert11.py
#
# If there is more than one non-split .onnx in this directory, specify one:
#   python3 convert11.py --onnx Bb_20260420_v11s_320.onnx
ONNX_MODEL_NAME = ""
RKNN_MODEL_NAME = ""
DATASET_TXT = SCRIPT_DIR / "dataset.txt"

TARGET_PLATFORM = "rk3588"
INPUT_SIZE = 320
DO_QUANT = True
NUM_CLASSES = 6
SPLIT_YOLO_OUTPUT = True


def resolve_onnx_path(name):
    if name:
        path = Path(name)
        return path if path.is_absolute() else SCRIPT_DIR / path

    candidates = sorted(
        p for p in SCRIPT_DIR.glob("*.onnx")
        if "_split_" not in p.stem and not p.name.startswith(".")
    )
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        raise SystemExit(f"No .onnx found in: {SCRIPT_DIR}")

    print("Multiple .onnx files found. Please specify one with --onnx:")
    for p in candidates:
        print(f"  {p.name}")
    raise SystemExit(2)


def resolve_rknn_path(name, onnx_path, num_classes, split_enabled):
    if name:
        path = Path(name)
        return path if path.is_absolute() else SCRIPT_DIR / path
    suffix = f"_split_{num_classes}cls" if split_enabled else ""
    return onnx_path.with_name(onnx_path.stem + suffix + ".rknn")


def make_split_output_onnx(src_path, num_classes, split_enabled=True):
    """Split YOLO 1x(4+nc)xN output into boxes and scores before RKNN quantizes it."""
    if not split_enabled:
        return src_path

    try:
        import onnx
        from onnx import TensorProto, helper, numpy_helper
        import numpy as np
    except ImportError as exc:
        raise SystemExit(
            "SPLIT_YOLO_OUTPUT=True needs the onnx Python package. "
            "Install it in WSL with: pip install onnx"
        ) from exc

    model = onnx.load(str(src_path))
    graph = model.graph
    if len(graph.output) != 1:
        print(f"    split skipped: model has {len(graph.output)} outputs")
        return src_path

    output = graph.output[0]
    output_name = output.name
    dims = []
    tensor_type = output.type.tensor_type
    if tensor_type.HasField("shape"):
        for dim in tensor_type.shape.dim:
            if dim.HasField("dim_value"):
                dims.append(int(dim.dim_value))
            else:
                dims.append(None)

    expected_c = 4 + int(num_classes)
    channel_axis = None
    for axis, dim in enumerate(dims):
        if dim == expected_c:
            channel_axis = axis
            break
    if channel_axis is None and len(dims) == 3:
        # Most YOLO exports are [1, C, N]. Fall back to the smaller non-batch axis.
        if dims[1] is not None and dims[1] <= 512:
            channel_axis = 1
        elif dims[2] is not None and dims[2] <= 512:
            channel_axis = 2
    if channel_axis is None:
        raise SystemExit(
            f"Cannot find YOLO channel axis in output shape {dims}. "
            f"Expected one axis to be {expected_c}."
        )

    def add_const(name, values):
        arr = np.asarray(values, dtype=np.int64)
        graph.initializer.append(numpy_helper.from_array(arr, name))
        return name

    boxes_name = output_name + "_boxes"
    scores_name = output_name + "_scores"
    graph.initializer.extend([])
    graph.node.extend([
        helper.make_node(
            "Slice",
            inputs=[
                output_name,
                add_const("split_boxes_starts", [0]),
                add_const("split_boxes_ends", [4]),
                add_const("split_boxes_axes", [channel_axis]),
                add_const("split_boxes_steps", [1]),
            ],
            outputs=[boxes_name],
            name="SplitYoloBoxes",
        ),
        helper.make_node(
            "Slice",
            inputs=[
                output_name,
                add_const("split_scores_starts", [4]),
                add_const("split_scores_ends", [expected_c]),
                add_const("split_scores_axes", [channel_axis]),
                add_const("split_scores_steps", [1]),
            ],
            outputs=[scores_name],
            name="SplitYoloScores",
        ),
    ])

    boxes_dims = list(dims) if dims else None
    scores_dims = list(dims) if dims else None
    if boxes_dims:
        boxes_dims[channel_axis] = 4
    if scores_dims:
        scores_dims[channel_axis] = int(num_classes)

    del graph.output[:]
    graph.output.extend([
        helper.make_tensor_value_info(boxes_name, TensorProto.FLOAT, boxes_dims),
        helper.make_tensor_value_info(scores_name, TensorProto.FLOAT, scores_dims),
    ])

    split_path = src_path.with_name(src_path.stem + f"_split_{num_classes}cls.onnx")
    onnx.checker.check_model(model)
    onnx.save(model, str(split_path))
    print(f"    split output: {output_name} shape={dims} axis={channel_axis}")
    print(f"    split onnx: {split_path}")
    return split_path


def main():
    parser = argparse.ArgumentParser(description="Convert YOLOv11 ONNX to RKNN for RK3588.")
    parser.add_argument("--onnx", default=ONNX_MODEL_NAME, help="ONNX filename/path. Default: auto-pick the only .onnx beside this script.")
    parser.add_argument("--out", default=RKNN_MODEL_NAME, help="RKNN output filename/path. Default: beside ONNX.")
    parser.add_argument("--dataset", default=str(DATASET_TXT), help="dataset.txt path. Default: beside this script.")
    parser.add_argument("--classes", type=int, default=NUM_CLASSES, help="Class count. For your Bb model this should be 6.")
    parser.add_argument("--input-size", type=int, default=INPUT_SIZE, help="Input size, default 320.")
    parser.add_argument("--no-quant", action="store_true", help="Disable INT8 quantization for debugging.")
    parser.add_argument("--no-split", action="store_true", help="Do not split 1x(4+nc)xN output into boxes/scores.")
    args = parser.parse_args()

    do_quant = DO_QUANT and not args.no_quant
    split_enabled = SPLIT_YOLO_OUTPUT and not args.no_split
    onnx_model = resolve_onnx_path(args.onnx)
    rknn_model = resolve_rknn_path(args.out, onnx_model, args.classes, split_enabled)
    dataset_txt = Path(args.dataset)
    if not dataset_txt.is_absolute():
        dataset_txt = SCRIPT_DIR / dataset_txt

    if not onnx_model.exists():
        raise SystemExit(f"ONNX model not found: {onnx_model}")
    if do_quant and not dataset_txt.exists():
        raise SystemExit(f"dataset.txt not found: {dataset_txt}")

    rknn = RKNN(verbose=True)
    try:
        print("--> Config model")
        rknn.config(
            mean_values=[[0, 0, 0]],
            std_values=[[255, 255, 255]],
            target_platform=TARGET_PLATFORM,
            quantized_dtype="asymmetric_quantized-8",
            quantized_algorithm="normal",
        )
        print("done")

        print("--> Loading ONNX model")
        load_model = make_split_output_onnx(onnx_model, args.classes, split_enabled)
        print(f"    onnx: {load_model}")
        print(f"    force input: 1x3x{args.input_size}x{args.input_size}")
        ret = rknn.load_onnx(
            model=str(load_model),
            input_size_list=[[1, 3, args.input_size, args.input_size]],
        )
        if ret != 0:
            print("Load ONNX model failed.")
            print("This usually means the 640 ONNX has fixed 640-only shapes.")
            print("If so, export a real 320 ONNX from the original .pt instead.")
            raise SystemExit(ret)
        print("done")

        print("--> Building RKNN model")
        ret = rknn.build(
            do_quantization=do_quant,
            dataset=str(dataset_txt) if do_quant else None,
        )
        if ret != 0:
            print("Build RKNN model failed.")
            print("If the error is shape-related, this 640 ONNX cannot be forced to 320.")
            raise SystemExit(ret)
        print("done")

        print("--> Export RKNN model")
        rknn_model.parent.mkdir(parents=True, exist_ok=True)
        ret = rknn.export_rknn(str(rknn_model))
        if ret != 0:
            print("Export RKNN model failed.")
            raise SystemExit(ret)
        print("done")

        print(f"Convert success: {rknn_model}")
    finally:
        rknn.release()


if __name__ == "__main__":
    main()
