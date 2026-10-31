"""Generate HD building art from your own game files. Output is never committed.

Needs a separate environment with torch, spandrel, numpy and pillow, plus a Real-ESRGAN
x4 anime model. For each building image it writes, for the game's overlay:
  <name>_<dir>.png     the upscaled sprite
  <name>_<dir>_lo.png  the original sprite, which the overlay compares against

Usage: hd_buildings.py <Data/Sprites folder> <output folder> <model.pth> [--scale N] [--detail D] [name ...]
"""
import argparse
import struct
from pathlib import Path

import numpy as np
import torch
from PIL import Image, ImageFilter
from spandrel import ModelLoader


class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def take(self, layout):
        value = struct.unpack_from(layout, self.data, self.at)[0]
        self.at += struct.calcsize(layout)
        return value

    def text(self, size):
        value = self.data[self.at:self.at + size]
        self.at += size
        return value.split(b"\0")[0].decode("latin1")


def parse_sprites(path):
    r = Reader(path.read_bytes())
    r.at = 20
    r.take("<i")
    sprites = []
    for _ in range(r.take("<i")):
        sprite = {"name": r.text(r.take("<B")), "type": r.take("<i"), "palettes": [], "anims": []}
        if sprite["type"] == 5:
            r.at += 4
        for _ in range(r.take("<i")):
            if sprite["type"] in (1, 2, 3, 5, 6, 11):
                sprite["palettes"].append(r.data[r.at:r.at + 768])
                r.at += 768
        for _ in range(r.take("<i")):
            anim = {"name": r.text(r.take("<B")), "frames": r.take("<H"), "dirs": r.take("<B"),
                    "w": r.take("<H"), "h": r.take("<H")}
            r.at += 4
            anim["offset"] = r.take("<i")
            anim["size"] = r.take("<i")
            anim["unique"] = r.take("<H")
            sprite["anims"].append(anim)
        sprites.append(sprite)
    return sprites


def decode(data_file, sprite, anim, direction, frame=0):
    data_file.seek(anim["offset"])
    count = anim["frames"] * anim["dirs"]
    index = struct.unpack(f"<{count}H", data_file.read(count * 2))
    offsets = struct.unpack(f"<{anim['unique']}I", data_file.read(anim["unique"] * 4))
    body = data_file.read(anim["size"])
    at = offsets[index[anim["frames"] * direction + frame]]
    w, h = anim["w"], anim["h"]
    palette = np.frombuffer(sprite["palettes"][0], dtype=np.uint8).reshape(-1, 3)
    image = np.zeros((h, w, 4), np.uint8)
    for y in range(h):
        x = 0
        while x < w:
            code = body[at]
            at += 1
            if code >= 128:
                x += 256 - code
                continue
            for _ in range(code):
                if x < w:
                    image[y, x, :3] = palette[body[at]]
                    image[y, x, 3] = 255
                at += 1
                x += 1
    return image


def bleed(image):
    """Extend colours into transparency, so the model does not ring against black."""
    rgb = image[..., :3].astype(np.float32)
    known = (image[..., 3] > 0).astype(np.float32)
    for _ in range(12):
        total = np.zeros_like(rgb)
        weight = np.zeros_like(known)
        for shift in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            total += np.roll(rgb * known[..., None], shift, (0, 1))
            weight += np.roll(known, shift, (0, 1))
        grow = (known == 0) & (weight > 0)
        rgb[grow] = total[grow] / weight[grow][..., None]
        known = np.maximum(known, grow.astype(np.float32))
    return rgb.astype(np.uint8)


@torch.no_grad()
def upscale(model, device, image, scale, detail):
    tensor = torch.from_numpy(bleed(image).astype(np.float32) / 255).permute(2, 0, 1)[None].to(device)
    rgb = (model(tensor).clamp(0, 1)[0].permute(1, 2, 0).cpu().numpy() * 255 + 0.5).astype(np.uint8)
    h, w = image.shape[:2]
    alpha = np.array(Image.fromarray(image[..., 3]).resize((w * 4, h * 4), Image.BICUBIC)).astype(np.float32)
    alpha = np.clip((alpha - 96) * 255 / 64, 0, 255).astype(np.uint8)
    # The model smooths fine texture such as cobbles and plaster away. Put the original's
    # back: its high frequencies, over the model's clean shapes.
    if detail > 0:
        size = (w * 4, h * 4)
        source = np.array(Image.fromarray(bleed(image)).resize(size, Image.LANCZOS)).astype(np.float32)
        blurred = np.array(Image.fromarray(source.astype(np.uint8)).filter(ImageFilter.GaussianBlur(5))).astype(np.float32)
        model_blurred = np.array(Image.fromarray(rgb).filter(ImageFilter.GaussianBlur(5))).astype(np.float32)
        restored = rgb.astype(np.float32) * (1 - detail) + (model_blurred + source - blurred) * detail
        rgb = np.clip(restored, 0, 255).astype(np.uint8)
    result = Image.fromarray(np.dstack([rgb, alpha]), "RGBA")
    return result if scale == 4 else result.resize((w * scale, h * scale), Image.LANCZOS)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("sprites", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("model")
    parser.add_argument("--scale", type=int, default=4, choices=(2, 3, 4))
    parser.add_argument("--detail", type=float, default=0.5, help="original texture to keep, 0 to 1")
    parser.add_argument("--interface", action="store_true", help="also interface sprites, animated overlays and screens")
    parser.add_argument("names", nargs="*")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    device = "mps" if torch.backends.mps.is_available() else "cpu"
    state = torch.load(args.model, map_location="cpu", weights_only=True)
    model = ModelLoader().load_from_state_dict(state).to(device).eval()
    written = 0
    with (args.sprites / "BLSprite.dat").open("rb") as data_file:
        for sprite in parse_sprites(args.sprites / "BLSprite.hdr"):
            if sprite["type"] != 5 or (args.names and sprite["name"] not in args.names):
                continue
            if not args.names and sprite["name"].endswith("_construct"):
                continue
            anim = sprite["anims"][0]
            if anim["name"].lower() != "image":
                continue
            for direction in range(anim["dirs"]):
                image = decode(data_file, sprite, anim, direction)
                Image.fromarray(image, "RGBA").save(args.output / f"{sprite['name']}_{direction}_lo.png")
                upscale(model, device, image, args.scale, args.detail).save(args.output / f"{sprite['name']}_{direction}.png")
                written += 1
        for sprite in parse_sprites(args.sprites / "BLSprite.hdr") if args.interface else []:
            # Interface images and animated overlays. Lightmaps are data, not pictures.
            is_overlay = sprite["name"].endswith("_anim")
            if (sprite["type"] != 1 and not is_overlay) or "light" in sprite["name"] or sprite["name"].endswith("map"):
                continue
            for number, anim in enumerate(sprite["anims"]):
                if anim["name"].lower() in ("depth", "pick", "shadow", "lightmap"):
                    continue
                for direction in range(anim["dirs"]):
                    for frame in range(anim["frames"]):
                        image = decode(data_file, sprite, anim, direction, frame)
                        if min(image.shape[:2]) < 2 or image[..., 3].max() == 0:
                            continue
                        key = f"{sprite['name']}_{number}_{direction}_{frame}"
                        Image.fromarray(image, "RGBA").save(args.output / f"{key}_lo.png")
                        upscale(model, device, image, 2 if is_overlay else args.scale, args.detail).save(args.output / f"{key}.png")
                        written += 1
    for path in sorted((args.sprites.parent / "Screens").glob("*.tga")) if args.interface else []:
        image = np.array(Image.open(path).convert("RGBA"))
        if (image.shape[1], image.shape[0]) == (800, 600):
            key = f"screen_{path.stem.lower()}"
            Image.fromarray(image, "RGBA").save(args.output / f"{key}_lo.png")
            upscale(model, device, image, args.scale, args.detail).save(args.output / f"{key}.png")
            written += 1
    print(f"{written} images written to {args.output}")


if __name__ == "__main__":
    main()
