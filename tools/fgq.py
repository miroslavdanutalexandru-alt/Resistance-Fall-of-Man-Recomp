"""fgq: offline frame-generation motion-vector quality.
For consecutive frames (colour C, motion mv = previous - current in UV, depth Z), per pixel:
  warp error   |C_N(p) - C_N-1(p + mv)| vs the zero-motion error,
  block match  the displacement d* (|d| <= R px, 7x7 SSD, sub-pixel parabola) that best explains C_N from C_N-1 on
               textured, unambiguous pixels; 'within 1 px' = |d* - mv_px| <= 1.
Bad pixels are classified by source (caller-provided class map)."""
import numpy as np

def luma(c):
    return 0.2126 * c[..., 0] + 0.7152 * c[..., 1] + 0.0722 * c[..., 2]

def box(a, r):
    k = 2 * r + 1
    p = np.pad(a, r, mode="edge")
    s = np.cumsum(np.cumsum(p, 0), 1)
    s = np.pad(s, ((1, 0), (1, 0)))
    return (s[k:, k:] - s[:-k, k:] - s[k:, :-k] + s[:-k, :-k]) / (k * k)

def shift(a, dy, dx):
    """a[y+dy, x+dx] with edge clamp"""
    H, W = a.shape
    ys = np.clip(np.arange(H) + dy, 0, H - 1); xs = np.clip(np.arange(W) + dx, 0, W - 1)
    return a[ys][:, xs]

def bilinear(a, y, x):
    H, W = a.shape
    x = np.clip(x, 0, W - 1.001); y = np.clip(y, 0, H - 1.001)
    x0 = np.floor(x).astype(int); y0 = np.floor(y).astype(int); fx = x - x0; fy = y - y0
    return (a[y0, x0] * (1 - fx) * (1 - fy) + a[y0, x0 + 1] * fx * (1 - fy) + a[y0 + 1, x0] * (1 - fx) * fy + a[y0 + 1, x0 + 1] * fx * fy)

def tonemap(l):
    return l / (1.0 + l)

def analyse(c0, c1, mv1, R=None, pr=3, tex_thr=0.01, sign=1.0):
    """c0 previous colour, c1 current colour, mv1 current motion (UV, previous - current) * sign. Returns dict of maps."""
    H, W = c1.shape[:2]
    l0 = tonemap(luma(c0)); l1 = tonemap(luma(c1))
    mx = sign * mv1[..., 0] * W; my = sign * mv1[..., 1] * H
    if R is None: R = int(min(24, np.ceil(max(np.percentile(np.abs(mx), 99.5), np.percentile(np.abs(my), 99.5))) + 4))
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float64)
    wl = bilinear(l0, yy + my, xx + mx)
    e_mv = np.abs(l1 - wl); e_0 = np.abs(l1 - l0)
    # block matching: pass 1 best, pass 2 second best (outside the best's 3x3) and the best's 4 neighbours
    best = np.full((H, W), np.inf, np.float32); bdy = np.zeros((H, W), np.int32); bdx = np.zeros((H, W), np.int32)
    l0 = l0.astype(np.float32); l1 = l1.astype(np.float32)
    def ssd(dy, dx):
        return box((l1 - shift(l0, dy, dx)) ** 2, pr).astype(np.float32)
    for dy in range(-R, R + 1):
        for dx in range(-R, R + 1):
            s = ssd(dy, dx); m = s < best
            best[m] = s[m]; bdy[m] = dy; bdx[m] = dx
    second = np.full((H, W), np.inf, np.float32)
    nb = {k: np.full((H, W), np.inf, np.float32) for k in ((0, -1), (0, 1), (-1, 0), (1, 0))}
    for dy in range(-R, R + 1):
        for dx in range(-R, R + 1):
            s = ssd(dy, dx)
            ddy = dy - bdy; ddx = dx - bdx
            far = (np.abs(ddy) > 1) | (np.abs(ddx) > 1)
            m = far & (s < second); second[m] = s[m]
            for k, a in nb.items():
                m = (ddy == k[0]) & (ddx == k[1]); a[m] = s[m]
    cm = best; xm = nb[(0, -1)]; xp = nb[(0, 1)]; ym = nb[(-1, 0)]; yp = nb[(1, 0)]
    with np.errstate(all="ignore"):
        dnx = xm - 2 * cm + xp; dny = ym - 2 * cm + yp
        sx = np.where(np.isfinite(dnx) & (dnx > 0), 0.5 * (xm - xp) / dnx, 0.0)
        sy = np.where(np.isfinite(dny) & (dny > 0), 0.5 * (ym - yp) / dny, 0.0)
    ex = bdx + np.clip(sx, -0.5, 0.5); ey = bdy + np.clip(sy, -0.5, 0.5)
    var = box(l1 ** 2, pr) - box(l1, pr) ** 2
    textured = var > tex_thr ** 2
    unamb = (second > 2.0 * best + 1e-7) & (np.abs(bdx) < R) & (np.abs(bdy) < R)
    good_match = best < 0.25 * var + 1e-6
    valid = textured & unamb & good_match
    # the inputs are jittered: a frame pair carries one global sub-pixel offset (jitter N - jitter N-1) that is not MV error
    jx = float(np.median((ex - mx)[valid])) if valid.any() else 0.0; jy = float(np.median((ey - my)[valid])) if valid.any() else 0.0
    if abs(jx) > 1.0 or abs(jy) > 1.0: jx = jy = 0.0
    err = np.hypot(ex - mx - jx, ey - my - jy)
    return dict(e_mv=e_mv, e_0=e_0, err=err, valid=valid, ex=ex, ey=ey, mx=mx, my=my, jx=jx, jy=jy, R=R)

def report(r, classes=None, names=None, px=1.0):
    v = r["valid"]; ok = v & (r["err"] <= px)
    out = {"valid_px": int(v.sum()), "within": float(ok.sum() / max(1, v.sum())),
           "warp_err_mv": float(r["e_mv"].mean()), "warp_err_zero": float(r["e_0"].mean()), "jit": (round(r["jx"], 2), round(r["jy"], 2)), "R": r["R"]}
    if classes is not None:
        bad = v & ~ok
        tb = max(1, bad.sum())
        rows = []
        for k in np.unique(classes):
            m = classes == k
            rows.append((names.get(int(k), str(k)) if names else str(k), int((v & m).sum()),
                         float((ok & m).sum() / max(1, (v & m).sum())), float((bad & m).sum() / tb)))
        out["classes"] = rows
    return out
