///////////////////////////////////////////////////////////////////////////////
//
//      TargaImageNPR.cpp
//
//      The advanced non-photorealistic rendering commands:
//      npr-paint-advanced, npr-cartoon and npr-watercolor.
//      They were moved here from TargaImage.cpp so that file only holds the
//      basic operations. The basic npr-paint is still in TargaImage.cpp.
//
///////////////////////////////////////////////////////////////////////////////

#include "Globals.h"
// The old VC6 loop-scope workaround interferes with modern standard headers.
#ifdef for
#undef for
#endif
#include "TargaImage.h"
#include "ImageUtils.h"
#include <math.h>
#include <vector>
#include <algorithm>
#include <random>

using namespace std;

namespace
{
    struct PaintPoint
    {
        double x, y;
        PaintPoint(double px = 0, double py = 0) : x(px), y(py) {}
    };

    struct CurvedStroke
    {
        vector<PaintPoint> points;
        double color[3];
        double radius, opacity, phase;
    };

    // Blur premultiplied color AND alpha, then normalize by the filtered alpha.
    // Transparent black texels must not create a dark fringe in the paint.
    vector<float> PaintReference(const TargaImage& source, double radius)
    {
        const double sigma = Max(0.5, radius * 0.5);
        const int extent = static_cast<int>(ceil(3.0 * sigma));
        vector<double> kernel(extent * 2 + 1);
        double total = 0.0;
        for (int k = -extent; k <= extent; ++k)
            total += kernel[k + extent] = exp(-k * k / (2.0 * sigma * sigma));
        for (size_t k = 0; k < kernel.size(); ++k) kernel[k] /= total;

        const size_t count = PixelCount(source);
        vector<float> horizontal(count * 4, 0.0f), reference(count * 3);
        for (int y = 0; y < source.height; ++y)
            for (int x = 0; x < source.width; ++x)
            {
                const size_t dst = (static_cast<size_t>(y) * source.width + x) * 4;
                for (int k = -extent; k <= extent; ++k)
                {
                    const size_t src = (static_cast<size_t>(y) * source.width +
                        Reflect(x + k, source.width)) * 4;
                    for (int c = 0; c < 4; ++c)
                        horizontal[dst + c] += static_cast<float>(
                            kernel[k + extent] * source.data[src + c]);
                }
            }
        for (int y = 0; y < source.height; ++y)
            for (int x = 0; x < source.width; ++x)
            {
                double rgba[4] = { 0, 0, 0, 0 };
                for (int k = -extent; k <= extent; ++k)
                {
                    const size_t src = (static_cast<size_t>(Reflect(y + k, source.height)) *
                        source.width + x) * 4;
                    for (int c = 0; c < 4; ++c)
                        rgba[c] += kernel[k + extent] * horizontal[src + c];
                }
                const size_t dst = (static_cast<size_t>(y) * source.width + x) * 3;
                for (int c = 0; c < 3; ++c)
                    reference[dst + c] = static_cast<float>(rgba[3] > 1e-6
                        ? Min(255.0, rgba[c] * 255.0 / rgba[3]) : 240.0);
            }
        return reference;
    }

    vector<PaintPoint> PaintGradient(const vector<float>& reference, int w, int h)
    {
        const size_t count = static_cast<size_t>(w) * h;
        vector<float> luminance(count);
        for (size_t p = 0; p < count; ++p)
            luminance[p] = 0.299f * reference[p * 3] +
                           0.587f * reference[p * 3 + 1] + 0.114f * reference[p * 3 + 2];
        vector<PaintPoint> gradient(count);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                double gx = 0.0, gy = 0.0;
                for (int j = -1; j <= 1; ++j)
                    for (int i = -1; i <= 1; ++i)
                    {
                        const double value = luminance[
                            static_cast<size_t>(Reflect(y + j, h)) * w + Reflect(x + i, w)];
                        gx += i * (j == 0 ? 2 : 1) * value;
                        gy += j * (i == 0 ? 2 : 1) * value;
                    }
                gradient[static_cast<size_t>(y) * w + x] = PaintPoint(gx / 8, gy / 8);
            }
        return gradient;
    }

    PaintPoint SamplePaintGradient(const vector<PaintPoint>& gradient,
                                   int w, int h, const PaintPoint& point)
    {
        const int x = static_cast<int>(point.x), y = static_cast<int>(point.y);
        const int x1 = Min(x + 1, w - 1), y1 = Min(y + 1, h - 1);
        const double tx = point.x - x, ty = point.y - y;
        const PaintPoint a = gradient[static_cast<size_t>(y) * w + x];
        const PaintPoint b = gradient[static_cast<size_t>(y) * w + x1];
        const PaintPoint c = gradient[static_cast<size_t>(y1) * w + x];
        const PaintPoint d = gradient[static_cast<size_t>(y1) * w + x1];
        return PaintPoint((1 - ty) * ((1 - tx) * a.x + tx * b.x) +
                          ty * ((1 - tx) * c.x + tx * d.x),
                          (1 - ty) * ((1 - tx) * a.y + tx * b.y) +
                          ty * ((1 - tx) * c.y + tx * d.y));
    }

    double PaintColorError(const float* a, const double* b)
    {
        double error = 0;
        for (int c = 0; c < 3; ++c) error += (a[c] - b[c]) * (a[c] - b[c]);
        return error;
    }

    CurvedStroke TracePaintStroke(int x, int y, double radius,
                                  const vector<float>& reference,
                                  const vector<float>& canvas,
                                  const vector<PaintPoint>& gradient,
                                  const TargaImage& source, mt19937& randomEngine)
    {
        CurvedStroke stroke;
        stroke.radius = radius;
        stroke.opacity = 0.86;
        const size_t start = (static_cast<size_t>(y) * source.width + x) * 3;
        for (int c = 0; c < 3; ++c) stroke.color[c] = reference[start + c];
        uniform_real_distribution<double> random(0.0, 1.0);
        stroke.phase = random(randomEngine) * 2.0 * c_pi;
        const double flatAngle = -0.65 + (random(randomEngine) - 0.5) * 1.8;
        vector<PaintPoint> arms[2];
        // Follow BOTH directions of the isophote so the seed lies in the
        // interior of a stroke. Sign correction prevents a 180-degree turn.
        for (int arm = 0; arm < 2; ++arm)
        {
            PaintPoint point(x, y), previous;
            for (int step = 0; step < 7; ++step)
            {
                const PaintPoint g = SamplePaintGradient(gradient, source.width, source.height, point);
                double dx = -g.y, dy = g.x;
                double length = sqrt(dx * dx + dy * dy);
                if (length < 0.3)
                {
                    dx = step ? previous.x : cos(flatAngle) * (arm ? 1 : -1);
                    dy = step ? previous.y : sin(flatAngle) * (arm ? 1 : -1);
                }
                else
                {
                    dx /= length; dy /= length;
                    if ((!step && arm == 0) ||
                        (step && dx * previous.x + dy * previous.y < 0))
                    { dx = -dx; dy = -dy; }
                }
                if (step)
                {
                    dx = 0.65 * dx + 0.35 * previous.x;
                    dy = 0.65 * dy + 0.35 * previous.y;
                }
                length = sqrt(dx * dx + dy * dy);
                if (length < 1e-6) break;
                dx /= length; dy /= length;
                PaintPoint next(point.x + radius * dx, point.y + radius * dy);
                if (next.x < 0 || next.y < 0 || next.x > source.width - 1 ||
                    next.y > source.height - 1) break;
                const size_t p = static_cast<size_t>(static_cast<int>(next.y + 0.5)) *
                    source.width + static_cast<int>(next.x + 0.5);
                if (!source.data[p * 4 + 3]) break;
                const double strokeError = PaintColorError(&reference[p * 3], stroke.color);
                double canvasColor[3] = { canvas[p * 3], canvas[p * 3 + 1], canvas[p * 3 + 2] };
                const double canvasError = PaintColorError(&reference[p * 3], canvasColor);
                // Always stop at a strong color boundary. After two control
                // points, stop if the existing painting already fits better.
                if (strokeError > 85.0 * 85.0 ||
                    (step >= 2 && strokeError > Max(20.0 * 20.0, canvasError))) break;
                arms[arm].push_back(next);
                point = next;
                previous = PaintPoint(dx, dy);
            }
        }
        stroke.points.assign(arms[0].rbegin(), arms[0].rend());
        stroke.points.push_back(PaintPoint(x, y));
        stroke.points.insert(stroke.points.end(), arms[1].begin(), arms[1].end());
        const double luminance = 0.299 * stroke.color[0] + 0.587 * stroke.color[1] + 0.114 * stroke.color[2];
        const double valueJitter = (random(randomEngine) - 0.5) * 10.0;
        for (int c = 0; c < 3; ++c)
            stroke.color[c] = Max(0.0, Min(255.0, luminance +
                1.04 * (stroke.color[c] - luminance) + valueJitter + (random(randomEngine) - 0.5) * 3.0));
        return stroke;
    }

    vector<PaintPoint> PaintSpline(const vector<PaintPoint>& points)
    {
        if (points.size() < 2) return points;
        // Repeated endpoints make the uniform cubic B-spline reach both tips.
        vector<PaintPoint> padded(2, points.front()), result;
        padded.insert(padded.end(), points.begin(), points.end());
        padded.push_back(points.back()); padded.push_back(points.back());
        for (size_t i = 0; i + 3 < padded.size(); ++i)
            for (int sample = 0; sample < 4; ++sample)
            {
                const double t = sample / 4.0, t2 = t * t, t3 = t2 * t;
                const double b[4] = { (1 - 3*t + 3*t2 - t3) / 6.0,
                    (4 - 6*t2 + 3*t3) / 6.0, (1 + 3*t + 3*t2 - 3*t3) / 6.0, t3 / 6.0 };
                PaintPoint point;
                for (int k = 0; k < 4; ++k)
                { point.x += b[k] * padded[i+k].x; point.y += b[k] * padded[i+k].y; }
                result.push_back(point);
            }
        result.push_back(points.back());
        return result;
    }

    void DrawPaintStroke(vector<float>& canvas, int w, int h, const CurvedStroke& stroke)
    {
        vector<PaintPoint> path = PaintSpline(stroke.points);
        if (path.size() == 1) path.push_back(path.front());
        double minX = path[0].x, minY = path[0].y, maxX = minX, maxY = minY;
        for (size_t i = 1; i < path.size(); ++i)
        {
            minX = Min(minX, path[i].x); minY = Min(minY, path[i].y);
            maxX = Max(maxX, path[i].x); maxY = Max(maxY, path[i].y);
        }
        const int left = Max(0, static_cast<int>(floor(minX - stroke.radius - 1)));
        const int top = Max(0, static_cast<int>(floor(minY - stroke.radius - 1)));
        const int right = Min(w - 1, static_cast<int>(ceil(maxX + stroke.radius + 1)));
        const int bottom = Min(h - 1, static_cast<int>(ceil(maxY + stroke.radius + 1)));
        const int tileWidth = right - left + 1;
        const size_t tileCount = static_cast<size_t>(tileWidth) * (bottom - top + 1);
        vector<float> coverage(tileCount, 0.0f), pigment(tileCount, 1.0f);
        for (size_t i = 1; i < path.size(); ++i)
        {
            const PaintPoint a = path[i - 1], b = path[i];
            const double vx = b.x - a.x, vy = b.y - a.y;
            const double length2 = vx * vx + vy * vy, length = sqrt(length2);
            const double progress = (i - 0.5) / (path.size() - 1);
            const double radius = stroke.radius * (0.65 + 0.35 * sin(c_pi * progress));
            const int x0 = Max(left, static_cast<int>(floor(Min(a.x, b.x) - radius - 1)));
            const int x1 = Min(right, static_cast<int>(ceil(Max(a.x, b.x) + radius + 1)));
            const int y0 = Max(top, static_cast<int>(floor(Min(a.y, b.y) - radius - 1)));
            const int y1 = Min(bottom, static_cast<int>(ceil(Max(a.y, b.y) + radius + 1)));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    const double t = length2 > 1e-8
                        ? Max(0.0, Min(1.0, ((x - a.x) * vx + (y - a.y) * vy) / length2)) : 0.0;
                    const double dx = x - a.x - t * vx, dy = y - a.y - t * vy;
                    const double edge = Max(0.0, Min(1.0, radius + 0.5 - sqrt(dx * dx + dy * dy)));
                    if (edge <= 0.0) continue;
                    const double cross = length > 1e-4 ? (-vy * dx + vx * dy) / length : dx;
                    // Longitudinal bristle tracks and a soft, tapered footprint.
                    const double bristle = sin(cross * 2.2 + stroke.phase);
                    const float amount = static_cast<float>(edge * stroke.opacity * (0.90 + 0.10 * bristle));
                    const size_t tile = static_cast<size_t>(y - top) * tileWidth + x - left;
                    // Union the whole footprint before blending. Overlapping
                    // segments of ONE stroke must not accumulate opacity.
                    if (amount > coverage[tile])
                    {
                        coverage[tile] = amount;
                        pigment[tile] = static_cast<float>(1.0 + 0.035 * bristle);
                    }
                }
        }
        for (int y = top; y <= bottom; ++y)
            for (int x = left; x <= right; ++x)
            {
                const size_t tile = static_cast<size_t>(y - top) * tileWidth + x - left;
                if (coverage[tile] == 0.0f) continue;
                const size_t p = (static_cast<size_t>(y) * w + x) * 3;
                for (int c = 0; c < 3; ++c)
                    canvas[p + c] = static_cast<float>(canvas[p + c] * (1 - coverage[tile]) +
                        Min(255.0, stroke.color[c] * pigment[tile]) * coverage[tile]);
            }
    }

    double SmoothPaintStep(double low, double high, double value)
    {
        const double t = Max(0.0, Min(1.0, (value - low) / (high - low)));
        return t * t * (3.0 - 2.0 * t);
    }

    double PaintNoise(int x, int y, unsigned int seed)
    {
        unsigned int hash = static_cast<unsigned int>(x) * 374761393u +
            static_cast<unsigned int>(y) * 668265263u + seed;
        hash = (hash ^ (hash >> 13)) * 1274126177u;
        hash ^= hash >> 16;
        return (hash & 65535u) / 65535.0;
    }

    double PaintValueNoise(double x, double y, unsigned int seed)
    {
        const int ix = static_cast<int>(floor(x)), iy = static_cast<int>(floor(y));
        const double tx = SmoothPaintStep(0.0, 1.0, x - ix);
        const double ty = SmoothPaintStep(0.0, 1.0, y - iy);
        return (1 - ty) * ((1 - tx) * PaintNoise(ix, iy, seed) + tx * PaintNoise(ix + 1, iy, seed)) +
            ty * ((1 - tx) * PaintNoise(ix, iy + 1, seed) + tx * PaintNoise(ix + 1, iy + 1, seed));
    }

    vector<float> BilateralPaint(const TargaImage& source, int radius, int passes)
    {
        const size_t count = PixelCount(source);
        vector<float> work(count * 3), next(count * 3);
        for (size_t p = 0; p < count; ++p)
            for (int c = 0; c < 3; ++c)
                work[p * 3 + c] = source.data[p * 4 + 3]
                    ? Min(255.0f, source.data[p * 4 + c] * 255.0f / source.data[p * 4 + 3]) : 0.0f;
        const int size = radius * 2 + 1;
        vector<double> spatial(size * size);
        for (int y = -radius; y <= radius; ++y)
            for (int x = -radius; x <= radius; ++x)
                spatial[(y + radius) * size + x + radius] =
                    exp(-(x * x + y * y) / (2.0 * radius * radius));
        // Look up the range weight instead of evaluating an exponential at
        // every neighbor of every pixel. RGB distance never exceeds 3*255^2.
        vector<float> range(3 * 255 * 255 + 1);
        for (size_t i = 0; i < range.size(); ++i)
            range[i] = static_cast<float>(exp(-static_cast<double>(i) / (2.0 * 40.0 * 40.0 * 3.0)));
        for (int pass = 0; pass < passes; ++pass)
        {
            for (int y = 0; y < source.height; ++y)
                for (int x = 0; x < source.width; ++x)
                {
                    const size_t p = static_cast<size_t>(y) * source.width + x;
                    if (!source.data[p * 4 + 3])
                    {
                        for (int c = 0; c < 3; ++c) next[p * 3 + c] = 0.0f;
                        continue;
                    }
                    double sum[3] = { 0, 0, 0 }, total = 0.0;
                    for (int j = -radius; j <= radius; ++j)
                        for (int i = -radius; i <= radius; ++i)
                        {
                            const size_t q = static_cast<size_t>(Reflect(y + j, source.height)) *
                                source.width + Reflect(x + i, source.width);
                            double distance = 0;
                            for (int c = 0; c < 3; ++c)
                            {
                                const double delta = work[p * 3 + c] - work[q * 3 + c];
                                distance += delta * delta;
                            }
                            const int bin = Min(static_cast<int>(range.size()) - 1, static_cast<int>(distance));
                            const double weight = spatial[(j + radius) * size + i + radius] *
                                range[bin] * source.data[q * 4 + 3];
                            total += weight;
                            for (int c = 0; c < 3; ++c) sum[c] += work[q * 3 + c] * weight;
                        }
                    for (int c = 0; c < 3; ++c) next[p * 3 + c] = static_cast<float>(sum[c] / total);
                }
            work.swap(next);
        }
        return work;
    }

    // Quantize HSV rather than R/G/B separately to keep the dominant hue of
    // each material. Saturation, value bands and cool shadows form a cel look.
    void CelPaintColor(const float* rgb, double strength, double* result)
    {
        const double r = rgb[0] / 255.0, g = rgb[1] / 255.0, b = rgb[2] / 255.0;
        const double maximum = Max(r, Max(g, b)), minimum = Min(r, Min(g, b));
        const double delta = maximum - minimum;
        double hue = 0.0;
        if (delta > 1e-6)
        {
            if (maximum == r) hue = (g - b) / delta;
            else if (maximum == g) hue = 2.0 + (b - r) / delta;
            else hue = 4.0 + (r - g) / delta;
            if (hue < 0) hue += 6.0;
        }
        const double hueBins = 36.0;
        hue = fmod(floor(hue * hueBins / 6.0 + 0.5) * 6.0 / hueBins, 6.0);
        double saturation = maximum > 1e-6 ? delta / maximum : 0.0;
        saturation = Min(1.0, floor(saturation * 1.12 * 8.0 + 0.5) / 8.0);
        const int bands = Max(3, Min(8, static_cast<int>(6.0 / sqrt(strength) + 0.5)));
        const double value = floor(maximum * (bands - 1) + 0.5) / (bands - 1);
        const double chroma = value * saturation;
        const double second = chroma * (1 - fabs(fmod(hue, 2.0) - 1));
        double color[3] = { 0, 0, 0 };
        if (hue < 1) { color[0] = chroma; color[1] = second; }
        else if (hue < 2) { color[0] = second; color[1] = chroma; }
        else if (hue < 3) { color[1] = chroma; color[2] = second; }
        else if (hue < 4) { color[1] = second; color[2] = chroma; }
        else if (hue < 5) { color[0] = second; color[2] = chroma; }
        else { color[0] = chroma; color[2] = second; }
        const double shadow = 1.0 - SmoothPaintStep(0.05, 0.6, value);
        const double tint[3] = { 3.0, 5.0, 11.0 };
        for (int c = 0; c < 3; ++c)
            result[c] = Max(0.0, Min(255.0, (color[c] + value - chroma) * 255.0 + tint[c] * shadow));
    }

    const double waterPaper[3] = { 244.0, 237.0, 219.0 };

    void WaterPigment(const double* rgb, double* density)
    {
        const double luminance = (0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2]) / 255.0;
        const double shadow = 1.0 - SmoothPaintStep(0.10, 0.65, luminance);
        const double cool[3] = { -3.0, 1.0, 6.0 };
        for (int c = 0; c < 3; ++c)
        {
            const double color = rgb[c] * 0.86 + waterPaper[c] * 0.14 + cool[c] * shadow;
            density[c] = -log(Max(0.02, Min(1.0, color / waterPaper[c])));
        }
    }

    void DrawWatercolorStroke(vector<float>& density, int w, int h,
                              const CurvedStroke& stroke, const vector<float>& wetness,
                              const vector<float>& granulation)
    {
        vector<PaintPoint> path = PaintSpline(stroke.points);
        if (path.size() == 1) path.push_back(path.front());
        double minX = path[0].x, minY = path[0].y, maxX = minX, maxY = minY;
        for (size_t i = 1; i < path.size(); ++i)
        {
            minX = Min(minX, path[i].x); minY = Min(minY, path[i].y);
            maxX = Max(maxX, path[i].x); maxY = Max(maxY, path[i].y);
        }
        const double margin = stroke.radius * 1.5 + 1;
        const int left = Max(0, static_cast<int>(floor(minX - margin)));
        const int top = Max(0, static_cast<int>(floor(minY - margin)));
        const int right = Min(w - 1, static_cast<int>(ceil(maxX + margin)));
        const int bottom = Min(h - 1, static_cast<int>(ceil(maxY + margin)));
        const int tileWidth = right - left + 1;
        vector<float> distance(static_cast<size_t>(tileWidth) * (bottom - top + 1), 2.0f);
        for (size_t i = 1; i < path.size(); ++i)
        {
            const PaintPoint a = path[i - 1], b = path[i];
            const double vx = b.x - a.x, vy = b.y - a.y, length2 = vx * vx + vy * vy;
            const double progress = (i - 0.5) / (path.size() - 1);
            const double radius = stroke.radius * (0.75 + 0.25 * sin(c_pi * progress));
            const int x0 = Max(left, static_cast<int>(floor(Min(a.x, b.x) - margin)));
            const int x1 = Min(right, static_cast<int>(ceil(Max(a.x, b.x) + margin)));
            const int y0 = Max(top, static_cast<int>(floor(Min(a.y, b.y) - margin)));
            const int y1 = Min(bottom, static_cast<int>(ceil(Max(a.y, b.y) + margin)));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    const size_t p = static_cast<size_t>(y) * w + x;
                    const double t = length2 > 1e-8
                        ? Max(0.0, Min(1.0, ((x - a.x) * vx + (y - a.y) * vy) / length2)) : 0.0;
                    const double dx = x - a.x - t * vx, dy = y - a.y - t * vy;
                    const float d = static_cast<float>(sqrt(dx * dx + dy * dy) /
                        (radius * (0.8 + wetness[p] * 0.4)));
                    const size_t q = static_cast<size_t>(y - top) * tileWidth + x - left;
                    distance[q] = Min(distance[q], d);
                }
        }
        double pigment[3];
        for (int c = 0; c < 3; ++c)
            pigment[c] = -log(Max(0.02, Min(1.0, stroke.color[c] / waterPaper[c])));
        for (int y = top; y <= bottom; ++y)
            for (int x = left; x <= right; ++x)
            {
                const size_t q = static_cast<size_t>(y - top) * tileWidth + x - left;
                const double d = distance[q];
                if (d >= 1.15) continue;
                const size_t p = static_cast<size_t>(y) * w + x;
                const double coverage = 1.0 - SmoothPaintStep(0.55, 1.15, d);
                const double rim = exp(-((d - 0.84) * (d - 0.84)) / 0.018);
                const double amount = coverage * stroke.opacity * (0.78 + 0.22 * granulation[p]);
                const double deposit = 0.88 + 0.20 * granulation[p] + 0.30 * rim;
                for (int c = 0; c < 3; ++c)
                    density[p * 3 + c] = static_cast<float>(density[p * 3 + c] * (1 - amount) +
                        pigment[c] * deposit * amount);
            }
    }
}


///////////////////////////////////////////////////////////////////////////////
//
//      Advanced painterly rendering with curved, textured B-spline strokes.
//      Based on Hertzmann (SIGGRAPH 1998), section 2.2, with bidirectional
//      tracing, tapered bristles and alpha-normalized reference images.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::NPR_Paint_Advanced(float brushScale, unsigned int seed)
{
    if (!ValidImage(*this) || !std::isfinite(brushScale) ||
        brushScale < 0.5f || brushScale > 3.0f) return false;

    const size_t count = PixelCount(*this);
    const double baseRadius = Max(2.0, Min(32.0,
        Min(width, height) * static_cast<double>(brushScale) / 58.0));
    const double radii[3] = { baseRadius, Max(1.0, baseRadius * 0.5), Max(0.75, baseRadius * 0.25) };
    const double thresholds[3] = { 0.0, 28.0, 20.0 };
    vector<float> canvas(count * 3), difference(count);
    mt19937 randomEngine(seed);

    for (int layer = 0; layer < 3; ++layer)
    {
        const double radius = radii[layer];
        const vector<float> reference = PaintReference(*this, radius);
        const vector<PaintPoint> gradient = PaintGradient(reference, width, height);
        if (layer == 0)
        {
            // A lightly tinted underpainting guarantees complete coverage,
            // including the image border and between semi-transparent strokes.
            const double paper[3] = { 242.0, 238.0, 228.0 };
            for (size_t p = 0; p < count; ++p)
                for (int c = 0; c < 3; ++c)
                    canvas[p * 3 + c] = static_cast<float>(0.96 * reference[p * 3 + c] + 0.04 * paper[c]);
        }
        for (size_t p = 0; p < count; ++p)
        {
            double color[3] = { canvas[p * 3], canvas[p * 3 + 1], canvas[p * 3 + 2] };
            difference[p] = static_cast<float>(sqrt(PaintColorError(&reference[p * 3], color)));
        }

        const int grid = Max(1, static_cast<int>(radius + 0.5));
        vector<CurvedStroke> strokes;
        for (int top = 0; top < height; top += grid)
            for (int left = 0; left < width; left += grid)
            {
                const int bottom = Min(height, top + grid), right = Min(width, left + grid);
                int bestX = left, bestY = top;
                double sumError = 0.0, sumAlpha = 0.0, bestError = -1.0;
                for (int y = top; y < bottom; ++y)
                    for (int x = left; x < right; ++x)
                    {
                        const size_t p = static_cast<size_t>(y) * width + x;
                        const double alpha = data[p * 4 + 3] / 255.0;
                        if (alpha == 0.0) continue;
                        sumAlpha += alpha;
                        sumError += difference[p] * alpha;
                        // Coarse seeds prefer the cell center; detail layers
                        // concentrate new strokes at the greatest color error.
                        const double error = layer == 0
                            ? -fabs(x - (left + right - 1) * 0.5) - fabs(y - (top + bottom - 1) * 0.5)
                            : difference[p] * alpha;
                        if (sumAlpha == alpha || error > bestError)
                        { bestError = error; bestX = x; bestY = y; }
                    }
                if (sumAlpha > 0.0 && (layer == 0 || sumError / sumAlpha > thresholds[layer]))
                    strokes.push_back(TracePaintStroke(bestX, bestY, radius,
                        reference, canvas, gradient, *this, randomEngine));
            }

        // Plan against a frozen canvas, then shuffle to avoid a scanline bias.
        shuffle(strokes.begin(), strokes.end(), randomEngine);
        for (size_t i = 0; i < strokes.size(); ++i)
            DrawPaintStroke(canvas, width, height, strokes[i]);
    }

    // Fine canvas grain is tied to coordinates and the seed, not wall-clock
    // time. All calculations finish before replacing the original pixels.
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            const size_t p = static_cast<size_t>(y) * width + x;
            unsigned int hash = static_cast<unsigned int>(x) * 374761393u +
                static_cast<unsigned int>(y) * 668265263u + seed;
            hash = (hash ^ (hash >> 13)) * 1274126177u;
            hash ^= hash >> 16;
            const double grain = 1.0 + (static_cast<double>(hash & 255u) / 255.0 - 0.5) * 0.018;
            const double alpha = data[p * 4 + 3] / 255.0;
            for (int c = 0; c < 3; ++c)
                data[p * 4 + c] = ClampByte(Min(255.0, canvas[p * 3 + c] * grain) * alpha + 0.5);
        }
    return true;
}


///////////////////////////////////////////////////////////////////////////////
//
//      Cel-style abstraction: bilateral smoothing, HSV bands and ink edges.
//      Visual direction inspired by Guilty Gear -Strive-, not its 3-D shader.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::NPR_Cartoon(float strength)
{
    if (!ValidImage(*this) || !std::isfinite(strength) || strength < 0.5f || strength > 3.0f)
        return false;
    const int radius = Max(2, Min(5, static_cast<int>(2.0 + strength)));
    const vector<float> smooth = BilateralPaint(*this, radius, 3);
    vector<PaintPoint> gradient = PaintGradient(smooth, width, height);
    const size_t count = PixelCount(*this);
    vector<float> magnitude(count), ink(count, 0.0f);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            const size_t p = static_cast<size_t>(y) * width + x;
            double largest = gradient[p].x * gradient[p].x + gradient[p].y * gradient[p].y;
            // A red/green boundary can have equal luminance. Include chromatic
            // gradients so such material boundaries still receive an outline.
            for (int c = 0; c < 3; ++c)
            {
                const double gx = 0.275 * (smooth[(static_cast<size_t>(y) * width + Reflect(x + 1, width)) * 3 + c] -
                    smooth[(static_cast<size_t>(y) * width + Reflect(x - 1, width)) * 3 + c]);
                const double gy = 0.275 * (smooth[(static_cast<size_t>(Reflect(y + 1, height)) * width + x) * 3 + c] -
                    smooth[(static_cast<size_t>(Reflect(y - 1, height)) * width + x) * 3 + c]);
                if (gx * gx + gy * gy > largest)
                { largest = gx * gx + gy * gy; gradient[p] = PaintPoint(gx, gy); }
            }
            magnitude[p] = static_cast<float>(sqrt(largest));
        }
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            const size_t p = static_cast<size_t>(y) * width + x;
            const double length = magnitude[p];
            if (length < 1e-5) continue;
            const int dx = static_cast<int>(round(gradient[p].x / length));
            const int dy = static_cast<int>(round(gradient[p].y / length));
            const size_t a = static_cast<size_t>(Reflect(y + dy, height)) * width + Reflect(x + dx, width);
            const size_t b = static_cast<size_t>(Reflect(y - dy, height)) * width + Reflect(x - dx, width);
            // Nonmaximum suppression makes a thin centerline before widening.
            if (length >= magnitude[a] && length >= magnitude[b])
                ink[p] = static_cast<float>(SmoothPaintStep(6.0 / sqrt(strength), 22.0 / sqrt(strength), length));
        }
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            const size_t p = static_cast<size_t>(y) * width + x;
            double edge = ink[p];
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i)
                {
                    const size_t q = static_cast<size_t>(Reflect(y + j, height)) * width + Reflect(x + i, width);
                    const double weight = (i && j ? 0.35 : 0.65) * Min(1.3, sqrt(static_cast<double>(strength)));
                    edge = Max(edge, ink[q] * weight);
                }
            double color[3];
            CelPaintColor(&smooth[p * 3], strength, color);
            const double inkColor[3] = { 9.0, 10.0, 18.0 };
            const double alpha = data[p * 4 + 3] / 255.0;
            for (int c = 0; c < 3; ++c)
                data[p * 4 + c] = ClampByte((color[c] * (1 - edge) + inkColor[c] * edge) * alpha + 0.5);
        }
    return true;
}


///////////////////////////////////////////////////////////////////////////////
//
//      Expressive watercolor: irregular washes in optical-density space,
//      pigment at wet edges, paper granulation and selective contour accents.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::NPR_Watercolor(float brushScale, unsigned int seed)
{
    if (!ValidImage(*this) || !std::isfinite(brushScale) ||
        brushScale < 0.5f || brushScale > 3.0f) return false;
    const size_t count = PixelCount(*this);
    const double base = Max(2.0, Min(24.0,
        Min(width, height) * static_cast<double>(brushScale) / 60.0));
    const double radii[3] = { base * 2.0, base, Max(0.8, base * 0.35) };
    const double thresholds[3] = { 0.0, 18.0, 12.0 };
    vector<float> wetness(count), granulation(count), accents(count);
    vector<float> density(count * 3), canvas(count * 3), difference(count);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            const size_t p = static_cast<size_t>(y) * width + x;
            wetness[p] = static_cast<float>(0.65 * PaintValueNoise(x / (base * 2.3), y / (base * 2.3), seed) +
                0.35 * PaintValueNoise(x / (base * 0.65), y / (base * 0.65), seed ^ 0x9e3779b9u));
            granulation[p] = static_cast<float>(0.6 * PaintNoise(x, y, seed ^ 0x85ebca6bu) +
                0.4 * PaintValueNoise(x / 2.8, y / 2.8, seed ^ 0xc2b2ae35u));
        }
    mt19937 randomEngine(seed);
    for (int layer = 0; layer < 3; ++layer)
    {
        vector<float> reference = PaintReference(*this, radii[layer]);
        for (size_t p = 0; p < count; ++p)
        {
            double rgb[3] = { reference[p * 3], reference[p * 3 + 1], reference[p * 3 + 2] }, pigment[3];
            WaterPigment(rgb, pigment);
            for (int c = 0; c < 3; ++c)
            {
                reference[p * 3 + c] = static_cast<float>(waterPaper[c] * exp(-pigment[c]));
                if (layer == 0) density[p * 3 + c] = static_cast<float>(pigment[c] * 0.85);
                canvas[p * 3 + c] = static_cast<float>(waterPaper[c] * exp(-density[p * 3 + c]));
            }
            double current[3] = { canvas[p * 3], canvas[p * 3 + 1], canvas[p * 3 + 2] };
            difference[p] = static_cast<float>(sqrt(PaintColorError(&reference[p * 3], current)));
        }
        const vector<PaintPoint> gradient = PaintGradient(reference, width, height);
        if (layer == 2)
            for (size_t p = 0; p < count; ++p)
                accents[p] = static_cast<float>(SmoothPaintStep(4.0, 18.0,
                    sqrt(gradient[p].x * gradient[p].x + gradient[p].y * gradient[p].y)));
        const int grid = Max(2, static_cast<int>(radii[layer] * 1.25 + 0.5));
        vector<CurvedStroke> strokes;
        for (int top = 0; top < height; top += grid)
            for (int left = 0; left < width; left += grid)
            {
                const int bottom = Min(height, top + grid), right = Min(width, left + grid);
                double error = 0, total = 0, best = -1;
                int bestX = left, bestY = top;
                for (int y = top; y < bottom; ++y)
                    for (int x = left; x < right; ++x)
                    {
                        const size_t p = static_cast<size_t>(y) * width + x;
                        const double alpha = data[p * 4 + 3] / 255.0;
                        if (alpha == 0) continue;
                        total += alpha;
                        error += difference[p] * alpha;
                        const double candidate = layer == 0
                            ? wetness[p] : difference[p] * alpha;
                        if (candidate > best) { best = candidate; bestX = x; bestY = y; }
                    }
                if (total > 0 && (layer == 0 || error / total > thresholds[layer]))
                {
                    CurvedStroke stroke = TracePaintStroke(bestX, bestY, radii[layer],
                        reference, canvas, gradient, *this, randomEngine);
                    stroke.opacity = layer == 0 ? 0.50 : 0.42;
                    strokes.push_back(stroke);
                }
            }
        shuffle(strokes.begin(), strokes.end(), randomEngine);
        for (size_t i = 0; i < strokes.size(); ++i)
            DrawWatercolorStroke(density, width, height, strokes[i], wetness, granulation);
    }
    for (size_t p = 0; p < count; ++p)
    {
        const double deposit = 0.93 + 0.14 * granulation[p] + 0.16 * (wetness[p] - 0.5);
        const double paperGrain = 0.985 + 0.03 * granulation[p];
        for (int c = 0; c < 3; ++c)
        {
            const double pigment = density[p * 3 + c] * deposit +
                0.10 * accents[p] * (0.4 + wetness[p] * 0.6);
            canvas[p * 3 + c] = static_cast<float>(Min(255.0, waterPaper[c] * paperGrain * exp(-pigment)));
        }
    }

    // Selective dry-brush accents recover features after the broad wet washes.
    // Only high-error, directional areas receive these thin broken strokes.
    vector<float> detail = PaintReference(*this, Max(0.8, base * 0.22));
    for (size_t p = 0; p < count; ++p)
    {
        double color[3] = { detail[p * 3], detail[p * 3 + 1], detail[p * 3 + 2] }, pigment[3];
        WaterPigment(color, pigment);
        for (int c = 0; c < 3; ++c)
            detail[p * 3 + c] = static_cast<float>(waterPaper[c] * exp(-pigment[c]));
    }
    const vector<PaintPoint> detailGradient = PaintGradient(detail, width, height);
    const int detailGrid = Max(3, static_cast<int>(base * 0.85));
    vector<CurvedStroke> dryStrokes;
    for (int top = 0; top < height; top += detailGrid)
        for (int left = 0; left < width; left += detailGrid)
        {
            double bestError = 16.0 * 16.0;
            int bestX = -1, bestY = -1;
            for (int y = top; y < Min(height, top + detailGrid); ++y)
                for (int x = left; x < Min(width, left + detailGrid); ++x)
                {
                    const size_t p = static_cast<size_t>(y) * width + x;
                    if (!data[p * 4 + 3] ||
                        fabs(detailGradient[p].x) + fabs(detailGradient[p].y) < 2.0) continue;
                    double color[3] = { canvas[p * 3], canvas[p * 3 + 1], canvas[p * 3 + 2] };
                    const double error = PaintColorError(&detail[p * 3], color);
                    if (error > bestError) { bestError = error; bestX = x; bestY = y; }
                }
            if (bestX >= 0)
            {
                CurvedStroke stroke = TracePaintStroke(bestX, bestY, Max(0.75, base * 0.23),
                    detail, canvas, detailGradient, *this, randomEngine);
                stroke.opacity = 0.38;
                dryStrokes.push_back(stroke);
            }
        }
    shuffle(dryStrokes.begin(), dryStrokes.end(), randomEngine);
    for (size_t i = 0; i < dryStrokes.size(); ++i)
        DrawPaintStroke(canvas, width, height, dryStrokes[i]);
    for (size_t p = 0; p < count; ++p)
    {
        const double alpha = data[p * 4 + 3] / 255.0;
        for (int c = 0; c < 3; ++c)
            data[p * 4 + c] = ClampByte(Min(255.0, static_cast<double>(canvas[p * 3 + c])) * alpha + 0.5);
    }
    return true;
}
