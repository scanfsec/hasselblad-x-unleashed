"""Compare RAW green detail with the same linear interpolation and tone curve."""
from pathlib import Path
import json
import cv2
import numpy as np
import rawpy
import tifffile
from PIL import Image
from compare_pixels import ROOT, ROIS, register, board


class Raw:
    def __init__(self, path):
        with rawpy.imread(str(path)) as raw:
            self.pattern = raw.raw_pattern.copy()
            self.black = list(raw.black_level_per_channel)
            self.white = raw.white_level
            self.color = raw.color_desc.decode().rstrip("\x00")
        with tifffile.TiffFile(path) as tiff:
            page = tiff.pages[0].pages[0]
            assert int(page.compression) == 1 and len(page.dataoffsets) == 1
            assert page.databytecounts[0] == np.prod(page.shape)*2
            self.data = np.memmap(path, dtype="<u2", mode="r", offset=page.dataoffsets[0],shape=page.shape)
        self.greens = [(y,x,int(self.pattern[y,x])) for y in range(2) for x in range(2)
                       if self.color[self.pattern[y,x]] == "G"]
        assert len(self.greens) == 2

    @staticmethod
    def tone(a):
        # Identical green-channel display curve; no WB/color matrix/sharpening/NR.
        return np.uint8(np.clip(np.maximum(a,0)**(1/2.2)*255,0,255))

    def overview(self):
        h,w = self.data.shape
        half = np.zeros((h//2,w//2),np.float32)
        for y,x,c in self.greens:
            half += (self.data[y:h//2*2:2,x:w//2*2:2].astype(np.float32)-self.black[c])*.5
        small = cv2.resize(half, (1800,round(h/w*1800)),interpolation=cv2.INTER_AREA)
        return self.tone(small/60619)

    def crop(self,x,y,w,h):
        assert x>=2 and y>=2 and x+w+2<=self.data.shape[1] and y+h+2<=self.data.shape[0]
        x0,y0=x-2,y-2
        data=self.data[y0:y+h+2,x0:x+w+2]
        green=np.zeros(data.shape,np.float32)
        for py,px,c in self.greens:
            yy,xx=(py-y0)%2,(px-x0)%2
            green[yy::2,xx::2]=data[yy::2,xx::2].astype(np.float32)-self.black[c]
        # Keep sampled green values; interpolate only the R/B sites from four
        # neighboring greens. Both images receive exactly this same operation.
        kernel=np.array([[0,.25,0],[.25,1,.25],[0,.25,0]],np.float32)
        linear=cv2.filter2D(green,-1,kernel)[2:-2,2:-2]/60619
        return linear


def remap(raw,inv,x,y,w,h,delta=(0,0)):
    yy,xx=np.mgrid[y:y+h,x:x+w].astype(np.float32)
    xx+=delta[0]; yy+=delta[1]
    sx=(inv[0,0]*xx+inv[0,1]*yy+inv[0,2]).astype(np.float32)
    sy=(inv[1,0]*xx+inv[1,1]*yy+inv[1,2]).astype(np.float32)
    x0,y0=int(np.floor(sx.min()))-4,int(np.floor(sy.min()))-4
    x1,y1=int(np.ceil(sx.max()))+5,int(np.ceil(sy.max()))+5
    source=raw.crop(x0,y0,x1-x0,y1-y0)
    return cv2.remap(source,sx-x0,sy-y0,cv2.INTER_CUBIC)


def main():
    ps=Raw(ROOT/"B0001053.3fr")
    single=Raw(ROOT/"B0001054.3FR")
    a,b=single.overview(),ps.overview()
    Image.fromarray(a).save(ROOT/"raw_single_overview.jpg",quality=95)
    Image.fromarray(b).save(ROOT/"raw_ps_overview.jpg",quality=95)
    m,report=register(a,b)
    sa,sb=1800/single.data.shape[1],1800/ps.data.shape[1]
    m[:,:2]*=sa/sb; m[:,2]/=sb
    report["median_inlier_error_overview_pixels"] = report.pop("median_inlier_error_ps_pixels")
    report["median_inlier_error_ps_pixels"] = report["median_inlier_error_overview_pixels"] / sb
    report["single_to_ps_raw"]=m.tolist()
    report["method"]="Green samples, per-channel LibRaw black subtraction; same bilinear green reconstruction, fixed denominator60619 and gamma1/2.2. No color matrix/WB/NR/sharpening. Single remapped once with bicubic. Not a full standardized RAW development."
    report["raw_dimensions"]={"single":list(single.data.shape[::-1]),"ps":list(ps.data.shape[::-1])}
    report["regions"]=[]
    inv=cv2.invertAffineTransform(m)
    for label,fx,fy in ROIS:
        w,h=768,512
        x,y=round(ps.data.shape[1]*fx)-w//2,round(ps.data.shape[0]*fy)-h//2
        target=ps.crop(x,y,w,h)
        candidate=remap(single,inv,x,y,w,h)
        ecc,warp=cv2.findTransformECC(target,candidate,np.eye(2,3,dtype=np.float32),
                 cv2.MOTION_TRANSLATION,(cv2.TERM_CRITERIA_COUNT|cv2.TERM_CRITERIA_EPS,100,1e-6),None,5)
        delta=warp[:,2].tolist()
        assert max(abs(v) for v in delta)<40,delta
        aligned=remap(single,inv,x,y,w,h,delta)
        before=np.corrcoef(target[32:-32,32:-32].ravel(),candidate[32:-32,32:-32].ravel())[0,1]
        after=np.corrcoef(target[32:-32,32:-32].ravel(),aligned[32:-32,32:-32].ravel())[0,1]
        assert after>=before-.005
        aa,bb=Raw.tone(aligned),Raw.tone(target)
        board(aa,bb,"RAW green | "+label+" | common-grid context").save(ROOT/(label+"_raw_context.png"))
        for z in (4,8):
            board(aa[192:320,288:480],bb[192:320,288:480],
                  "RAW green | "+label+f" | {z*100}% common PS grid",z).save(ROOT/(label+f"_raw_{z*100}pct.png"))
        # Native-scale, linear crops are saved for reproducibility, not just pictures.
        np.savez_compressed(ROOT/(label+"_raw_aligned.npz"),single=aligned,ps=target)
        report["regions"].append({"name":label,"ps_raw_xywh":[x,y,w,h],
            "local_translation_ps_pixels":delta,"ecc":ecc,"correlation":float(after)})
    (ROOT/"raw_registration.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
    print(json.dumps(report,indent=2))


if __name__ == "__main__":
    main()
