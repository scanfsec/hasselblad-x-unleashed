"""Same-field registered comparison, without sharpening or super-resolution."""
from pathlib import Path
import json
import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
FONT = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 19)
ROIS = [("back_center", .60, .34), ("back_left", .35, .43),
        ("seat_center", .53, .79), ("back_border", .245, .55)]


def gray(a):
    return cv2.cvtColor(a, cv2.COLOR_RGB2GRAY) if a.ndim == 3 else a


def register(single, ps):
    sa = 1800 / single.shape[1]
    sb = 1800 / ps.shape[1]
    a = cv2.resize(single, (1800, round(single.shape[0] * sa)), interpolation=cv2.INTER_AREA)
    b = cv2.resize(ps, (1800, round(ps.shape[0] * sb)), interpolation=cv2.INTER_AREA)
    sift = cv2.SIFT_create(nfeatures=12000)
    ka, da = sift.detectAndCompute(gray(a), None)
    kb, db = sift.detectAndCompute(gray(b), None)
    pairs = cv2.BFMatcher().knnMatch(da, db, k=2)
    good = [m for m,n in pairs if m.distance < .65*n.distance]
    aa = np.float32([ka[m.queryIdx].pt for m in good])
    bb = np.float32([kb[m.trainIdx].pt for m in good])
    m, mask = cv2.estimateAffinePartial2D(aa, bb, method=cv2.RANSAC,
                                        ransacReprojThreshold=1.5, maxIters=10000)
    assert m is not None and mask.sum() > 30, (len(good), mask)
    errors = np.linalg.norm(aa @ m[:,:2].T + m[:,2] - bb, axis=1)
    full = m.copy()
    full[:,:2] *= sa / sb
    full[:,2] /= sb
    return full, {"matches":len(good), "inliers":int(mask.sum()),
                  "median_inlier_error_ps_pixels":float(np.median(errors[mask.ravel()!=0])/sb),
                  "single_to_ps":full.tolist()}


def mapped(single, inv, x, y, w, h, delta=(0,0), interpolation=cv2.INTER_CUBIC):
    yy, xx = np.mgrid[y:y+h,x:x+w].astype(np.float32)
    xx += delta[0]
    yy += delta[1]
    sx = inv[0,0]*xx + inv[0,1]*yy + inv[0,2]
    sy = inv[1,0]*xx + inv[1,1]*yy + inv[1,2]
    # OpenCV remap's full-source dimension limit is 32767; both inputs qualify.
    return cv2.remap(single, sx.astype(np.float32), sy.astype(np.float32), interpolation,
                     borderMode=cv2.BORDER_REFLECT_101)


def board(a, b, title, scale=1):
    assert a.shape == b.shape
    left = Image.fromarray(a).convert("RGB")
    right = Image.fromarray(b).convert("RGB")
    if scale != 1:
        size = (left.width*scale, left.height*scale)
        left = left.resize(size, Image.Resampling.NEAREST)
        right = right.resize(size, Image.Resampling.NEAREST)
    canvas = Image.new("RGB", (left.width*2+24,left.height+86), "#171b20")
    canvas.paste(left,(8,78))
    canvas.paste(right,(16+left.width,78))
    draw = ImageDraw.Draw(canvas)
    draw.text((8,5),title,font=FONT,fill="white")
    draw.text((8,39),"1054 single / registered, bicubic",font=FONT,fill="#f1c776")
    draw.text((16+left.width,39),"1053 Pixel Shift / native",font=FONT,fill="#86dcd1")
    return canvas


def main():
    ps = np.load(ROOT / "B0001053.npy", mmap_mode="r")
    single = np.load(ROOT / "B0001054.npy", mmap_mode="r")
    matrix, report = register(single, ps)
    inverse = cv2.invertAffineTransform(matrix)
    print(json.dumps(report),flush=True)
    report["regions"] = []
    report["zoom_definition"] = "400%/800% of the common 23310x17482 PS grid; single is mapped once with bicubic, then both use nearest-neighbor display enlargement."
    overview = Image.open(ROOT / "B0001053_overview.jpg").convert("RGB")
    marks = ImageDraw.Draw(overview)
    for label, fx, fy in ROIS:
        w,h = 768,512
        x,y = round(ps.shape[1]*fx)-w//2, round(ps.shape[0]*fy)-h//2
        actual = np.array(ps[y:y+h,x:x+w])
        candidate = mapped(single,inverse,x,y,w,h)
        warp = np.eye(2,3,dtype=np.float32)
        try:
            ecc, warp = cv2.findTransformECC(gray(actual).astype(np.float32)/255,
                gray(candidate).astype(np.float32)/255, warp, cv2.MOTION_TRANSLATION,
                (cv2.TERM_CRITERIA_COUNT|cv2.TERM_CRITERIA_EPS,100,1e-6),None,5)
            delta = warp[:,2].tolist()
            assert max(abs(v) for v in delta) < 40, delta
        except cv2.error:
            ecc, delta = None,[0,0]
        aligned = mapped(single,inverse,x,y,w,h,delta)
        # Verify translation direction from ECC instead of accepting a bad warp.
        def corr(im):
            return float(np.corrcoef(gray(im)[32:-32,32:-32].ravel(),
                                    gray(actual)[32:-32,32:-32].ravel())[0,1])
        assert corr(aligned) >= corr(candidate)-.005
        board(aligned,actual,label+" | common-grid 100% context").save(ROOT/(label+"_context.png"))
        # Same scene window at both requested magnifications, no added sharpening.
        a,b = aligned[192:320,288:480], actual[192:320,288:480]
        for zoom in (4,8):
            board(a,b,label+f" | {zoom*100}% of common PS grid",zoom).save(ROOT/(label+f"_{zoom*100}pct.png"))
        report["regions"].append({"name":label,"ps_context_xywh":[x,y,w,h],
            "local_translation_ps_pixels":delta,"ecc":ecc,"gray_correlation":corr(aligned),
            "gray_mean_single":float(gray(aligned).mean()),"gray_mean_ps":float(gray(actual).mean())})
        scale = overview.width / ps.shape[1]
        xx, yy = (x+w/2)*scale,(y+h/2)*scale
        marks.rectangle((xx-14,yy-14,xx+14,yy+14),outline="red",width=3)
        marks.text((xx+16,yy-10),label,font=FONT,fill="red")
    overview.save(ROOT/"regions_overview.jpg",quality=95)
    (ROOT/"registration.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
    print(json.dumps(report,indent=2),flush=True)


if __name__ == "__main__":
    main()
