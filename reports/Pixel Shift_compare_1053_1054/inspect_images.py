"""Read downloaded originals; cache decoded pixels for reproducible comparison."""
from pathlib import Path
import hashlib
import json
import numpy as np
from PIL import Image, ExifTags
import pillow_heif
import tifffile

ROOT = Path(__file__).resolve().parent
Image.MAX_IMAGE_PIXELS = None
pillow_heif.options.DISABLE_SECURITY_LIMITS = True


def serial(value):
    if isinstance(value, bytes):
        return {"bytes": len(value), "sha256": hashlib.sha256(value).hexdigest()}
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    if isinstance(value, dict):
        return {str(k): serial(v) for k, v in value.items()}
    if isinstance(value, (tuple, list)):
        return [serial(v) for v in value]
    return str(value)


def main():
    report = {}
    for stem in ("B0001053", "B0001054"):
        path = ROOT / (stem + ".HEIC")
        image = pillow_heif.open_heif(path, convert_hdr_to_8bit=True)
        exif = Image.Exif()
        exif.load(image.info["exif"])
        tags = dict(exif)
        tags.update(exif.get_ifd(34665))
        report[stem] = {
            "size": image.size, "mode": image.mode,
            "file_bytes": path.stat().st_size,
            "bit_depth": image.info.get("bit_depth"),
            "chroma": image.info.get("chroma"),
            "tiling": serial(image.info.get("tiling")),
            "exif": {ExifTags.TAGS.get(k, str(k)): serial(v) for k, v in tags.items()},
        }
        cache = ROOT / (stem + ".npy")
        if not cache.exists():
            pixels = np.asarray(image)
            np.save(cache, pixels)
            del pixels
        pixels = np.load(cache, mmap_mode="r")
        thumb = Image.fromarray(pixels[::8, ::8])
        thumb.thumbnail((1800, 1400), Image.Resampling.LANCZOS)
        thumb.save(ROOT / (stem + "_overview.jpg"), quality=95)
        raw = ROOT / (stem + (".3fr" if stem.endswith("53") else ".3FR"))
        with tifffile.TiffFile(raw) as tiff:
            def page_info(page):
                return {"shape": page.shape, "dtype": str(page.dtype),
                        "tags": {t.name: serial(t.value) for t in page.tags.values()
                                 if t.code in (256,257,258,259,262,273,277,278,279,33421,33422,
                                               50713,50714,50717,50721,50728,50829)},
                        "subifds": [page_info(s) for s in page.pages] if page.subifds else []}
            report[stem]["raw_ifds"] = [page_info(p) for p in tiff.pages]
        print(stem, json.dumps(report[stem], ensure_ascii=False), flush=True)
    (ROOT / "metadata.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")


if __name__ == "__main__":
    main()
