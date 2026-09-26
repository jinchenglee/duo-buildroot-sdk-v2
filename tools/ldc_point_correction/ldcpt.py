"""Point-level versus image-level lens-distortion correction for AprilTag ROIs.

Library behind ldc_point_correction.ipynb. It provides:

- Camera: the OpenCV (k1,k2,p1,p2,k3) model from tools/ldc-calibration.json,
  with closed-form forward distortion and iterative point undistortion.
- Renderer: synthetic raw (distorted) frames of AprilTag 36h11 tags on paper
  planes, with exact ground-truth corners and poses.
- A Python port of the production crop decoder (third_party/aruco_nano), split
  so its candidate loops (contours plus quad vertices) are reusable. OpenCV's
  cv2.aruco is available as an independent cross-check.
- The compared methods, all run per ROI (mimicking CNN proposals):
    raw_nocorr   stock detect on the raw ROI, distortion ignored
    raw_corners  stock detect on the raw ROI, undistort only the 4 corners
    full_remap   undistort the whole frame (OpenCV model, the SW LDC path), then
                 stock detect on the mapped ROI
    full_remap_hw  the same through a per-pixel simulation of the SG2000 one-ratio
                 hardware LDC mesh (gdc_mesh.c), read as a pinhole image
    roi_remap    undistort only the ROI's corrected-domain box, then detect
    loop_points  stock candidate loops on the raw ROI; sample K edge points per
                 side, refine them to subpixel edge crossings, undistort the
                 samples, fit straight lines, intersect for corners, and decode
                 bits through a forward-distorted grid
    loop_points_px  the same without the subpixel step (contour pixels only)
    roi_remap_edgefit  roi_remap's corrected crop, with loop_points' corner
                 estimator run on it (isolates the correction domain)
    loop_points_rawdec  loop_points corners, but bits read by aruco_nano's raw-quad
                 homography instead of the forward-distorted grid (decode ablation)

All corner outputs are in "ideal" pixels: the undistorted image with camera
matrix K. Timings are host Python and only indicate relative cost.
"""

from __future__ import annotations

import json
import math
import time
from dataclasses import dataclass, field

import cv2
import numpy as np

TAG_CELLS = 8  # 36h11: 6x6 data bits plus a one-cell black border
DICT = cv2.aruco.getPredefinedDictionary(cv2.aruco.DICT_APRILTAG_36h11)
UNDISTORT_CRITERIA = (cv2.TERM_CRITERIA_COUNT | cv2.TERM_CRITERIA_EPS, 40, 1e-10)


# --------------------------------------------------------------------------- camera

@dataclass
class Camera:
    K: np.ndarray
    D: np.ndarray  # k1, k2, p1, p2, k3
    size: tuple  # (w, h)
    r_ideal_max: float = 0.0  # normalized ideal radius where the model folds
    r_dist_max: float = 0.0  # matching normalized distorted radius

    @classmethod
    def from_json(cls, path, dist_scale=1.0):
        with open(path) as f:
            cal = json.load(f)
        K = np.array(cal["camera_matrix"], dtype=np.float64)
        D = np.array(cal["distortion_coefficients"], dtype=np.float64).ravel()[:5].copy()
        D[[0, 1, 4]] *= dist_scale
        cam = cls(K, D, tuple(cal["image_size"]))
        cam._find_fold()
        return cam

    def _find_fold(self):
        k1, k2, _, _, k3 = self.D
        r = np.linspace(0, 3, 30001)
        f = r * (1 + k1 * r**2 + k2 * r**4 + k3 * r**6)
        falling = np.nonzero(np.diff(f) <= 0)[0]
        i = falling[0] if len(falling) else len(r) - 1
        self.r_ideal_max, self.r_dist_max = float(r[i]), float(f[i])

    @property
    def fold_radius_px(self):
        return self.r_dist_max * self.K[0, 0]

    def distort_norm(self, xy):
        """Normalized ideal coordinates -> raw pixels (closed form)."""
        xy = np.asarray(xy, dtype=np.float64)
        x, y = xy[..., 0], xy[..., 1]
        k1, k2, p1, p2, k3 = self.D
        r2 = x * x + y * y
        radial = 1 + r2 * (k1 + r2 * (k2 + r2 * k3))
        xd = x * radial + 2 * p1 * x * y + p2 * (r2 + 2 * x * x)
        yd = y * radial + p1 * (r2 + 2 * y * y) + 2 * p2 * x * y
        K = self.K
        return np.stack([K[0, 0] * xd + K[0, 1] * yd + K[0, 2], K[1, 1] * yd + K[1, 2]], -1)

    def ideal_to_norm(self, px):
        px = np.asarray(px, dtype=np.float64)
        K = self.K
        y = (px[..., 1] - K[1, 2]) / K[1, 1]
        x = (px[..., 0] - K[0, 2] - K[0, 1] * y) / K[0, 0]
        return np.stack([x, y], -1)

    def norm_to_ideal(self, xy):
        xy = np.asarray(xy, dtype=np.float64)
        K = self.K
        return np.stack([K[0, 0] * xy[..., 0] + K[0, 1] * xy[..., 1] + K[0, 2],
                         K[1, 1] * xy[..., 1] + K[1, 2]], -1)

    def distort_ideal(self, px):
        """Ideal pixels -> raw pixels."""
        return self.distort_norm(self.ideal_to_norm(px))

    def undistort_norm(self, raw_px):
        """Raw pixels -> normalized ideal coordinates (iterative inverse)."""
        pts = np.asarray(raw_px, dtype=np.float64).reshape(-1, 1, 2)
        out = cv2.undistortPointsIter(pts, self.K, self.D, np.eye(3), np.eye(3),
                                      UNDISTORT_CRITERIA)
        return out.reshape(np.shape(raw_px))

    def undistort(self, raw_px):
        """Raw pixels -> ideal pixels."""
        return self.norm_to_ideal(self.undistort_norm(raw_px))

    def raw_radius(self, raw_px):
        raw_px = np.asarray(raw_px, dtype=np.float64)
        return np.hypot(raw_px[..., 0] - self.K[0, 2], raw_px[..., 1] - self.K[1, 2])



# ------------------------------------------------------------------------ rendering

def rot(axis, deg):
    rv = np.zeros(3)
    rv["xyz".index(axis)] = math.radians(deg)
    return cv2.Rodrigues(rv)[0]


@dataclass
class Tag:
    id: int
    R: np.ndarray  # plane -> camera rotation
    t: np.ndarray  # camera position of the tag's top-left outer corner
    size: float  # black-border side length (m)
    margin_cells: float = 1.5  # white quiet zone around the black border

    def plane_to_cam(self, pts2):
        pts2 = np.asarray(pts2, dtype=np.float64)
        p3 = np.concatenate([pts2, np.zeros(pts2.shape[:-1] + (1,))], -1)
        return p3 @ self.R.T + self.t

    def corners_plane(self):
        s = self.size
        return np.array([[0, 0], [s, 0], [s, s], [0, s]], dtype=np.float64)

    def paper_plane(self):
        m = self.margin_cells * self.size / TAG_CELLS
        s = self.size
        return np.array([[-m, -m], [s + m, -m], [s + m, s + m], [-m, s + m]], dtype=np.float64)

    def center_cam(self):
        return self.plane_to_cam(np.array([self.size / 2, self.size / 2]))


def project_norm(P):
    return P[..., :2] / P[..., 2:3]


@dataclass
class GroundTruth:
    tag: Tag
    corners_ideal: np.ndarray  # 4x2, K pixels, in tag order (TL, TR, BR, BL)
    corners_raw: np.ndarray  # 4x2 raw pixels
    center_raw: np.ndarray
    side_raw: float  # approx. side length in raw pixels
    radius_raw: float  # raw radius of the tag center from the principal point


def ground_truth(cam, tag):
    cn = project_norm(tag.plane_to_cam(tag.corners_plane()))
    cc = project_norm(tag.center_cam()[None])[0]
    raw = cam.distort_norm(cn)
    center_raw = cam.distort_norm(cc)
    side = float(np.mean(np.linalg.norm(raw - np.roll(raw, -1, 0), axis=1)))
    return GroundTruth(tag, cam.norm_to_ideal(cn), raw, center_raw, side,
                       float(cam.raw_radius(center_raw)))


class Renderer:
    """Supersampled inverse-mapping renderer for raw (distorted) frames."""

    def __init__(self, cam, ss=3, cache_path=None):
        self.cam, self.ss = cam, ss
        w, h = cam.size
        loaded = False
        if cache_path is not None:
            try:
                z = np.load(cache_path)
                if z["ss"] == ss and np.allclose(z["K"], cam.K) and np.allclose(z["D"], cam.D):
                    self.norm, self.valid = z["norm"], z["valid"]
                    loaded = True
            except (OSError, KeyError):
                pass
        if not loaded:
            off = (np.arange(ss) + 0.5) / ss - 0.5
            xs = (np.arange(w)[:, None] + off[None]).ravel()
            ys = (np.arange(h)[:, None] + off[None]).ravel()
            gx, gy = np.meshgrid(xs, ys)
            raw = np.stack([gx, gy], -1).astype(np.float64)
            norm = cam.undistort_norm(raw.reshape(-1, 2)).reshape(raw.shape)
            back = cam.distort_norm(norm)
            r = np.hypot(norm[..., 0], norm[..., 1])
            self.valid = (np.hypot(*(back - raw).transpose(2, 0, 1)) < 1e-3) & \
                         (r < cam.r_ideal_max * 0.999)
            self.norm = norm.astype(np.float32)
            if cache_path is not None:
                np.savez(cache_path, norm=self.norm, valid=self.valid, ss=ss, K=cam.K, D=cam.D)

    def background(self, rng):
        w, h = self.cam.size
        low = cv2.resize(rng.random((9, 16)).astype(np.float32), (w, h),
                         interpolation=cv2.INTER_CUBIC)
        img = 70 + 90 * low
        for _ in range(40):  # clutter: rectangles and bars that make extra contours
            x0, y0 = rng.integers(0, w), rng.integers(0, h)
            bw, bh = rng.integers(8, 120), rng.integers(8, 120)
            img[y0:y0 + bh, x0:x0 + bw] = rng.uniform(20, 230)
        return cv2.GaussianBlur(img, (0, 0), 1.2)

    def render(self, tags, rng, noise=2.0, blur=0.6, vignette=0.35, white=(170, 215),
               black=(20, 45), quantize=True):
        """quantize=False with noise=0 returns the clean float image, so callers
        can add their own noise to identical geometry and shading."""
        cam, ss = self.cam, self.ss
        w, h = cam.size
        canvas = np.repeat(np.repeat(self.background(rng), ss, 0), ss, 1)
        for tag in tags:
            bits = cv2.aruco.generateImageMarker(DICT, tag.id, TAG_CELLS, borderBits=1) > 0
            wv, bv = rng.uniform(*white), rng.uniform(*black)
            paper_raw = cam.distort_norm(project_norm(tag.plane_to_cam(tag.paper_plane())))
            x0, y0 = np.floor(paper_raw.min(0)).astype(int) - 2
            x1, y1 = np.ceil(paper_raw.max(0)).astype(int) + 3
            x0, y0, x1, y1 = max(x0, 0), max(y0, 0), min(x1, w), min(y1, h)
            sl = np.s_[y0 * ss:y1 * ss, x0 * ss:x1 * ss]
            nrm, ok = self.norm[sl].astype(np.float64), self.valid[sl]
            # ray (x, y, 1) meets the plane at plane coordinates Hinv @ (x, y, 1)
            H = np.column_stack([tag.R[:, 0], tag.R[:, 1], tag.t])
            q = np.concatenate([nrm, np.ones(nrm.shape[:2] + (1,))], -1) @ np.linalg.inv(H).T
            front = q[..., 2] * np.sign(np.linalg.inv(H)[2] @ np.r_[project_norm(tag.center_cam()[None])[0], 1]) > 0
            X, Y = q[..., 0] / q[..., 2], q[..., 1] / q[..., 2]
            m = tag.margin_cells * tag.size / TAG_CELLS
            on_paper = ok & front & (X >= -m) & (X < tag.size + m) & (Y >= -m) & (Y < tag.size + m)
            on_tag = on_paper & (X >= 0) & (X < tag.size) & (Y >= 0) & (Y < tag.size)
            cx = np.clip((X / tag.size * TAG_CELLS).astype(int), 0, TAG_CELLS - 1)
            cy = np.clip((Y / tag.size * TAG_CELLS).astype(int), 0, TAG_CELLS - 1)
            val = np.where(on_tag & ~bits[cy, cx], bv, wv)
            region = canvas[sl]
            region[on_paper] = val[on_paper]
        img = canvas.reshape(h, ss, w, ss).mean(axis=(1, 3))
        yy, xx = np.mgrid[0:h, 0:w]
        rr = np.hypot(xx - cam.K[0, 2], yy - cam.K[1, 2]) / np.hypot(w / 2, h / 2)
        img *= 1 - vignette * rr**2
        img = cv2.GaussianBlur(img.astype(np.float32), (0, 0), blur)
        if noise:
            img += rng.normal(0, noise, img.shape)
        if not quantize:
            return img
        return np.clip(np.rint(img), 0, 255).astype(np.uint8)


def make_scene(cam, rng, n_x=6, n_y=4, dist=(0.7, 1.4), size=0.08, tilt=25.0,
               ids=None, edge_px=4, fold_margin=0.97):
    """Tags whose raw centers spread over the whole frame, including edges."""
    w, h = cam.size
    tags, gts, boxes = [], [], []
    ids = list(ids) if ids is not None else list(rng.permutation(587))
    cw, ch = w / n_x, h / n_y
    for j in range(n_y):
        for i in range(n_x):
            u = (i + rng.uniform(0.1, 0.9)) * cw
            v = (j + rng.uniform(0.1, 0.9)) * ch
            if cam.raw_radius(np.array([u, v])) > cam.fold_radius_px * fold_margin:
                continue
            c = cam.undistort_norm(np.array([[u, v]]))[0]
            Z = rng.uniform(*dist)
            R = rot("x", rng.uniform(-tilt, tilt)) @ rot("y", rng.uniform(-tilt, tilt)) @ \
                rot("z", rng.uniform(0, 360))
            C = Z * np.array([c[0], c[1], 1.0])
            t = C - R @ np.array([size / 2, size / 2, 0])
            tag = Tag(int(ids.pop()), R, t, size)
            paper = tag.plane_to_cam(tag.paper_plane())
            if np.any(paper[:, 2] <= 0.05):
                continue
            pn = project_norm(paper)
            if np.any(np.hypot(pn[:, 0], pn[:, 1]) > cam.r_ideal_max * fold_margin):
                continue
            praw = cam.distort_norm(pn)
            if np.any(praw < edge_px) or np.any(praw[:, 0] > w - 1 - edge_px) or \
                    np.any(praw[:, 1] > h - 1 - edge_px):
                continue
            box = np.r_[praw.min(0), praw.max(0)]
            if any(not (box[2] < b[0] or b[2] < box[0] or box[3] < b[1] or b[3] < box[1])
                   for b in boxes):
                continue
            boxes.append(box)
            tags.append(tag)
            gts.append(ground_truth(cam, tag))
    return tags, gts


def proposal_roi(cam, gt, rng, expand=1.5, jitter=0.08, scale_sd=0.08):
    """Mimic a CNN proposal: noisy center and size, times roi_expand, clamped."""
    w, h = cam.size
    ext = gt.corners_raw.max(0) - gt.corners_raw.min(0)
    side = float(max(ext)) * math.exp(rng.normal(0, scale_sd)) * expand
    c = gt.center_raw + rng.normal(0, jitter * max(ext), 2)
    x0, y0 = np.clip(c - side / 2, 0, [w, h])
    x1, y1 = np.clip(c + side / 2, 0, [w, h])
    return int(round(x0)), int(round(y0)), int(round(x1)), int(round(y1))


# ------------------------------------------------------------ aruco_nano port

@dataclass
class Candidate:
    contour: np.ndarray  # Nx2 int, crop coordinates
    vidx: np.ndarray  # 4 contour indices of the approxPolyDP vertices, ascending
    quad: np.ndarray  # 4x2 float, aruco_nano-sorted (visually clockwise)


@dataclass
class Detection:
    id: int
    corners: np.ndarray  # 4x2, in whatever space the method reports
    cand: Candidate | None = None
    extra: dict = field(default_factory=dict)


class NanoParams:
    box_filter = 15
    thres = 3
    min_size = 10
    attempts = 5


def _nano_sort(q):
    q = q.copy()
    d1, d2 = q[1] - q[0], q[2] - q[0]
    if d1[0] * d2[1] - d1[1] * d2[0] < 0:
        q[[1, 3]] = q[[3, 1]]
    return q


def nano_candidates(gray, p=NanoParams):
    """aruco_nano threshold + contours + quads (cv2.findContours stands in for
    its visited-aware tracer)."""
    box = cv2.boxFilter(gray, -1, (p.box_filter, p.box_filter), normalize=True,
                        borderType=cv2.BORDER_REPLICATE | cv2.BORDER_ISOLATED)
    diff = cv2.subtract(box, gray)
    _, th = cv2.threshold(diff, p.thres, 255, cv2.THRESH_BINARY)
    contours, _ = cv2.findContours(th, cv2.RETR_LIST, cv2.CHAIN_APPROX_NONE)
    out = []
    for c in contours:
        if len(c) < 4 * p.min_size:
            continue
        approx = cv2.approxPolyDP(c, len(c) * 0.03, True)
        if len(approx) != 4 or not cv2.isContourConvex(approx):
            continue
        a = approx.reshape(4, 2)
        if np.any(np.sum((a - np.roll(a, -1, 0)) ** 2, 1) < p.min_size**2):
            continue
        cc = c.reshape(-1, 2)
        vidx = np.sort([int(np.nonzero((cc == v).all(1))[0][0]) for v in a])
        out.append(Candidate(cc, vidx, _nano_sort(a.astype(np.float64))))
    return out, th


def bilinear(img, pts):
    """aruco_nano getSubpixelValue, vectorized (0 outside the valid 2x2 area)."""
    pts = np.asarray(pts, dtype=np.float64)
    h, w = img.shape
    finite = np.isfinite(pts).all(-1)
    pts = np.where(finite[..., None], pts, -1.0)
    x, y = np.clip(pts[..., 0], -2, w + 1), np.clip(pts[..., 1], -2, h + 1)
    ix, iy = np.floor(x).astype(int), np.floor(y).astype(int)
    ok = finite & (ix >= 0) & (iy >= 0) & (ix < w - 1) & (iy < h - 1)
    ix, iy = np.where(ok, ix, 0), np.where(ok, iy, 0)
    dx, dy = x - ix, y - iy
    f = img.astype(np.float64) if img.dtype != np.float64 else img
    top = f[iy, ix] + dx * (f[iy, ix + 1] - f[iy, ix])
    bot = f[iy + 1, ix] + dx * (f[iy + 1, ix + 1] - f[iy + 1, ix])
    return np.where(ok, top + dy * (bot - top), 0.0)


def _thres255_adaptive(bits, off=2, thres=3):
    mean = cv2.boxFilter(bits, -1, (2 * off + 1, 2 * off + 1), normalize=True,
                         borderType=cv2.BORDER_REFLECT_101)
    return np.where(mean.astype(int) - thres < bits.astype(int), 255, 0).astype(np.uint8)


def identify_bits(bits255):
    """Strict border and dictionary check on an 8x8 0/255 bit image."""
    b = bits255 > 0
    border = np.r_[b[0], b[-1], b[1:-1, 0], b[1:-1, -1]]
    if border.any():
        return None
    ok, idx, rotation = DICT.identify(b[1:-1, 1:-1].astype(np.uint8), 0.0)
    return (int(idx), int(rotation)) if ok else None


def _unit_grid():
    g = (np.arange(TAG_CELLS) + 0.5) / TAG_CELLS
    gx, gy = np.meshgrid(g, g)
    return np.stack([gx, gy], -1)


def nano_decode(gray, quad, rng, p=NanoParams):
    """aruco_nano bit extraction: homography on raw corners, one sample per cell."""
    unit = np.float32([[0, 0], [1, 0], [1, 1], [0, 1]])
    grid = _unit_grid().reshape(-1, 1, 2)
    for attempt in range(p.attempts):
        q = quad.copy()
        if attempt:
            q = q + rng.normal(0, 0.75, q.shape)
        H = cv2.getPerspectiveTransform(unit, q.astype(np.float32))
        pts = cv2.perspectiveTransform(grid, H).reshape(TAG_CELLS, TAG_CELLS, 2)
        vals = np.floor(0.5 + bilinear(gray, pts)).clip(0, 255).astype(np.uint8)
        if attempt == 2:
            bits = _thres255_adaptive(vals)
        else:
            _, bits = cv2.threshold(vals, 0, 255, cv2.THRESH_OTSU)
        r = identify_bits(bits)
        if r is not None:
            idx, rotation = r
            return idx, np.roll(quad, rotation, 0)
    return None


def _points_into(a, b):
    return int(sum(cv2.pointPolygonTest(b.astype(np.float32).reshape(-1, 1, 2),
                                        (float(x), float(y)), False) > 0 for x, y in a))


def dedupe(dets):
    """aruco_nano isInto: drop the inner duplicate of the same id."""
    keep = [True] * len(dets)
    for i in range(len(dets)):
        for j in range(i + 1, len(dets)):
            if dets[i].id != dets[j].id or not keep[i] or not keep[j]:
                continue
            ab, ba = _points_into(dets[i].corners, dets[j].corners), \
                _points_into(dets[j].corners, dets[i].corners)
            if ab > ba:
                keep[i] = False
            elif ba > ab:
                keep[j] = False
    return [d for d, k in zip(dets, keep) if k]


def nano_detect(gray, rng=None, p=NanoParams, rejected=None):
    """rejected: optional list that receives the undecoded candidates, like
    aruco_nano's candidatesOut."""
    rng = rng if rng is not None else np.random.default_rng(0)
    cands, _ = nano_candidates(gray, p)
    dets = []
    for c in cands:
        r = nano_decode(gray, c.quad, rng, p)
        if r is not None:
            dets.append(Detection(r[0], r[1], c))
        elif rejected is not None:
            rejected.append(c)
    dets = dedupe(dets)
    if dets:
        pts = np.concatenate([d.corners for d in dets]).astype(np.float32).reshape(-1, 1, 2)
        cv2.cornerSubPix(gray, pts, (4, 4), (-1, -1),
                         (cv2.TERM_CRITERIA_MAX_ITER | cv2.TERM_CRITERIA_EPS, 12, 0.005))
        pts = pts.reshape(-1, 4, 2).astype(np.float64)
        for d, c in zip(dets, pts):
            d.corners = c
    return dets


_CV_DETECTOR = None


def cv_aruco_detect(gray, rng=None):
    """OpenCV ArucoDetector, strict decoding and subpixel refinement."""
    global _CV_DETECTOR
    if _CV_DETECTOR is None:
        prm = cv2.aruco.DetectorParameters()
        prm.cornerRefinementMethod = cv2.aruco.CORNER_REFINE_SUBPIX
        prm.errorCorrectionRate = 0.0
        _CV_DETECTOR = cv2.aruco.ArucoDetector(DICT, prm)
    corners, ids, _ = _CV_DETECTOR.detectMarkers(gray)
    if ids is None:
        return []
    return [Detection(int(i), c.reshape(4, 2).astype(np.float64)) for c, i in zip(corners, ids.ravel())]


STOCK = {"nano": nano_detect, "cv2": cv_aruco_detect}


# --------------------------------------------------------- point-level method

@dataclass
class PointParams:
    k_per_edge: int | None = 8  # None: every contour pixel of the edge
    margin: float = 0.15  # fraction of each edge skipped at both ends
    subpix: bool = True  # refine samples to the gray-level edge crossing
    profile_half: float = 4.0  # max. search half-length along the normal (px)
    robust: bool = True  # Huber line fit instead of plain least squares
    decode: str = "fwd"  # "fwd": forward-distorted grid; "raw": nano on raw quad
    cell_samples: int = 3  # per-axis samples per cell for the fwd decode
    chord_half_cap: float = 8.0  # refine_quad_chord pass-1 search cap (px)


def edge_samples(cand, k, margin):
    """Evenly spaced contour samples per edge, skipping the corner margins.
    Returns a list of 4 arrays (edge i runs from vertex i to vertex i+1)."""
    n = len(cand.contour)
    v = list(cand.vidx) + [cand.vidx[0] + n]
    edges = []
    for i in range(4):
        a, b = v[i], v[i + 1]
        L = b - a
        lo, hi = a + margin * L, b - margin * L
        if k is None:
            idx = np.arange(int(math.ceil(lo)), int(math.floor(hi)) + 1)
        else:
            idx = np.rint(np.linspace(lo, hi, k)).astype(int)
        edges.append(cand.contour[idx % n].astype(np.float64))
    return edges


PROFILE_STEP = 0.25  # px between edge-profile samples


def subpix_edge(gray_f, pts, normal, half):
    """Move each point along `normal` to the mid-level crossing of the profile.
    `normal` points from the dark (tag) side to the bright side."""
    step = PROFILE_STEP
    steps = np.arange(-half, half + 1e-9, step)
    prof = bilinear(gray_f, pts[:, None, :] + steps[None, :, None] * normal[None, None, :])
    ends = max(2, len(steps) // 6)
    lo, hi = prof[:, :ends].mean(1), prof[:, -ends:].mean(1)
    level = (lo + hi) / 2
    out, keep = pts.copy(), np.zeros(len(pts), bool)
    for i in range(len(pts)):
        if hi[i] - lo[i] < 15:
            continue
        s = np.sign(prof[i] - level[i])
        cross = np.nonzero((s[:-1] < 0) & (s[1:] >= 0))[0]
        if not len(cross):
            continue
        j = cross[np.argmin(np.abs(steps[cross]))]
        a, b = prof[i, j] - level[i], prof[i, j + 1] - level[i]
        t = steps[j] + step * a / (a - b)
        out[i] = pts[i] + t * normal
        keep[i] = True
    return out[keep]


def fit_line_irls(pts, iters=3, c=1.345):
    """Closed-form total least squares with a few Huber reweightings: the
    apps/common/point_ldc.cc line fit (cv::fitLine's DIST_HUBER costs about
    70 us per call on the Duo-S). Scale is 1.4826 * MAD of the residuals,
    floored at 0.05 px."""
    p = np.asarray(pts, dtype=np.float64)
    if len(p) < 2:
        return None
    w = np.ones(len(p))
    for it in range(iters + 1):
        m = (w[:, None] * p).sum(0) / w.sum()
        d = p - m
        cxx = (w * d[:, 0] ** 2).sum()
        cxy = (w * d[:, 0] * d[:, 1]).sum()
        cyy = (w * d[:, 1] ** 2).sum()
        theta = 0.5 * math.atan2(2 * cxy, cxx - cyy)  # direction of largest spread
        n = np.array([-math.sin(theta), math.cos(theta)])
        r = d @ n
        if it == iters:
            break
        scale = max(1.4826 * np.median(np.abs(r)), 0.05)
        a = np.abs(r) / (c * scale)
        w = np.where(a <= 1, 1.0, 1.0 / np.maximum(a, 1e-12))
    return np.r_[n, -(n @ m)]


def fit_line(pts, robust):
    if len(pts) < 2:
        return None
    if robust == "irls":
        return fit_line_irls(pts)
    dist = cv2.DIST_HUBER if robust else cv2.DIST_L2
    vx, vy, x0, y0 = cv2.fitLine(pts.astype(np.float32), dist, 0, 0.01, 0.01).ravel()
    n = np.array([-vy, vx], dtype=np.float64)
    return np.r_[n, -(n @ np.array([x0, y0], dtype=np.float64))]


def intersect(l1, l2):
    p = np.cross(l1, l2)
    return None if abs(p[2]) < 1e-12 else p[:2] / p[2]


def refine_loop(cam, gray_f, cand, offset, pp: PointParams):
    """Corrected-domain corners (ideal px, contour vertex order) from a loop.
    Returns (corners 4x2 | None, info dict)."""
    edges = edge_samples(cand, pp.k_per_edge, pp.margin)
    vtx = cand.contour[cand.vidx].astype(np.float64)
    centroid = vtx.mean(0)
    lines, raw_samples, ideal_samples = [], [], []
    for i, pts in enumerate(edges):
        if pp.subpix:
            d = vtx[(i + 1) % 4] - vtx[i]
            nrm = np.array([-d[1], d[0]]) / (np.linalg.norm(d) + 1e-12)
            mid = (vtx[(i + 1) % 4] + vtx[i]) / 2
            if nrm @ (mid - centroid) < 0:
                nrm = -nrm
            cell = np.linalg.norm(d) / TAG_CELLS
            pts = subpix_edge(gray_f, pts, nrm, min(pp.profile_half, max(1.5, 0.45 * cell)))
        full = pts + offset
        ideal = cam.undistort(full) if len(full) else full
        raw_samples.append(full)
        ideal_samples.append(ideal)
        lines.append(fit_line(ideal, pp.robust))
    info = {"raw_samples": raw_samples, "ideal_samples": ideal_samples,
            "n_points": int(sum(len(s) for s in raw_samples))}
    if any(l is None for l in lines):
        return None, info
    corners = [intersect(lines[(i - 1) % 4], lines[i]) for i in range(4)]
    if any(c is None for c in corners):
        return None, info
    return np.array(corners), info


def fwd_grid_points(cam, corners_ideal, samples=3):
    """Raw pixel positions of per-cell samples through the corrected-domain grid."""
    unit = np.float32([[0, 0], [1, 0], [1, 1], [0, 1]])
    H = cv2.getPerspectiveTransform(unit, corners_ideal.astype(np.float32))
    sub = (np.arange(samples) + 0.5) / samples * 0.5 + 0.25  # inner half of each cell
    cells = np.arange(TAG_CELLS)
    u = (cells[:, None] + sub[None]).ravel() / TAG_CELLS
    gx, gy = np.meshgrid(u, u)
    pts = cv2.perspectiveTransform(np.stack([gx, gy], -1).reshape(-1, 1, 2), H).reshape(gx.shape + (2,))
    return cam.distort_ideal(pts)


def fwd_decode(cam, gray_full, corners_ideal, samples=3):
    raw = fwd_grid_points(cam, corners_ideal, samples)
    vals = bilinear(gray_full, raw)
    n = TAG_CELLS
    cells = vals.reshape(n, samples, n, samples).mean(axis=(1, 3))
    _, bits = cv2.threshold(np.rint(cells).clip(0, 255).astype(np.uint8), 0, 255, cv2.THRESH_OTSU)
    return identify_bits(bits)


def loop_points_detect(cam, gray_full, roi, pp: PointParams, rng=None):
    """The proposed pipeline on one ROI of the raw frame."""
    rng = rng if rng is not None else np.random.default_rng(0)
    x0, y0, x1, y1 = roi
    crop = gray_full[y0:y1, x0:x1]
    cands, _ = nano_candidates(crop)
    off = np.array([x0, y0], dtype=np.float64)
    gray_f = crop.astype(np.float64)
    dets = []
    for c in cands:
        corners, info = refine_loop(cam, gray_f, c, off, pp)
        if corners is None:
            continue
        # clockwise in the corrected domain, as the bit reader expects
        d1, d2 = corners[1] - corners[0], corners[2] - corners[0]
        if d1[0] * d2[1] - d1[1] * d2[0] < 0:
            corners = corners[[0, 3, 2, 1]]
        if pp.decode == "fwd":
            r = fwd_decode(cam, gray_full, corners, pp.cell_samples)
            if r is None:
                continue
            idx, rotation = r
            corners = np.roll(corners, rotation, 0)
        else:
            r = nano_decode(crop, c.quad, rng)
            if r is None:
                continue
            idx = r[0]
        dets.append(Detection(idx, corners, c, info))
    return dedupe(dets)


def _edge_pass(cam, gray_full_f, pts, normals, half, robust, min_pts):
    """Subpixel-search each sample along its own normal, undistort, fit a line."""
    steps = np.arange(-half, half + 1e-9, 0.25)
    out = []
    for p, n in zip(pts, normals):
        r = subpix_edge(gray_full_f, p[None], n, half)
        if len(r):
            out.append(r[0])
    raw = np.array(out).reshape(-1, 2)
    ideal = cam.undistort(raw) if len(raw) else raw
    line = fit_line(ideal, robust) if len(ideal) >= min_pts else None
    return line, raw, ideal


def _corners_from_lines(lines):
    if any(l is None for l in lines):
        return None
    corners = [intersect(lines[(i - 1) % 4], lines[i]) for i in range(4)]
    return None if any(c is None for c in corners) else np.array(corners)


def refine_quad_chord(cam, gray_full_f, quad, pp: PointParams, passes=2):
    """The C++ (apps/common/point_ldc) variant. Pass 1 samples each edge on the
    raw chord between two corners and searches along the chord normal. Later
    passes place samples on the previous corrected-domain edge, forward-distort
    them onto the curved raw edge, and search along the curve's local normal
    with a short window. Lines use fit_line_irls, as the C++ does. Corner j
    stays in quad[j]'s slot."""
    q = np.asarray(quad, dtype=np.float64)
    k = pp.k_per_edge or 16
    t = np.linspace(pp.margin, 1 - pp.margin, k)
    min_pts = max(2, k // 2)
    centroid = q.mean(0)
    side = np.mean(np.linalg.norm(q - np.roll(q, -1, 0), axis=1))
    wide = min(pp.chord_half_cap, max(1.5, 0.45 * side / TAG_CELLS))
    lines, raw_s, ideal_s = [], [], []
    for i in range(4):
        a, b = q[i], q[(i + 1) % 4]
        d = b - a
        nrm = np.array([-d[1], d[0]]) / (np.linalg.norm(d) + 1e-12)
        if nrm @ ((a + b) / 2 - centroid) < 0:
            nrm = -nrm
        pts = a[None] + t[:, None] * d[None]
        l, r, idl = _edge_pass(cam, gray_full_f, pts, [nrm] * k, wide, "irls", min_pts)
        lines.append(l); raw_s.append(r); ideal_s.append(idl)
    corners = _corners_from_lines(lines)
    narrow = min(wide, 1.5)
    for _ in range(passes - 1):
        if corners is None:
            break
        lines, raw_s, ideal_s = [], [], []
        eps = 0.5
        for i in range(4):
            a, b = corners[i], corners[(i + 1) % 4]
            ideal_pts = a[None] + t[:, None] * (b - a)[None]
            du = (b - a) / (np.linalg.norm(b - a) + 1e-12)
            raw = cam.distort_ideal(ideal_pts)
            tang = cam.distort_ideal(ideal_pts + eps * du) - cam.distort_ideal(ideal_pts - eps * du)
            nrm = np.c_[-tang[:, 1], tang[:, 0]]
            nrm /= np.linalg.norm(nrm, axis=1, keepdims=True) + 1e-12
            outward = cam.distort_ideal(((a + b) / 2)[None])[0] - cam.distort_ideal(corners.mean(0)[None])[0]
            nrm *= np.sign(nrm @ outward)[:, None]
            l, r, idl = _edge_pass(cam, gray_full_f, raw, nrm, narrow, "irls", min_pts)
            lines.append(l); raw_s.append(r); ideal_s.append(idl)
        corners = _corners_from_lines(lines)
    info = {"raw_samples": raw_s, "ideal_samples": ideal_s,
            "n_points": int(sum(len(x) for x in raw_s))}
    return corners, info


def point_ldc_detect(cam, gray_full, roi, pp: PointParams, rng=None, fallback=True,
                     max_fallback=8):
    """Decode-first point-level LDC, as implemented in C++: aruco_nano decodes
    as usual; decoded tags get chord-sampled edge refinement in the corrected
    domain; undecoded candidates not inside a decoded tag get the same
    refinement plus forward-distorted-grid decoding."""
    rng = rng if rng is not None else np.random.default_rng(0)
    x0, y0, x1, y1 = roi
    crop = gray_full[y0:y1, x0:x1]
    off = np.array([x0, y0], dtype=np.float64)
    gray_f = gray_full.astype(np.float64)
    rejected = []
    dets = nano_detect(crop, rng, rejected=rejected)
    out = []
    for d in dets:
        raw = d.corners + off
        corners, info = refine_quad_chord(cam, gray_f, raw, pp)
        if corners is None:
            corners, info = cam.undistort(raw), {"n_points": 0, "fallback_corners": True}
        out.append(Detection(d.id, corners, d.cand, info))
    if fallback:
        tried = 0
        for c in sorted(rejected, key=lambda c: -abs(cv2.contourArea(c.quad.astype(np.float32)))):
            if tried >= max_fallback:
                break
            quad = c.quad + off
            ctr = quad.mean(0)
            if any(cv2.pointPolygonTest((d.corners + off).astype(np.float32).reshape(-1, 1, 2),
                                        (float(ctr[0]), float(ctr[1])), False) >= 0 for d in dets):
                continue
            tried += 1
            corners, info = refine_quad_chord(cam, gray_f, quad, pp)
            if corners is None:
                continue
            r = fwd_decode(cam, gray_full, corners, pp.cell_samples)
            if r is None:
                continue
            info["fwd_decoded"] = True
            out.append(Detection(r[0], np.roll(corners, r[1], 0), c, info))
    return dedupe(out)


# ------------------------------------------------------------------ methods

def _crop(img, roi):
    x0, y0, x1, y1 = roi
    return img[y0:y1, x0:x1], np.array([x0, y0], dtype=np.float64)


def m_raw(cam, frame, roi, stock, correct, rng):
    crop, off = _crop(frame, roi)
    dets = STOCK[stock](crop, rng)
    for d in dets:
        d.corners = d.corners + off
        if correct:
            d.corners = cam.undistort(d.corners)
    return dets


def roi_box_ideal(cam, roi, n=12):
    x0, y0, x1, y1 = roi
    s = np.linspace(0, 1, n)
    edge = np.concatenate([np.stack([x0 + s * (x1 - x0), np.full(n, y0)], 1),
                           np.stack([np.full(n, x1), y0 + s * (y1 - y0)], 1),
                           np.stack([x0 + s * (x1 - x0), np.full(n, y1)], 1),
                           np.stack([np.full(n, x0), y0 + s * (y1 - y0)], 1)])
    return cam.undistort(edge)


class FullCorrector:
    """A full-frame correction: remap maps, a subsampled output->source map for
    locating ROIs, and the conversion from output pixels to ideal pixels."""

    def __init__(self, name, mapx, mapy, out_to_ideal, step=4):
        self.name = name
        self.m1, self.m2 = cv2.convertMaps(mapx, mapy, cv2.CV_16SC2)
        self.step = step
        self.sx, self.sy = mapx[::step, ::step], mapy[::step, ::step]
        self.out_to_ideal = out_to_ideal

    def correct(self, frame):
        """Full-frame correction; SG2000 does this in VPSS/GDC or in software."""
        return cv2.remap(frame, self.m1, self.m2, cv2.INTER_LINEAR)

    def roi_box(self, roi):
        """Output-image box whose sources fall inside the raw ROI."""
        x0, y0, x1, y1 = roi
        m = (self.sx >= x0) & (self.sx < x1) & (self.sy >= y0) & (self.sy < y1)
        ys, xs = np.nonzero(m)
        if not len(xs):
            return None
        s = self.step
        return xs.min() * s, ys.min() * s, (xs.max() + 1) * s, (ys.max() + 1) * s


def opencv_corrector(cam, zoom=1.0):
    """cv::initUndistortRectifyMap with newK = K scaled by zoom (the SW LDC view
    when zoom = 1)."""
    newK = cam.K.copy()
    newK[0, 0] *= zoom
    newK[1, 1] *= zoom
    mapx, mapy = cv2.initUndistortRectifyMap(cam.K, cam.D, None, newK, cam.size, cv2.CV_32FC1)
    pp, s = newK[:2, 2].copy(), cam.K[0, 0] / newK[0, 0]
    return FullCorrector(f"opencv(zoom={zoom})", mapx, mapy,
                         lambda p: (np.asarray(p) - pp) * s + cam.K[:2, 2])


def hw_ldc_source(size, ratio, center_x, center_y, view_ratio=100):
    """SG2000 gdc_mesh.c one-ratio mapping (output pixel -> source pixel),
    evaluated per pixel rather than per mesh knot."""
    w, h = size
    norm = math.sqrt((w // 2) ** 2 + (h // 2) ** 2)
    k = ratio / 1000.0
    gain = max(1 + k * (h / 2) ** 2 / norm**2, 1 + k * (w / 2) ** 2 / norm**2)
    shrink = 1 - 0.333 * (100 - view_ratio) / 100

    def f(p):
        p = np.asarray(p, dtype=np.float64)
        x = (p[..., 0] - w / 2 - center_x) / gain * shrink
        y = (p[..., 1] - h / 2 - center_y) / gain * shrink
        rd = np.minimum(norm, np.hypot(x, y))
        s = 1 + k * (rd / norm) ** 2
        return np.stack([x * s + w / 2 + center_x, y * s + h / 2 + center_y], -1)
    return f, gain


def hw_corrector(cam, ldc, fit_radius=200.0):
    """Hardware LDC output, read as a pinhole image whose affine camera is fitted
    on the central region (where the one-ratio model and the calibration agree)."""
    w, h = cam.size
    src, _ = hw_ldc_source(cam.size, ldc["ratio"], ldc["center_x"], ldc["center_y"],
                           ldc.get("view_ratio", 100))
    gx, gy = np.meshgrid(np.arange(w, dtype=np.float64), np.arange(h, dtype=np.float64))
    out = np.stack([gx, gy], -1)
    s = src(out)
    c = np.array([w / 2 + ldc["center_x"], h / 2 + ldc["center_y"]])
    g = out[::8, ::8].reshape(-1, 2)
    g = g[np.hypot(*(g - c).T) < fit_radius]
    ideal = cam.undistort(src(g))
    A = np.c_[g, np.ones(len(g))]
    coef, *_ = np.linalg.lstsq(A, ideal, rcond=None)
    corr = FullCorrector("hw_ldc", s[..., 0].astype(np.float32), s[..., 1].astype(np.float32),
                         lambda p: np.c_[np.asarray(p).reshape(-1, 2), np.ones(np.size(p) // 2)]
                         @ coef)
    corr.affine, corr.source = coef, src
    return corr


def m_full(corr: FullCorrector, corrected, roi, stock, rng):
    """Detect on a pre-corrected full frame (correction time is charged per frame)."""
    box = corr.roi_box(roi)
    if box is None:
        return []
    x0, y0, x1, y1 = box
    if x1 - x0 < 16 or y1 - y0 < 16:
        return []
    dets = STOCK[stock](corrected[y0:y1, x0:x1], rng)
    for d in dets:
        d.corners = corr.out_to_ideal(d.corners + [x0, y0]).reshape(4, 2)
    return dets


def roi_remap_crop(cam, frame, roi):
    """Corrected-domain (ideal px) crop covering the raw ROI, and its origin."""
    ideal = roi_box_ideal(cam, roi)
    x0, y0 = np.floor(ideal.min(0)).astype(int)
    x1, y1 = np.ceil(ideal.max(0)).astype(int)
    gx, gy = np.meshgrid(np.arange(x0, x1, dtype=np.float64), np.arange(y0, y1, dtype=np.float64))
    raw = cam.distort_ideal(np.stack([gx, gy], -1)).astype(np.float32)
    crop = cv2.remap(frame, raw[..., 0], raw[..., 1], cv2.INTER_LINEAR,
                     borderMode=cv2.BORDER_REPLICATE)
    return crop, np.array([x0, y0], dtype=np.float64)


def m_roi_remap(cam, frame, roi, stock, rng):
    """Correct only the ROI's corrected-domain bounding box (maps built live)."""
    crop, off = roi_remap_crop(cam, frame, roi)
    dets = STOCK[stock](crop, rng)
    for d in dets:
        d.corners = d.corners + off
    return dets


class IdentityCam:
    """A distortion-free camera, so the point pipeline can run on an image
    that is already corrected."""

    def undistort(self, px):
        return np.asarray(px, dtype=np.float64)

    def distort_ideal(self, px):
        return np.asarray(px, dtype=np.float64)


def m_roi_remap_edgefit(cam, frame, roi, pp, rng):
    """roi_remap, but with loop_points' corner estimator (subpixel edge samples,
    line fits, intersections) run on the corrected crop. Against loop_points,
    only the domain where edges are measured differs."""
    crop, off = roi_remap_crop(cam, frame, roi)
    h, w = crop.shape
    dets = loop_points_detect(IdentityCam(), crop, (0, 0, w, h), pp, rng)
    for d in dets:
        d.corners = d.corners + off
    return dets


# --------------------------------------------------------------- evaluation

def align(corners, gt_corners):
    """Best cyclic alignment; returns (aligned corners, per-corner errors)."""
    best = None
    for s in range(4):
        c = np.roll(corners, s, 0)
        e = np.linalg.norm(c - gt_corners, axis=1)
        if best is None or e.mean() < best[1].mean():
            best = (c, e)
    return best


def pose_error(cam, gt: GroundTruth, corners_ideal):
    obj = np.c_[gt.tag.corners_plane(), np.zeros(4)].astype(np.float64)
    ok, rvec, tvec = cv2.solvePnP(obj, corners_ideal.astype(np.float64), cam.K, None,
                                  flags=cv2.SOLVEPNP_IPPE)
    if not ok:
        return np.nan, np.nan
    R = cv2.Rodrigues(rvec)[0]
    dR = R @ gt.tag.R.T
    ang = math.degrees(math.acos(np.clip((np.trace(dR) - 1) / 2, -1, 1)))
    terr = np.linalg.norm(tvec.ravel() - gt.tag.t) / np.linalg.norm(gt.tag.t) * 100
    return ang, terr


METHODS = ["raw_nocorr", "raw_corners", "full_remap", "full_remap_hw", "roi_remap",
           "loop_points", "loop_points_px"]


def run_method(m, cam, frame, roi, rng, stock="nano", pp=None, pp_px=None, correctors=None,
               corrected=None):
    """One method on one ROI. Full-frame methods need `corrected[m]` precomputed."""
    pp = pp or PointParams()
    if m == "raw_nocorr":
        return m_raw(cam, frame, roi, stock, False, rng)
    if m == "raw_corners":
        return m_raw(cam, frame, roi, stock, True, rng)
    if m.startswith("full_remap"):
        return m_full(correctors[m], corrected[m], roi, stock, rng)
    if m == "roi_remap":
        return m_roi_remap(cam, frame, roi, stock, rng)
    if m == "loop_points":
        return loop_points_detect(cam, frame, roi, pp, rng)
    if m == "point_ldc":
        return point_ldc_detect(cam, frame, roi, pp, rng)
    if m == "roi_remap_edgefit":
        return m_roi_remap_edgefit(cam, frame, roi, pp, rng)
    if m == "loop_points_rawdec":
        return loop_points_detect(cam, frame, roi, PointParams(**{**pp.__dict__, "decode": "raw"}),
                                  rng)
    if m == "loop_points_px":
        return loop_points_detect(cam, frame, roi,
                                  pp_px or PointParams(**{**pp.__dict__, "subpix": False}), rng)
    raise ValueError(m)


def run_methods(cam, frame, gts, rois, rng, stock="nano", pp=None, methods=METHODS,
                correctors=None):
    """Run every method on every ROI of one frame. Returns a list of row dicts.
    correctors: {"full_remap": FullCorrector, "full_remap_hw": FullCorrector}."""
    pp = pp or PointParams()
    pp_px = PointParams(**{**pp.__dict__, "subpix": False})
    correctors = correctors or {}
    rows, frame_ms, corrected = [], {}, {}
    for m, corr in correctors.items():
        if m in methods:
            t = time.perf_counter()
            corrected[m] = corr.correct(frame)
            frame_ms[m] = (time.perf_counter() - t) * 1e3
    for gt, roi in zip(gts, rois):
        for m in methods:
            if m.startswith("full_remap") and m not in correctors:
                continue
            t = time.perf_counter()
            dets = run_method(m, cam, frame, roi, rng, stock, pp, pp_px, correctors, corrected)
            ms = (time.perf_counter() - t) * 1e3
            hit = [d for d in dets if d.id == gt.tag.id]
            row = dict(method=m, id=gt.tag.id, radius=gt.radius_raw, side=gt.side_raw,
                       found=bool(hit), false_ids=sum(d.id != gt.tag.id for d in dets),
                       ms=ms, frame_ms=frame_ms.get(m, 0.0), gt=gt.corners_ideal)
            if hit:
                c, e = align(hit[0].corners, gt.corners_ideal)
                row.update(err_mean=float(e.mean()), err_max=float(e.max()))
                row["rot_deg"], row["t_pct"] = pose_error(cam, gt, c)
                row["n_points"] = hit[0].extra.get("n_points", 0)
                row["corners"] = c  # ideal px, aligned to the ground-truth order
            rows.append(row)
    return rows
