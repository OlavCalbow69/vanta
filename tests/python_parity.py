"""Compare pure reference methods with Vanta's native Python mode.

Only three AST-selected methods are executed. The supplied module's imports,
constructor, desktop capture, serial connection and main loop never run.
Requires test-only numpy/opencv-python-headless; not shipped with Vanta.
"""
import argparse
import ast
import copy
import json
from pathlib import Path
import random
import subprocess

import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[1]


def reference_class():
    tree = ast.parse((ROOT / "tests/reference/colorbot.py").read_text(encoding="utf-8"))
    original = next(node for node in tree.body if isinstance(node, ast.ClassDef)
                    and node.name == "MakcuColorBot")
    allowed = {"update_settings", "get_color_mask", "find_target"}
    methods = [node for node in original.body if isinstance(node, ast.FunctionDef)
               and node.name in allowed]
    assert {node.name for node in methods} == allowed
    sandbox = ast.Module(body=[ast.ClassDef(name="Reference", bases=[], keywords=[],
                         body=methods, decorator_list=[])], type_ignores=[])
    namespace = {"np": np, "cv2": cv2}
    exec(compile(ast.fix_missing_locations(sandbox), "<pure reference methods>", "exec"), namespace)
    return namespace["Reference"]


def color(hue, saturation=255, value=255):
    return cv2.cvtColor(np.uint8([[[hue, saturation, value]]]), cv2.COLOR_HSV2BGR)[0, 0].tolist()


def rectangle(x, y, w, h, bgr=None):
    return dict(x=x, y=y, w=w, h=h, bgr=color(150) if bgr is None else bgr)


def fixtures():
    base = json.loads((ROOT / "assets/python-config.json").read_text(encoding="utf-8"))
    cases = []

    def add(name, shapes, width=320, height=240, **settings):
        config = copy.deepcopy(base)
        config.update(settings)
        cases.append(dict(name=name, width=width, height=height, config=config, shapes=shapes))

    add("empty", [])
    add("supplied-tall", [rectangle(150, 92, 6, 20)])
    add("truncation-negative", [rectangle(150, 92, 6, 20)], smooth_factor=0.4030150753768844)
    add("zero-smoothing", [rectangle(150, 92, 6, 20)], smooth_factor=0.0)
    add("negative-smoothing", [rectangle(150, 92, 6, 20)], smooth_factor=-0.4030150753768844)
    add("square-orb", [rectangle(145, 105, 30, 30)])
    add("orb-flag-unused", [rectangle(145, 105, 30, 30)], filter_orbs=False)
    add("flat", [rectangle(125, 109, 60, 3)])
    add("too-large-area", [rectangle(135, 85, 45, 65)])
    add("minimum-area", [rectangle(150, 92, 6, 20)], min_contour_area=1000)
    add("width-rejection", [rectangle(90, 103, 140, 12)], fov_radius=100)
    add("height-rejection", [rectangle(157, 8, 3, 210)], fov_radius=115)
    add("merge-no-y-test", [rectangle(147, 72, 3, 10), rectangle(157, 144, 3, 10)])
    add("greedy-horizontal", [rectangle(115, 100, 3, 12), rectangle(141, 121, 3, 12),
                             rectangle(127, 76, 3, 12), rectangle(160, 107, 3, 12)])
    add("morphology-gaps", [rectangle(148, 90, 3, 8), rectangle(152, 100, 3, 20),
                           rectangle(148, 121, 3, 9)])
    add("post-offset-outside-fov", [rectangle(150, 92, 6, 20)], offset_x=250, offset_y=-140)
    add("negative-mean-truncation", [rectangle(150, 92, 6, 20)], offset_x=-100)
    add("ignored-screen-metadata", [rectangle(150, 92, 6, 20)], screen_width=1, screen_height=1)
    add("native-1440p", [rectangle(1270, 680, 8, 40)], width=2560, height=1440)
    add("native-1080p", [rectangle(950, 500, 8, 40)], width=1920, height=1080)
    add("odd-source-center", [rectangle(150, 92, 6, 20)], width=321, height=241)
    add("irregular-top-mask", [dict(points=[[140, 90], [147, 87], [153, 92],
                                              [158, 126], [140, 129]], bgr=color(150))])
    for hue in [0, 1, 9, 10, 11, 169, 170, 179]:
        add(f"red-hue-{hue}", [rectangle(150, 92, 6, 20, color(hue))], active_preset="red")
    for hue in [24, 25, 30, 35, 36, 130, 165, 166]:
        add(f"yellow-hue-{hue}", [rectangle(150, 92, 6, 20, color(hue))], active_preset="yellow")
    # Exact circle boundary and morphology before the circular crop.
    for dx, dy in [(0, -60), (36, -48), (60, 0), (-36, 48), (37, -48)]:
        add(f"circle-{dx}-{dy}", [rectangle(160 + dx, 120 + dy, 2, 5)], min_contour_area=0)
    add("near-tied-targets", [rectangle(130, 100, 3, 12), rectangle(187, 100, 3, 12)], offset_x=0)
    rng = random.Random(20261015)
    for index in range(160):
        preset = rng.choice(["purple", "red", "yellow"])
        hue = dict(purple=150, red=rng.choice([0, 179]), yellow=30)[preset]
        shapes = [rectangle(rng.randrange(90, 220), rng.randrange(50, 175),
                            rng.randrange(1, 40), rng.randrange(1, 55),
                            color(hue if rng.random() < 0.8 else 80))
                  for _ in range(rng.randrange(1, 9))]
        add(f"seeded-{index:03}", shapes, active_preset=preset,
            smooth_factor=rng.choice([0.4030150753768844, 0.45, 1.0, 0.13]),
            offset_x=rng.randrange(-4, 5), offset_y=rng.randrange(-5, 12),
            min_contour_area=rng.choice([0, 1, 4, 15]), filter_orbs=bool(index % 2))
    return cases


def runs(mask):
    flat = mask.ravel() != 0
    edges = np.diff(np.r_[False, flat, False].astype(np.int8))
    starts, ends = np.flatnonzero(edges == 1), np.flatnonzero(edges == -1)
    return [[int(a), int(b-a)] for a, b in zip(starts, ends)]


def evaluate(fixture, reference):
    image = np.zeros((fixture["height"], fixture["width"], 4), dtype=np.uint8)
    for shape in fixture["shapes"]:
        paint = tuple(shape["bgr"]) + (255,)
        if "points" in shape:
            cv2.fillPoly(image, [np.array(shape["points"], dtype=np.int32)], paint)
        else:
            x, y, w, h = (shape[k] for k in ["x", "y", "w", "h"])
            cv2.rectangle(image, (x, y), (x+w-1, y+h-1), paint, cv2.FILLED)
    bot = reference()
    bot.center_x, bot.center_y = fixture["width"] // 2, fixture["height"] // 2
    bot.kernel = np.ones((3, 3), np.uint8)
    bot.update_settings(fixture["config"])
    r = bot.grab_region
    crop = image[r["top"]:r["top"]+r["height"], r["left"]:r["left"]+r["width"]]
    hsv = cv2.cvtColor(cv2.cvtColor(crop, cv2.COLOR_BGRA2BGR), cv2.COLOR_BGR2HSV)
    mask = bot.get_color_mask(hsv)
    target, contours = bot.find_target(mask)
    yy, xx = np.ogrid[:mask.shape[0], :mask.shape[1]]
    fov = np.where(np.hypot(xx-bot.local_cx, yy-bot.local_cy) <= bot.fov_radius, mask, 0)
    movement = [int((target[0]-bot.local_cx)*bot.smooth),
                int((target[1]-bot.local_cy)*bot.smooth)] if target is not None else [0, 0]
    return dict(name=fixture["name"], target=list(target) if target is not None else None,
                movement=movement, color_runs=runs(mask), fov_runs=runs(fov),
                contours=[list(cv2.boundingRect(c)) + [cv2.contourArea(c)] for c in contours])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/python-parity")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    input_path = (args.output_dir / "fixtures.json").resolve()
    output_path = (args.output_dir / "native.json").resolve()
    cases = fixtures()
    input_path.write_text(json.dumps(dict(cases=cases)), encoding="utf-8")
    subprocess.run([str(args.exe.resolve()), "--python-parity-fixtures",
                    str(input_path), str(output_path)], check=True)
    actual = json.loads(output_path.read_text(encoding="utf-8"))
    reference = reference_class()
    assert len(actual) == len(cases), "Fixture count mismatch"
    for fixture, native in zip(cases, actual):
        expected = evaluate(fixture, reference)
        for key in expected:
            assert native[key] == expected[key], f"{fixture['name']}: {key} differs"
    print(f"PASS: {len(cases)} fixtures; exact masks, contour measurements/order, aim points and integer moves.")
    print(f"Reference OpenCV {cv2.__version__}; native bundled OpenCV 5.0. No hardware or desktop input used.")


if __name__ == "__main__":
    main()
