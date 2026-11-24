#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors.
# Modified by the OBS Native NR contributors on 2026-11-22.
# Adapted PushFPS prepare_runtime.py and rife_graph.py into a standalone model-only build.
"""Build a separate RIFE model ZIP. Run with uv run, or use the listed dependencies."""
# /// script
# requires-python = ">=3.11"
# dependencies = ["numpy==2.2.6", "onnx==1.18.0", "onnxruntime==1.20.1"]
# ///

from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile
from collections import deque
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Final

import numpy as np
import onnx
import onnxruntime as ort
from numpy.typing import NDArray
from onnx import TensorProto, helper

RELEASE: Final = "https://github.com/AmusementClub/vs-mlrt/releases/download/external-models"
PACK_NAME: Final = "pushflow-rife-models-v1"
OUTPUTS: Final = ("pushflow_grid0", "pushflow_grid1", "pushflow_mask")
RIFE_MIT: Final = """MIT License

Copyright (c) 2021 hzwer

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
"""


@dataclass(frozen=True, slots=True)
class ModelPin:
    stem: str
    role: str
    archive_sha: str
    model_sha: str


@dataclass(frozen=True, slots=True)
class ModelBuildError(Exception):
    detail: str

    def __str__(self) -> str:
        return self.detail


MODELS: Final = (
    ModelPin("rife_v4.25_lite", "fast", "7d53e29fff5e67345b19f4ce97dfd1e34b490eedc752df2236904fda1a13842c",
             "610b5de57cdcfbcce9914c23e60a1cd357779a6f9582a1bcfcb035f8eb38509b"),
    ModelPin("rife_v4.26", "quality", "dfdabd84a2a3db773f87604b8cc255e94a6a72f13550d910ccd3b4ee2606cd4f",
             "af25762dfec02a4bbb949decea63988b01fa56c46c0ff9dc66ac8e2f12cbb661"),
)


def sha256(path: Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def get_model(pin: ModelPin, workspace: Path, archives: Path | None) -> Path:
    name = f"{pin.stem}.7z"
    archive = workspace / name
    if archives is None:
        with urllib.request.urlopen(f"{RELEASE}/{name}", timeout=60) as response, archive.open("xb") as output:
            shutil.copyfileobj(response, output)
    else:
        shutil.copyfile(archives / name, archive)
    if sha256(archive) != pin.archive_sha:
        raise ModelBuildError(f"Archive SHA-256 mismatch: {name}")
    member = f"rife_v2/{pin.stem}.onnx"
    listing = subprocess.run(["tar", "-tf", str(archive)], check=True, capture_output=True, text=True, timeout=60)
    names = listing.stdout.splitlines()
    for name_in_archive in names:
        name_to_check = name_in_archive.replace("\\", "/")
        path = PurePosixPath(name_to_check)
        if path.is_absolute() or ":" in name_to_check or ".." in path.parts:
            raise ModelBuildError("Invalid archive entry path")
    if names.count(member) != 1:
        raise ModelBuildError(f"Archive is missing its unique model member: {member}")
    model = workspace / f"{pin.stem}.onnx"
    with model.open("xb") as output:
        subprocess.run(["tar", "-xOf", str(archive), "--", member], check=True, stdout=output, timeout=60)
    if sha256(model) != pin.model_sha:
        raise ModelBuildError(f"Model SHA-256 mismatch: {pin.stem}")
    return model


def expose(model: onnx.ModelProto) -> tuple[int, str]:
    producers = {name: node for node in model.graph.node for name in node.output}
    seen = {model.graph.output[0].name}
    pending = deque(seen)
    warp: onnx.NodeProto | None = None
    mask: onnx.NodeProto | None = None
    while pending and (warp is None or mask is None):
        node = producers.get(pending.popleft())
        if node is None:
            continue
        match node.op_type:
            case "GridSample" if warp is None:
                warp = node
            case "Sigmoid" if mask is None:
                mask = node
            case _:
                pass
        pending.extend(name for name in node.input if name and name not in seen)
        seen.update(node.input)
    if warp is None or mask is None:
        raise ModelBuildError("RIFE final blend is missing its warp or mask")
    attrs = {attr.name: attr for attr in warp.attribute}
    align = attrs["align_corners"].i if "align_corners" in attrs else 0
    padding = attrs["padding_mode"].s.decode("ascii") if "padding_mode" in attrs else "zeros"
    if padding != "border":
        raise ModelBuildError(f"Unsupported RIFE warp padding: {padding}")
    for value in range(3):
        model.graph.initializer.append(helper.make_tensor(f"pushflow_bound{value}", TensorProto.INT64, [1], [value]))
    for index, output in enumerate(OUTPUTS[:2]):
        model.graph.node.append(helper.make_node(
            "Slice", [warp.input[1], f"pushflow_bound{index}", f"pushflow_bound{index + 1}"], [output],
        ))
    model.graph.node.append(helper.make_node("Identity", [mask.output[0]], [OUTPUTS[2]]))
    shapes = ([1, "pushflow_h", "pushflow_w", 2], [1, "pushflow_h", "pushflow_w", 2],
              [1, 1, "pushflow_h", "pushflow_w"])
    for name, shape in zip(OUTPUTS, shapes, strict=True):
        model.graph.output.append(helper.make_tensor_value_info(name, TensorProto.FLOAT, shape))
    return align, padding


def frame(x: NDArray[np.float32], y: NDArray[np.float32], left: int) -> NDArray[np.float32]:
    base = (60 + 40 * x / 256 + 30 * y / 256) / 255
    dx, dy = x - left, y - 90
    inside = (dx >= 0) & (dx < 40) & (dy >= 0) & (dy < 40)
    light = (dx // 8 + dy // 8) % 2 == 0
    return np.stack([
        np.where(inside, np.where(light, hi, lo) / 255, base + index * 10 / 255)
        for index, (lo, hi) in enumerate(((40, 235), (30, 200), (20, 60)))
    ]).astype(np.float32)


def grid_sample(img: NDArray[np.float32], field: NDArray[np.float32], align: int) -> NDArray[np.float64]:
    grid = field[0]
    if grid.shape != (256, 256, 2) or not np.isfinite(grid).all():
        raise ModelBuildError("Unexpected RIFE output grid")
    x, y = np.moveaxis(grid.astype(np.float64), -1, 0)
    x, y = x * (127.5 if align else 128) + 127.5, y * (127.5 if align else 128) + 127.5
    x, y = np.clip(x, 0, 255), np.clip(y, 0, 255)
    x0, y0 = x.astype(np.int64), y.astype(np.int64)
    x1, y1 = np.minimum(x0 + 1, 255), np.minimum(y0 + 1, 255)
    fx, fy = x - x0, y - y0
    return (img[:, y0, x0] * (1 - fx) + img[:, y0, x1] * fx) * (1 - fy) + (
        img[:, y1, x0] * (1 - fx) + img[:, y1, x1] * fx
    ) * fy


def psnr(reference: NDArray[np.float32], actual: NDArray[np.floating]) -> float:
    mse = float(np.mean((reference - actual) ** 2))
    return float(-10 * np.log10(max(mse, 1e-12)))


def probe_required_pad(path: Path) -> int:
    options = ort.SessionOptions()
    options.intra_op_num_threads = 1
    options.inter_op_num_threads = 1
    options.add_free_dimension_override_by_name("batch", 1)
    options.add_free_dimension_override_by_name("height", 192)
    options.add_free_dimension_override_by_name("width", 320)
    session = ort.InferenceSession(str(path), sess_options=options, providers=["CPUExecutionProvider"])
    by_name = {o.name: o.shape for o in session.get_outputs()}
    grid = by_name.get("pushflow_grid0")
    if grid == [1, 256, 384, 2]:
        return 128
    if grid == [1, 192, 320, 2]:
        return 64
    raise ModelBuildError(f"Unexpected grid0 shape at 192x320 probe: {grid}")


def check_model(path: Path, align: int) -> tuple[bool, int]:
    required_pad = probe_required_pad(path)
    options = ort.SessionOptions()
    options.intra_op_num_threads = 2
    options.inter_op_num_threads = 1
    session = ort.InferenceSession(str(path), sess_options=options, providers=["CPUExecutionProvider"])
    (input_info,) = session.get_inputs()
    y, x = np.mgrid[:256, :256].astype(np.float32)
    first, middle, second = (frame(x, y, left) for left in (100, 108, 116))
    planes = [first, second, np.full((1, 256, 256), 0.5, dtype=np.float32)]
    match input_info.shape[1]:
        case 7:
            pass
        case 11:
            planes.extend((np.full_like(x[None], 2 / 255), np.full_like(y[None], 2 / 255),
                           (2 * x / 255 - 1)[None], (2 * y / 255 - 1)[None]))
        case channels:
            raise ModelBuildError(f"Unsupported RIFE channel count: {channels}")
    rgb, grid0, grid1, mask = session.run(None, {input_info.name: np.concatenate(planes)[None]})
    if rgb.shape != (1, 3, 256, 256) or mask.shape != (1, 1, 256, 256):
        raise ModelBuildError("Unexpected RIFE RGB or mask shape")
    w0, w1 = grid_sample(first, grid0, align), grid_sample(second, grid1, align)
    weight = mask[0, 0]
    a = psnr(rgb[0], w0 * weight + w1 * (1 - weight))
    b = psnr(rgb[0], w0 * (1 - weight) + w1 * weight)
    interpolation = psnr(middle, rgb[0])
    blend = psnr(middle, (first + second) / 2)
    if not np.isfinite([a, b, interpolation, blend]).all() or max(a, b) < 55 or interpolation <= blend:
        raise ModelBuildError(f"RIFE CPU check failed: moving {interpolation:.2f}, blend {blend:.2f}, rebuild {max(a, b):.2f} dB")
    print(f"PASS {path.name}: moving {interpolation:.2f}, blend {blend:.2f}, rebuild {max(a, b):.2f} dB (pad={required_pad})")
    return a >= b, required_pad


def build(out: Path, archives: Path | None) -> None:
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="pushflow-models-", dir=out) as scratch:
        workspace = Path(scratch)
        pack = workspace / f"{PACK_NAME}.zip"
        with zipfile.ZipFile(pack, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("RIFE-MIT.txt", RIFE_MIT)
            archive.writestr("vs-mlrt-attribution.txt",
                             f"RIFE weights: https://github.com/hzwer/Practical-RIFE\nONNX conversions: {RELEASE}\n")
            for pin in MODELS:
                original = onnx.load(get_model(pin, workspace, archives), load_external_data=False)
                align, padding = expose(original)
                target = workspace / f"pushflow-{pin.role}.onnx"
                onnx.checker.check_model(original)
                onnx.save(original, target)
                mask_for_first, required_pad = check_model(target, align)
                archive.write(target, f"models/{target.name}")
                archive.writestr(f"models/{target.name}.txt",
                                 f"align_corners={align}\nmask_for_first={int(mask_for_first)}\npadding_mode={padding}\npad={required_pad}\n")
        digest = sha256(pack)
        checksum = pack.with_suffix(".sha256")
        checksum.write_text(f"{digest}  {pack.name}\n", encoding="ascii")
        pack.replace(out / pack.name)
        checksum.replace(out / checksum.name)
        print(f"{digest}  {pack.name}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--archives", type=Path, help="Use local copies of the two pinned archives without network access")
    args = parser.parse_args()
    try:
        build(args.out.resolve(), args.archives)
    except (ModelBuildError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
