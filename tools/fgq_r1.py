"""Offline motion-vector quality of an R1 RSX_MV_DUMP burst.

usage: python tools/fgq_r1.py <burst dir> [--png out_prefix] [--sign 1|-1|auto]

For each consecutive frame pair (N-1, N) it measures how well frame N's motion vectors
(UV, previous - current) predict where each pixel was in frame N-1: block matching gives the
true displacement, the error is |true - MV| in render pixels (the per-pair jitter offset removed).
Pixels are classed by the motion pass's own record of their source (fNN_zpre.raw) and, for
matched draws, by whether the MV is the camera's, zero (view-attached) or the draw's own:
  0 no draw (camera fill)  1 no match (camera fill)  2 ambiguous  3 matched = camera
  4 matched, own motion    5 matched, zero motion (view-still / view-locked)
"""
import json, os, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fgq  # noqa: E402

NAMES = {0: "no draw->camera", 1: "no match->camera", 2: "ambiguous", 3: "matched=camera", 4: "matched own", 5: "matched zero"}
DT = {2: np.float32, 10: np.float16, 16: np.float32, 34: np.float16, 41: np.float32, 28: np.uint8, 87: np.uint8, 24: np.uint32}
CH = {2: 4, 10: 4, 16: 2, 34: 2, 41: 1, 28: 4, 87: 4, 24: 1}


def load_raw(path):
    with open(path, "rb") as f:
        w, h, pitch, dxgi = np.frombuffer(f.read(16), np.uint32)
        data = np.frombuffer(f.read(), np.uint8)
    dt = DT.get(int(dxgi))
    if dt is None:
        raise ValueError("%s: dxgi %d not handled" % (path, dxgi))
    ch = CH[int(dxgi)]
    rowb = int(w) * ch * np.dtype(dt).itemsize
    rows = data[: int(pitch) * int(h)].reshape(int(h), int(pitch))[:, :rowb]
    a = np.ascontiguousarray(rows).view(dt).reshape(int(h), int(w), ch)
    if dt == np.uint8:
        a = a.astype(np.float32) / 255.0
        if int(dxgi) == 87:
            a = a[..., [2, 1, 0, 3]]
    return a.astype(np.float32)


def camera_mv(meta, d):
    """the camera fill's motion for depth d (rh x rw), as the fill CS computes it"""
    rh, rw = d.shape
    if meta.get("M") is None:
        return np.zeros((rh, rw, 2), np.float32)
    M = np.array(meta["M"], np.float64).reshape(4, 4)
    jx, jy = meta.get("fill_jit_ndc", [0.0, 0.0])
    yy, xx = np.mgrid[0:rh, 0:rw].astype(np.float64)
    nx = (xx + 0.5) / rw * 2.0 - 1.0 - jx
    ny = 1.0 - (yy + 0.5) / rh * 2.0 - jy
    c = np.stack([nx, ny, d.astype(np.float64), np.ones_like(nx)], -1)
    p = c @ M.T
    with np.errstate(all="ignore"):
        ok = p[..., 3] > 1e-6
        mx = np.where(ok, (p[..., 0] / p[..., 3] - nx) * 0.5, 0.0)
        my = np.where(ok, (p[..., 1] / p[..., 3] - ny) * -0.5, 0.0)
    return np.stack([mx, my], -1).astype(np.float32)


def measure(c0, c1, mv, sign):
    big = max(np.percentile(np.abs(mv[..., 0]) * mv.shape[1], 99.5), np.percentile(np.abs(mv[..., 1]) * mv.shape[0], 99.5)) > 16
    return (fgq.analyse_ctf if big else fgq.analyse)(c0, c1, mv, sign=sign)


def classes(meta, zpre, z, mv):
    rh, rw = z.shape
    cl = np.full((rh, rw), 3, np.int32)
    cl[zpre == 4.0] = 0
    cl[(zpre <= -0.5) & (zpre > -2.5)] = 1
    cl[(zpre <= -2.5) & (zpre > -4.5)] = 2
    m = (zpre >= 0.0) & (zpre <= 1.0)
    cam = camera_mv(meta, z)
    dpx = np.hypot((mv[..., 0] - cam[..., 0]) * rw, (mv[..., 1] - cam[..., 1]) * rh)
    zero = np.hypot(mv[..., 0] * rw, mv[..., 1] * rh) < 0.05
    camz = np.hypot(cam[..., 0] * rw, cam[..., 1] * rh) < 0.05
    cl[m & (dpx > 0.5)] = 4
    cl[m & zero & ~camz] = 5
    return cl


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    d = args[0]
    png = args[args.index("--png") + 1] if "--png" in args else None
    sign = args[args.index("--sign") + 1] if "--sign" in args else "auto"
    metas = {}
    with open(os.path.join(d, "frames.jsonl")) as f:
        for line in f:
            if line.strip():
                j = json.loads(line)
                metas[j["f"]] = j
    idx = sorted(metas)
    tot_v = tot_ok = 0
    agg = {}
    sg = None if sign == "auto" else float(sign)
    for a, b in zip(idx, idx[1:]):
        ma, mb = metas[a], metas[b]
        if mb["frame"] != ma["frame"] + 1:
            print("f%02d->f%02d: not consecutive frames (%d -> %d), skipped" % (a, b, ma["frame"], mb["frame"]))
            continue
        rw, rh = mb["rw"], mb["rh"]
        ld = lambda i, n: load_raw(os.path.join(d, "f%02d_%s.raw" % (i, n)))[:rh, :rw]
        c0, c1 = ld(a, "col")[..., :3], ld(b, "col")[..., :3]
        mv = ld(b, "mv")
        z, zpre = ld(b, "z")[..., 0], ld(b, "zpre")[..., 0]
        if sg is None:   # the first pair decides the sign convention
            best = None
            for s in (1.0, -1.0):
                r = measure(c0, c1, mv, s)
                w = fgq.report(r)["within"]
                if best is None or w > best[0]:
                    best = (w, s, r)
            sg, r = best[1], best[2]
            print("sign %+d (within-1px %.1f%% vs %.1f%% the other way)" % (sg, 100 * best[0], 100 * fgq.report(measure(c0, c1, mv, -sg))["within"]))
        else:
            r = measure(c0, c1, mv, sg)
        cl = classes(mb, zpre, z, mv)
        o = fgq.report(r, cl, NAMES)
        v = r["valid"]; ok = v & (r["err"] <= 1.0)
        tot_v += int(v.sum()); tot_ok += int(ok.sum())
        for k in range(6):
            m = cl == k
            e = agg.setdefault(k, [0, 0, 0, 0])
            e[0] += int(m.sum()); e[1] += int((v & m).sum()); e[2] += int((ok & m).sum()); e[3] += int((v & ~ok & m).sum())
        print("f%02d->f%02d frame %d: within-1px %.1f%% of %d px, jitter %s (dump %s px), cam %s, R %d" % (
            a, b, mb["frame"], 100 * o["within"], o["valid_px"], o["jit"],
            [round(mb["jit_px"][i] - ma["jit_px"][i], 2) for i in (0, 1)], "M" if mb.get("M") else "none", o["R"]))
        for n, nv, w, sb in o["classes"]:
            print("    %-18s valid %7d  within %5.1f%%  share of bad %5.1f%%" % (n, nv, 100 * w, 100 * sb))
        if png:
            try:
                import cv2
                bad = (v & ~ok).astype(np.uint8) * 255
                img = np.clip(fgq.tonemap(c1) * 255, 0, 255).astype(np.uint8)[..., ::-1].copy()
                img[bad > 0] = (0, 0, 255)
                cv2.imwrite("%s_f%02d_bad.png" % (png, b), img)
                pal = np.array([[128, 128, 128], [0, 0, 255], [255, 0, 255], [0, 200, 0], [0, 255, 255], [255, 255, 255]], np.uint8)
                cv2.imwrite("%s_f%02d_class.png" % (png, b), pal[cl])
            except ImportError:
                pass
    if not tot_v:
        print("no usable frame pairs")
        return 1
    tb = max(1, sum(e[3] for e in agg.values()))
    print("TOTAL within-1px %.2f%% of %d valid px" % (100.0 * tot_ok / tot_v, tot_v))
    for k, e in sorted(agg.items(), key=lambda kv: -kv[1][3]):
        print("    %-18s px %8d  valid %8d  within %5.1f%%  share of bad %5.1f%%" % (NAMES[k], e[0], e[1], 100.0 * e[2] / max(1, e[1]), 100.0 * e[3] / tb))
    return 0


if __name__ == "__main__":
    sys.exit(main())
