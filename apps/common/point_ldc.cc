#include "point_ldc.h"

#include <opencv2/imgproc/imgproc.hpp>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kTagCells = 8; // 36h11: 6x6 data plus a one-cell black border

// Homogeneous line a*x + b*y + c = 0 with (a, b) a unit normal.
struct Line
{
    double a = 0, b = 0, c = 0;
};

// Total least squares with a few Huber reweightings (scale 1.4826 * MAD of the
// residuals, floored at 0.05 px). Mirrors ldcpt.fit_line_irls. cv::fitLine's
// DIST_HUBER gives the same accuracy but costs about 70 us per call on the
// Duo-S's A53, against about 2 us for this.
bool fit_line(const std::vector<cv::Point2f> &points, Line &line)
{
    constexpr int kIterations = 3;
    constexpr double kHuber = 1.345;
    const size_t n = points.size();
    if (n < 2)
        return false;
    std::vector<double> w(n, 1.0), r(n), abs_r(n);
    double mx = 0, my = 0, nx = 0, ny = 0;
    for (int it = 0; it <= kIterations; ++it)
    {
        double sw = 0;
        mx = my = 0;
        for (size_t i = 0; i < n; ++i)
        {
            sw += w[i];
            mx += w[i] * points[i].x;
            my += w[i] * points[i].y;
        }
        mx /= sw;
        my /= sw;
        double cxx = 0, cxy = 0, cyy = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const double dx = points[i].x - mx, dy = points[i].y - my;
            cxx += w[i] * dx * dx;
            cxy += w[i] * dx * dy;
            cyy += w[i] * dy * dy;
        }
        const double theta = 0.5 * std::atan2(2 * cxy, cxx - cyy); // direction of largest spread
        nx = -std::sin(theta);
        ny = std::cos(theta);
        if (it == kIterations)
            break;
        for (size_t i = 0; i < n; ++i)
        {
            r[i] = (points[i].x - mx) * nx + (points[i].y - my) * ny;
            abs_r[i] = std::fabs(r[i]);
        }
        // np.median: mean of the two middle values for an even count.
        std::vector<double> sorted(abs_r);
        std::sort(sorted.begin(), sorted.end());
        const double med = n % 2 ? sorted[n / 2] : 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);
        const double scale = std::max(1.4826 * med, 0.05);
        for (size_t i = 0; i < n; ++i)
        {
            const double a = abs_r[i] / (kHuber * scale);
            w[i] = a <= 1 ? 1.0 : 1.0 / a;
        }
    }
    line.a = nx;
    line.b = ny;
    line.c = -(nx * mx + ny * my);
    return true;
}

bool intersect(const Line &l1, const Line &l2, cv::Point2d &p)
{
    const double w = l1.a * l2.b - l1.b * l2.a;
    if (std::fabs(w) < 1e-12)
        return false;
    p.x = (l1.b * l2.c - l1.c * l2.b) / w;
    p.y = (l1.c * l2.a - l1.a * l2.c) / w;
    return true;
}

// Move `p` along the unit `normal` (dark tag side -> bright side) to the
// mid-level crossing of the grey profile nearest to p. Mirrors
// ldcpt.subpix_edge.
bool subpix_edge(const cv::Mat &frame, const cv::Point2d &p, const cv::Point2d &normal,
                 double half, int min_contrast, cv::Point2d &edge)
{
    constexpr double kStep = 0.25;
    constexpr int kMaxSteps = 128;
    const int n = static_cast<int>(std::floor(2 * half / kStep + 1e-9)) + 1;
    if (n < 3 || n > kMaxSteps)
        return false;
    float profile[kMaxSteps];
    for (int i = 0; i < n; ++i)
    {
        const double t = -half + i * kStep;
        profile[i] = sample_bilinear(frame, static_cast<float>(p.x + t * normal.x),
                                     static_cast<float>(p.y + t * normal.y));
    }
    const int ends = std::max(2, n / 6);
    double lo = 0, hi = 0;
    for (int i = 0; i < ends; ++i)
    {
        lo += profile[i];
        hi += profile[n - 1 - i];
    }
    lo /= ends;
    hi /= ends;
    if (hi - lo < min_contrast)
        return false;
    const double level = (lo + hi) / 2;
    int best = -1;
    double best_dist = 1e9;
    for (int i = 0; i + 1 < n; ++i)
    {
        if (profile[i] - level < 0 && profile[i + 1] - level >= 0)
        {
            const double dist = std::fabs(-half + i * kStep);
            if (dist < best_dist)
            {
                best_dist = dist;
                best = i;
            }
        }
    }
    if (best < 0)
        return false;
    const double a = profile[best] - level, b = profile[best + 1] - level;
    const double t = -half + best * kStep + kStep * a / (a - b);
    edge = cv::Point2d(p.x + t * normal.x, p.y + t * normal.y);
    return true;
}

cv::Point2d unit(const cv::Point2d &v)
{
    const double n = std::sqrt(v.x * v.x + v.y * v.y);
    return n > 1e-12 ? v * (1.0 / n) : cv::Point2d(0, 0);
}

} // namespace

// ---------------------------------------------------------------- LensModel

bool LensModel::init(const double camera_matrix[9], const std::vector<double> &distortion,
                     int calibration_width, int calibration_height, int width, int height,
                     std::string &error)
{
    if (calibration_width <= 0 || calibration_height <= 0 || width < 2 || height < 2)
    {
        error = "point LDC needs valid calibration and frame sizes";
        return false;
    }
    const double aspect_in = static_cast<double>(calibration_width) / calibration_height;
    const double aspect_out = static_cast<double>(width) / height;
    if (std::fabs(aspect_in - aspect_out) > 0.005 * aspect_out)
    {
        error = "calibration aspect does not match the point LDC frame size";
        return false;
    }
    if (distortion.size() < 4)
    {
        error = "point LDC needs at least 4 distortion coefficients";
        return false;
    }
    for (size_t i = 5; i < distortion.size(); ++i)
    {
        if (distortion[i] != 0.0)
        {
            error = "point LDC supports k1 k2 p1 p2 k3 only (rational, prism and tilt terms must be 0)";
            return false;
        }
    }

    // Same scaling as SoftwareLdc: pixel-centre principal point.
    const double sx = static_cast<double>(width) / calibration_width;
    const double sy = static_cast<double>(height) / calibration_height;
    const double *k = camera_matrix;
    camera_ = cv::Matx33d(k[0] * sx, k[1] * sx, (k[2] + 0.5) * sx - 0.5,
                          k[3] * sy, k[4] * sy, (k[5] + 0.5) * sy - 0.5,
                          k[6], k[7], k[8]);
    fx_ = camera_(0, 0);
    fy_ = camera_(1, 1);
    cx_ = camera_(0, 2);
    cy_ = camera_(1, 2);
    skew_ = camera_(0, 1);
    k1_ = distortion[0];
    k2_ = distortion[1];
    p1_ = distortion[2];
    p2_ = distortion[3];
    k3_ = distortion.size() > 4 ? distortion[4] : 0.0;
    size_ = cv::Size(width, height);

    // Radial fold: the first ideal radius where r * radial(r) stops increasing.
    double r_max = 3.0, rd_max = 0;
    double prev = 0;
    for (int i = 1; i <= 30000; ++i)
    {
        const double r = i * 1e-4, r2 = r * r;
        const double rd = r * (1 + r2 * (k1_ + r2 * (k2_ + r2 * k3_)));
        if (rd <= prev)
        {
            r_max = r - 1e-4;
            break;
        }
        prev = rd_max = rd;
    }
    r2_ideal_max_ = r_max * r_max * 0.999;
    fold_radius_px_ = rd_max * fx_;
    return true;
}

void LensModel::distort_norm(double x, double y, double &xd, double &yd) const
{
    const double r2 = x * x + y * y;
    const double radial = 1 + r2 * (k1_ + r2 * (k2_ + r2 * k3_));
    xd = x * radial + 2 * p1_ * x * y + p2_ * (r2 + 2 * x * x);
    yd = y * radial + p1_ * (r2 + 2 * y * y) + 2 * p2_ * x * y;
}

cv::Point2d LensModel::distort(const cv::Point2d &ideal) const
{
    const double y = (ideal.y - cy_) / fy_;
    const double x = (ideal.x - cx_ - skew_ * y) / fx_;
    double xd, yd;
    distort_norm(x, y, xd, yd);
    return cv::Point2d(fx_ * xd + skew_ * yd + cx_, fy_ * yd + cy_);
}

bool LensModel::undistort(const cv::Point2d &raw, cv::Point2d &ideal) const
{
    const double yd = (raw.y - cy_) / fy_;
    const double xd = (raw.x - cx_ - skew_ * yd) / fx_;
    double x = xd, y = yd;
    for (int iter = 0; iter < 30; ++iter)
    {
        const double r2 = x * x + y * y;
        const double radial = 1 + r2 * (k1_ + r2 * (k2_ + r2 * k3_));
        const double dradial = k1_ + r2 * (2 * k2_ + 3 * k3_ * r2); // d radial / d r2
        const double fx = x * radial + 2 * p1_ * x * y + p2_ * (r2 + 2 * x * x) - xd;
        const double fy = y * radial + p1_ * (r2 + 2 * y * y) + 2 * p2_ * x * y - yd;
        const double j11 = radial + 2 * x * x * dradial + 2 * p1_ * y + 6 * p2_ * x;
        const double j12 = 2 * x * y * dradial + 2 * p1_ * x + 2 * p2_ * y;
        const double j21 = j12;
        const double j22 = radial + 2 * y * y * dradial + 6 * p1_ * y + 2 * p2_ * x;
        const double det = j11 * j22 - j12 * j21;
        if (det <= 1e-12)
            return false; // at or past the fold
        double dx = (j22 * fx - j12 * fy) / det;
        double dy = (j11 * fy - j21 * fx) / det;
        // Damp steps that would leave the invertible range.
        for (int halve = 0; halve < 8; ++halve)
        {
            const double nx = x - dx, ny = y - dy;
            if (nx * nx + ny * ny < r2_ideal_max_)
                break;
            dx *= 0.5;
            dy *= 0.5;
        }
        x -= dx;
        y -= dy;
        if (dx * dx + dy * dy < 1e-24)
            break;
    }
    if (x * x + y * y >= r2_ideal_max_)
        return false;
    double cxd, cyd;
    distort_norm(x, y, cxd, cyd);
    if ((cxd - xd) * (cxd - xd) + (cyd - yd) * (cyd - yd) > 1e-16)
        return false; // 1e-8 normalized, far below a thousandth of a pixel
    ideal = cv::Point2d(fx_ * x + skew_ * y + cx_, fy_ * y + cy_);
    return true;
}

// -------------------------------------------------------------- refinement

float sample_bilinear(const cv::Mat &frame, float x, float y)
{
    if (!(x >= 0.f) || !(y >= 0.f))
        return 0.f;
    const int ix = static_cast<int>(x), iy = static_cast<int>(y);
    if (ix >= frame.cols - 1 || iy >= frame.rows - 1)
        return 0.f;
    const float dx = x - ix, dy = y - iy;
    const uchar *p = frame.ptr<uchar>(iy) + ix;
    const size_t step = frame.step;
    const float top = p[0] + dx * (p[1] - p[0]);
    const float bot = p[step] + dx * (p[step + 1] - p[step]);
    return top + dy * (bot - top);
}

namespace {

// One edge: search each raw sample along its normal, undistort the hits, and
// fit a corrected-domain line.
bool edge_pass(const LensModel &model, const cv::Mat &frame,
               const std::vector<cv::Point2d> &samples, const std::vector<cv::Point2d> &normals,
               double half, const PointLdcParams &params, int min_points, Line &line,
               PointLdcRefineStats *stats)
{
    std::vector<cv::Point2f> ideal;
    ideal.reserve(samples.size());
    for (size_t i = 0; i < samples.size(); ++i)
    {
        cv::Point2d edge, corrected;
        if (!subpix_edge(frame, samples[i], normals[i], half, params.min_edge_contrast, edge))
            continue;
        if (stats)
            ++stats->undistorts;
        if (!model.undistort(edge, corrected))
            continue;
        ideal.push_back(cv::Point2f(static_cast<float>(corrected.x),
                                    static_cast<float>(corrected.y)));
    }
    if (stats)
        stats->samples += static_cast<int>(ideal.size());
    if (static_cast<int>(ideal.size()) < min_points)
        return false;
    return fit_line(ideal, line);
}

bool corners_from_lines(const Line lines[4], cv::Point2d corners[4])
{
    for (int j = 0; j < 4; ++j)
        if (!intersect(lines[(j + 3) % 4], lines[j], corners[j]))
            return false;
    return true;
}

} // namespace

bool refine_quad_ideal(const LensModel &model, const cv::Mat &frame,
                       const cv::Point2f raw_corners[4], const PointLdcParams &params,
                       cv::Point2d ideal_corners[4], PointLdcRefineStats *stats)
{
    CV_Assert(frame.type() == CV_8UC1);
    const int k = std::max(2, params.samples_per_edge);
    const int min_points = std::max(2, k / 2);
    std::vector<double> t(k);
    for (int i = 0; i < k; ++i)
        t[i] = params.corner_margin + (1 - 2 * params.corner_margin) * i / (k - 1);

    cv::Point2d q[4], centroid(0, 0);
    double side = 0;
    for (int j = 0; j < 4; ++j)
    {
        q[j] = cv::Point2d(raw_corners[j].x, raw_corners[j].y);
        centroid += q[j] * 0.25;
    }
    for (int j = 0; j < 4; ++j)
        side += cv::norm(q[(j + 1) % 4] - q[j]) / 4;
    const double wide = std::min(params.first_pass_cap, std::max(1.5, 0.45 * side / kTagCells));
    const double narrow = std::min(wide, params.later_pass_half);

    std::vector<cv::Point2d> samples(k), normals(k);
    Line lines[4];
    for (int j = 0; j < 4; ++j)
    {
        const cv::Point2d a = q[j], b = q[(j + 1) % 4], d = b - a;
        cv::Point2d n = unit(cv::Point2d(-d.y, d.x));
        if (n.dot((a + b) * 0.5 - centroid) < 0)
            n = -n;
        for (int i = 0; i < k; ++i)
        {
            samples[i] = a + d * t[i];
            normals[i] = n;
        }
        if (!edge_pass(model, frame, samples, normals, wide, params, min_points, lines[j], stats))
            return false;
    }
    cv::Point2d corners[4];
    if (!corners_from_lines(lines, corners))
        return false;

    constexpr double kEps = 0.5; // ideal px, for the raw tangent by central difference
    for (int pass = 1; pass < params.passes; ++pass)
    {
        cv::Point2d mid(0, 0);
        for (int j = 0; j < 4; ++j)
            mid += corners[j] * 0.25;
        const cv::Point2d raw_mid = model.distort(mid);
        for (int j = 0; j < 4; ++j)
        {
            const cv::Point2d a = corners[j], b = corners[(j + 1) % 4];
            const cv::Point2d du = unit(b - a);
            const cv::Point2d outward = model.distort((a + b) * 0.5) - raw_mid;
            for (int i = 0; i < k; ++i)
            {
                const cv::Point2d p = a + (b - a) * t[i];
                samples[i] = model.distort(p);
                const cv::Point2d tangent = model.distort(p + du * kEps) - model.distort(p - du * kEps);
                cv::Point2d n = unit(cv::Point2d(-tangent.y, tangent.x));
                if (n.dot(outward) < 0)
                    n = -n;
                normals[i] = n;
            }
            if (!edge_pass(model, frame, samples, normals, narrow, params, min_points, lines[j], stats))
                return false;
        }
        if (!corners_from_lines(lines, corners))
            return false;
    }
    for (int j = 0; j < 4; ++j)
        ideal_corners[j] = corners[j];
    return true;
}

void forward_grid_points(const LensModel &model, const cv::Point2d ideal_corners[4],
                         int cells, int samples, std::vector<cv::Point2f> &raw_points)
{
    const cv::Point2f unit_square[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    cv::Point2f dst[4];
    for (int j = 0; j < 4; ++j)
        dst[j] = cv::Point2f(static_cast<float>(ideal_corners[j].x),
                             static_cast<float>(ideal_corners[j].y));
    const cv::Matx33d h(cv::getPerspectiveTransform(unit_square, dst));
    const int n = cells * samples;
    std::vector<double> u(n);
    for (int c = 0; c < cells; ++c)
        for (int s = 0; s < samples; ++s)
            u[c * samples + s] = (c + ((s + 0.5) / samples) * 0.5 + 0.25) / cells;
    raw_points.resize(static_cast<size_t>(n) * n);
    for (int row = 0; row < n; ++row)
    {
        for (int col = 0; col < n; ++col)
        {
            const double x = u[col], y = u[row];
            const double w = h(2, 0) * x + h(2, 1) * y + h(2, 2);
            const cv::Point2d ideal((h(0, 0) * x + h(0, 1) * y + h(0, 2)) / w,
                                    (h(1, 0) * x + h(1, 1) * y + h(1, 2)) / w);
            const cv::Point2d raw = model.distort(ideal);
            raw_points[static_cast<size_t>(row) * n + col] =
                cv::Point2f(static_cast<float>(raw.x), static_cast<float>(raw.y));
        }
    }
}
