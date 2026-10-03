///////////////////////////////////////////////////////////////////////////////
//
//      TargaImage.cpp                          Author:     Stephen Chenney
//                                              Modified:   Eric McDaniel
//                                              Date:       Fall 2004
//
//      Implementation of TargaImage methods.  You must implement the image
//  modification functions.
//
///////////////////////////////////////////////////////////////////////////////

#include "Globals.h"
// The old VC6 loop-scope workaround interferes with modern standard headers.
#ifdef for
#undef for
#endif
#include "TargaImage.h"
#include "libtarga.h"
#include <stdlib.h>
#include <assert.h>
#include <memory.h>
#include <math.h>
#include <iostream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <climits>
#include <random>
#include <cctype>
#include <memory>
#include <string>

// Keep stb's implementation in this translation unit only. TGA still uses
// LibTarga so the assignment's existing file format behavior is preserved.
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_FAILURE_USERMSG
#define STBI_WINDOWS_UTF8
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
#define STBIW_WINDOWS_UTF8
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

using namespace std;

// constants
const int           RED             = 0;                // red channel
const int           GREEN           = 1;                // green channel
const int           BLUE            = 2;                // blue channel
const unsigned char BACKGROUND[3]   = { 0, 0, 0 };      // background color

namespace
{
    enum ImageFormat { IMAGE_TGA, IMAGE_PNG, IMAGE_JPEG, IMAGE_UNSUPPORTED };

    ImageFormat FormatFromFilename(const char* filename)
    {
        const string path(filename);
        const size_t dot = path.find_last_of('.');
        const size_t separator = path.find_last_of("/\\");
        // Existing scripts may omit a suffix; continue treating those as TGA.
        if (dot == string::npos || (separator != string::npos && dot < separator))
            return IMAGE_TGA;
        string extension = path.substr(dot);
        for (size_t i = 0; i < extension.size(); ++i)
            extension[i] = static_cast<char>(tolower(static_cast<unsigned char>(extension[i])));
        if (extension == ".tga") return IMAGE_TGA;
        if (extension == ".png") return IMAGE_PNG;
        if (extension == ".jpg" || extension == ".jpeg") return IMAGE_JPEG;
        return IMAGE_UNSUPPORTED;
    }

    bool ValidImage(const TargaImage& image)
    {
        return image.data && image.width > 0 && image.height > 0 &&
            static_cast<size_t>(image.width) * image.height <= INT_MAX / 4;
    }

    size_t PixelCount(const TargaImage& image)
    {
        return static_cast<size_t>(image.width) * image.height;
    }

    unsigned char ClampByte(double value)
    {
        if (value <= 0.0) return 0;
        if (value >= 255.0) return 255;
        // Suppress roundoff such as 99.99999999999999 on a constant image.
        return static_cast<unsigned char>(value + 1e-9);
    }

    double Luminance(const unsigned char* pixel)
    {
        return 0.299 * pixel[0] + 0.587 * pixel[1] + 0.114 * pixel[2];
    }

    // Gray level 0~255 of a pixel (the same value To_Grayscale writes).
    unsigned char GrayValue(const unsigned char* pixel)
    {
        return static_cast<unsigned char>(Luminance(pixel));
    }

    void SetGray(unsigned char* pixel, unsigned char value)
    {
        pixel[0] = pixel[1] = pixel[2] = value;
    }

    // Reflect about pixel centers at the boundary; also works for 1-pixel axes.
    int Reflect(long long coordinate, int length)
    {
        if (length <= 1) return 0;
        const long long period = 2LL * (length - 1);
        coordinate %= period;
        if (coordinate < 0) coordinate += period;
        return static_cast<int>(coordinate < length ? coordinate : period - coordinate);
    }

    // Build normalized binomial coefficients outward from the middle. Starting
    // at the largest coefficient avoids overflow for large Gaussian kernels.
    vector<double> GaussianKernel(unsigned int size)
    {
        vector<double> kernel(size, 0.0);
        const unsigned int middle = size / 2;
        kernel[middle] = 1.0;
        for (unsigned int i = middle; i > 0; --i)
            kernel[i - 1] = kernel[i] * i / (size - i);
        for (unsigned int i = 0; i < middle; ++i)
            kernel[size - 1 - i] = kernel[i];
        double sum = 0.0;
        for (size_t i = 0; i < kernel.size(); ++i) sum += kernel[i];
        for (size_t i = 0; i < kernel.size(); ++i) kernel[i] /= sum;
        return kernel;
    }

    enum FilterMode { BLUR, HIGH_PASS, SHARPEN };

    // Filter the RGB channels with kernel x kernel (alpha is left unchanged).
    // The 2-D mask is the outer product of the 1-D kernel, so we can filter
    // each row first and then each column. Pixels outside the image are
    // taken from the mirrored position (see Reflect).
    bool FilterRGB(TargaImage& image, const vector<double>& kernel,
                   FilterMode mode = BLUR)
    {
        if (!ValidImage(image) || kernel.empty() || kernel.size() % 2 == 0)
            return false;

        const int w = image.width, h = image.height;
        const int radius = static_cast<int>(kernel.size() / 2);

        // Pass 1: horizontal filter, result kept as double (3 values per pixel).
        vector<double> rows(PixelCount(image) * 3, 0.0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int k = -radius; k <= radius; ++k)
                {
                    const int src = (y * w + Reflect(x + k, w)) * 4;
                    const int dst = (y * w + x) * 3;
                    for (int c = 0; c < 3; ++c)
                        rows[dst + c] += kernel[k + radius] * image.data[src + c];
                }

        // Pass 2: vertical filter on the result of pass 1.
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                double blurred[3] = { 0.0, 0.0, 0.0 };
                for (int k = -radius; k <= radius; ++k)
                {
                    const int src = (Reflect(y + k, h) * w + x) * 3;
                    for (int c = 0; c < 3; ++c)
                        blurred[c] += kernel[k + radius] * rows[src + c];
                }

                unsigned char* pixel = image.data + (y * w + x) * 4;
                for (int c = 0; c < 3; ++c)
                {
                    double value = blurred[c];                               // low pass
                    if (mode == HIGH_PASS) value = pixel[c] - value;         // original - low pass
                    if (mode == SHARPEN) value = 2.0 * pixel[c] - value;     // original + high pass
                    pixel[c] = ClampByte(value);
                }
            }
        return true;
    }

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

    // Euclidean RGB distance between two pixels.
    double ColorDistance(const unsigned char* a, const unsigned char* b)
    {
        double squaredDistance = 0.0;
        for (int c = 0; c < 3; ++c)
        {
            const double delta = a[c] - b[c];
            squaredDistance += delta * delta;
        }
        return sqrt(squaredDistance);
    }

    // NPR_Paint helper: look at the cell of pixels within "half" of (x, y).
    // Returns the average error in the cell, and the pixel with the largest
    // error through bestX / bestY.
    double GridCellError(const vector<double>& difference, int w, int h,
                         int x, int y, int half, int& bestX, int& bestY)
    {
        double sum = 0.0;
        int samples = 0;
        bestX = x;
        bestY = y;
        double largestError = difference[y * w + x];
        for (int sy = Max(0, y - half); sy <= Min(h - 1, y + half); ++sy)
            for (int sx = Max(0, x - half); sx <= Min(w - 1, x + half); ++sx)
            {
                const double error = difference[sy * w + sx];
                sum += error;
                ++samples;
                if (error > largestError)
                {
                    largestError = error;
                    bestX = sx;
                    bestY = sy;
                }
            }
        return sum / samples;
    }

    // Populosity helpers: a color is put in a bin by its top 5 bits per channel.
    struct PaletteColor { int r, g, b; };

    int ColorBin(int r, int g, int b)
    {
        return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
    }

    PaletteColor BinColor(int bin)
    {
        PaletteColor color = { ((bin >> 10) & 31) << 3, ((bin >> 5) & 31) << 3, (bin & 31) << 3 };
        return color;
    }

    // Index of the palette color with the smallest squared RGB distance.
    int NearestPaletteIndex(const vector<PaletteColor>& palette, const unsigned char* pixel)
    {
        int nearest = 0, bestDistance = INT_MAX;
        for (int i = 0; i < static_cast<int>(palette.size()); ++i)
        {
            const int dr = pixel[RED] - palette[i].r;
            const int dg = pixel[GREEN] - palette[i].g;
            const int db = pixel[BLUE] - palette[i].b;
            const int distance = dr * dr + dg * dg + db * db;
            if (distance < bestDistance)
            {
                bestDistance = distance;
                nearest = i;
            }
        }
        return nearest;
    }

    unsigned char NearestUniform(double value, int channel)
    {
        const int rg[8] = { 0, 36, 73, 109, 146, 182, 219, 255 };
        const int blue[4] = { 0, 85, 170, 255 };
        const int* palette = channel == BLUE ? blue : rg;
        const int count = channel == BLUE ? 4 : 8;
        int nearest = 0;
        for (int i = 1; i < count; ++i)
            if (fabs(value - palette[i]) < fabs(value - palette[nearest])) nearest = i;
        return static_cast<unsigned char>(palette[nearest]);
    }

    // Floyd-Steinberg dithering.
    //   color == false: black/white from the gray value (Dither_FS).
    //   color == true : each of R, G, B to the uniform palette (Dither_Color).
    bool FloydSteinberg(TargaImage& image, bool color)
    {
        if (!ValidImage(image)) return false;
        const int w = image.width, h = image.height;
        const int channels = color ? 3 : 1;

        // Working copy in double, so the diffused error is not rounded away.
        vector<double> work(PixelCount(image) * channels);
        for (int p = 0; p < w * h; ++p)
            for (int c = 0; c < channels; ++c)
                work[p * channels + c] = color ? image.data[p * 4 + c]
                                               : GrayValue(image.data + p * 4);

        // Error goes to: right 7/16, lower-left 3/16, below 5/16, lower-right 1/16.
        // On odd rows we walk right-to-left (serpentine), so left/right swap.
        const int dy[4] = { 0, 1, 1, 1 };
        const double weights[4] = { 7.0 / 16, 3.0 / 16, 5.0 / 16, 1.0 / 16 };
        for (int y = 0; y < h; ++y)
        {
            const int direction = y % 2 == 0 ? 1 : -1;
            const int dx[4] = { direction, -direction, 0, direction };
            for (int i = 0; i < w; ++i)
            {
                const int x = direction == 1 ? i : w - 1 - i;
                const int p = y * w + x;
                for (int c = 0; c < channels; ++c)
                {
                    const double oldValue = work[p * channels + c];
                    const unsigned char newValue = color ? NearestUniform(oldValue, c)
                                                         : (oldValue >= 127.5 ? 255 : 0);
                    if (color) image.data[p * 4 + c] = newValue;
                    else SetGray(image.data + p * 4, newValue);

                    const double error = oldValue - newValue;
                    for (int n = 0; n < 4; ++n)
                    {
                        const int nx = x + dx[n], ny = y + dy[n];
                        if (nx >= 0 && nx < w && ny < h)
                            work[(ny * w + nx) * channels + c] += error * weights[n];
                    }
                }
            }
        }
        return true;
    }

    // Width-four triangular reconstruction. At integer coordinates its 1-D
    // weights are [1,2,1]/4; at half coordinates they are [1,3,3,1]/8.
    // Their outer products give exactly the half/double masks in the assignment.
    void SampleBartlett(const TargaImage& source, double x, double y,
                        unsigned char* pixel)
    {
        const int left = static_cast<int>(floor(x)) - 1;
        const int top = static_cast<int>(floor(y)) - 1;
        double result[4] = { 0.0, 0.0, 0.0, 0.0 };
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i)
            {
                const double wx = Max(0.0, 1.0 - fabs(x - (left + i)) / 2.0);
                const double wy = Max(0.0, 1.0 - fabs(y - (top + j)) / 2.0);
                const int sx = Reflect(static_cast<long long>(left) + i, source.width);
                const int sy = Reflect(static_cast<long long>(top) + j, source.height);
                const size_t offset = (static_cast<size_t>(sy) * source.width + sx) * 4;
                for (int c = 0; c < 4; ++c)
                    result[c] += source.data[offset + c] * wx * wy / 4.0;
            }
        for (int c = 0; c < 4; ++c) pixel[c] = ClampByte(result[c]);
    }

    enum CompositeMode { COMPOSITE_OVER, COMPOSITE_IN, COMPOSITE_OUT,
                         COMPOSITE_ATOP, COMPOSITE_XOR };

    bool Composite(TargaImage& foreground, const TargaImage* background,
                   CompositeMode mode)
    {
        if (!ValidImage(foreground) || !background || !ValidImage(*background) ||
            foreground.width != background->width || foreground.height != background->height)
            return false;

        // Porter-Duff factors apply directly to the stored premultiplied RGBA.
        for (size_t p = 0; p < PixelCount(foreground); ++p)
        {
            const size_t i = p * 4;
            const double a = foreground.data[i + 3] / 255.0;
            const double b = background->data[i + 3] / 255.0;
            double fa = 1.0, fb = 1.0 - a;
            switch (mode)
            {
                case COMPOSITE_IN:   fa = b;       fb = 0.0;     break;
                case COMPOSITE_OUT:  fa = 1.0 - b; fb = 0.0;     break;
                case COMPOSITE_ATOP: fa = b;       fb = 1.0 - a; break;
                case COMPOSITE_XOR:  fa = 1.0 - b; fb = 1.0 - a; break;
                case COMPOSITE_OVER: break;
            }
            for (int c = 0; c < 4; ++c)
                foreground.data[i + c] = ClampByte(foreground.data[i + c] * fa +
                                                   background->data[i + c] * fb);
        }
        return true;
    }
}


// Computes n choose s, efficiently
double Binomial(int n, int s)
{
    double        res;

    res = 1;
    for (int i = 1 ; i <= s ; i++)
        res = (n - i + 1) * res / i ;

    return res;
}// Binomial


///////////////////////////////////////////////////////////////////////////////
//
//      Constructor.  Initialize member variables.
//
///////////////////////////////////////////////////////////////////////////////
TargaImage::TargaImage() : width(0), height(0), data(NULL)
{}// TargaImage

///////////////////////////////////////////////////////////////////////////////
//
//      Constructor.  Initialize member variables.
//
///////////////////////////////////////////////////////////////////////////////
TargaImage::TargaImage(int w, int h) : width(w), height(h)
{
   data = new unsigned char[width * height * 4];
   ClearToBlack();
}// TargaImage



///////////////////////////////////////////////////////////////////////////////
//
//      Constructor.  Initialize member variables to values given.
//
///////////////////////////////////////////////////////////////////////////////
TargaImage::TargaImage(int w, int h, unsigned char *d)
{
    int i;

    width = w;
    height = h;
    data = new unsigned char[width * height * 4];

    for (i = 0; i < width * height * 4; i++)
	    data[i] = d[i];
}// TargaImage

///////////////////////////////////////////////////////////////////////////////
//
//      Copy Constructor.  Initialize member to that of input
//
///////////////////////////////////////////////////////////////////////////////
TargaImage::TargaImage(const TargaImage& image) 
{
   width = image.width;
   height = image.height;
   data = NULL; 
   if (image.data != NULL) {
      data = new unsigned char[width * height * 4];
      memcpy(data, image.data, sizeof(unsigned char) * width * height * 4);
   }
}


///////////////////////////////////////////////////////////////////////////////
//
//      Destructor.  Free image memory.
//
///////////////////////////////////////////////////////////////////////////////
TargaImage::~TargaImage()
{
    if (data)
        delete[] data;
}// ~TargaImage


///////////////////////////////////////////////////////////////////////////////
//
//      Converts an image to RGB form, and returns the rgb pixel data - 24 
//  bits per pixel. The returned space should be deleted when no longer 
//  required.
//
///////////////////////////////////////////////////////////////////////////////
unsigned char* TargaImage::To_RGB(void)
{
    if (!ValidImage(*this)) return NULL;
    unsigned char   *rgb = new unsigned char[width * height * 3];
    int		    i, j;

    // Divide out the alpha
    for (i = 0 ; i < height ; i++)
    {
	    int in_offset = i * width * 4;
	    int out_offset = i * width * 3;

	    for (j = 0 ; j < width ; j++)
        {
	        RGBA_To_RGB(data + (in_offset + j*4), rgb + (out_offset + j*3));
	    }
    }

    return rgb;
}// TargaImage


///////////////////////////////////////////////////////////////////////////////
//
//      Save as TGA, PNG, or JPEG according to the filename. PNG preserves alpha;
//  JPEG composites onto white. Returns true on success, false on failure.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Save_Image(const char *filename)
{
    if (!ValidImage(*this) || !filename || !*filename) return false;
    const ImageFormat format = FormatFromFilename(filename);
    if (format == IMAGE_UNSUPPORTED)
    {
        cout << "Unsupported image extension. Use .tga, .png, .jpg, or .jpeg." << endl;
        return false;
    }
    if (format == IMAGE_PNG || format == IMAGE_JPEG)
    {
        const int channels = format == IMAGE_PNG ? 4 : 3;
        vector<unsigned char> pixels(PixelCount(*this) * channels);
        for (size_t p = 0; p < PixelCount(*this); ++p)
        {
            const unsigned int alpha = data[p * 4 + 3];
            for (int c = 0; c < 3; ++c)
            {
                unsigned int value;
                if (format == IMAGE_PNG)
                {
                    // PNG stores straight alpha; undo our premultiplication.
                    // Transparent pixels have no recoverable hidden RGB color.
                    value = alpha ? (data[p * 4 + c] * 255u + alpha / 2) / alpha : 0;
                }
                else
                {
                    // Premultiplied source-over-white, before lossy encoding.
                    value = data[p * 4 + c] + (255u - alpha);
                }
                pixels[p * channels + c] = static_cast<unsigned char>(value > 255 ? 255 : value);
            }
            if (format == IMAGE_PNG) pixels[p * 4 + 3] = static_cast<unsigned char>(alpha);
        }

        // Both stb and our in-memory image use top-to-bottom rows.
        const int written = format == IMAGE_PNG
            ? stbi_write_png(filename, width, height, 4, pixels.data(), width * 4)
            : stbi_write_jpg(filename, width, height, 3, pixels.data(), 90);
        if (!written) cout << "Unable to save image: " << filename << endl;
        return written != 0;
    }

    TargaImage	*out_image = Reverse_Rows();

    if (! out_image)
	    return false;

    if (!tga_write_raw(filename, width, height, out_image->data, TGA_TRUECOLOR_32))
    {
        cout << "TGA Save Error: " << tga_error_string(tga_get_last_error()) << endl;
        delete out_image;
	    return false;
    }

    delete out_image;

    return true;
}// Save_Image


///////////////////////////////////////////////////////////////////////////////
//
//      Load a TGA, PNG, or JPEG file. Return a new TargaImage object which
//  must be deleted by caller.  Return NULL on failure.
//
///////////////////////////////////////////////////////////////////////////////
TargaImage* TargaImage::Load_Image(char *filename)
{
    unsigned char   *temp_data;
    TargaImage	    *temp_image;
    TargaImage	    *result;
    int		        width, height;

    if (!filename || !*filename)
    {
        cout << "No filename given." << endl;
        return NULL;
    }// if

    const ImageFormat format = FormatFromFilename(filename);
    if (format == IMAGE_UNSUPPORTED)
    {
        cout << "Unsupported image extension. Use .tga, .png, .jpg, or .jpeg." << endl;
        return NULL;
    }
    if (format == IMAGE_PNG || format == IMAGE_JPEG)
    {
        int channels;
        unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
            stbi_load(filename, &width, &height, &channels, STBI_rgb_alpha), stbi_image_free);
        if (!pixels)
        {
            const char* reason = stbi_failure_reason();
            cout << "Image load error: " << (reason ? reason : "Unknown error") << endl;
            return NULL;
        }
        if (width <= 0 || height <= 0 || static_cast<size_t>(width) * height > INT_MAX / 4)
        {
            cout << "Image dimensions exceed the supported size." << endl;
            return NULL;
        }

        // stb supplies straight RGBA (opaque alpha for JPEG). Convert to the
        // premultiplied RGBA representation used by the existing operations.
        for (size_t p = 0; p < static_cast<size_t>(width) * height; ++p)
        {
            unsigned char* pixel = pixels.get() + p * 4;
            for (int c = 0; c < 3; ++c)
                pixel[c] = static_cast<unsigned char>((pixel[c] * pixel[3] + 127u) / 255u);
        }
        return new TargaImage(width, height, pixels.get());
    }

    temp_data = (unsigned char*)tga_load(filename, &width, &height, TGA_TRUECOLOR_32);
    if (!temp_data)
    {
        cout << "TGA Error: " << tga_error_string(tga_get_last_error()) << endl;
	    width = height = 0;
	    return NULL;
    }
    temp_image = new TargaImage(width, height, temp_data);
    free(temp_data);

    result = temp_image->Reverse_Rows();

    delete temp_image;

    return result;
}// Load_Image


///////////////////////////////////////////////////////////////////////////////
//
//      Convert image to grayscale.  Red, green, and blue channels should all 
//  contain grayscale value.  Alpha channel shoould be left unchanged.  Return
//  success of operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::To_Grayscale()
{
    if (!ValidImage(*this)) return false;
    for (int p = 0; p < width * height; ++p)
    {
        unsigned char* pixel = data + p * 4;
        SetGray(pixel, GrayValue(pixel));   // gray = 0.299 R + 0.587 G + 0.114 B
        // Alpha (pixel[3]) is left unchanged.
    }
    return true;
}// To_Grayscale


///////////////////////////////////////////////////////////////////////////////
//
//  Convert the image to an 8 bit image using uniform quantization.  Return 
//  success of operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Quant_Uniform()
{
    if (!ValidImage(*this)) return false;
    // 8 levels of red, 8 of green, 4 of blue: 8 * 8 * 4 = 256 colors.
    for (int p = 0; p < width * height; ++p)
    {
        unsigned char* pixel = data + p * 4;
        const int rLevel = pixel[RED] >> 5;     // 0~7
        const int gLevel = pixel[GREEN] >> 5;   // 0~7
        const int bLevel = pixel[BLUE] >> 6;    // 0~3
        pixel[RED]   = (unsigned char)round(rLevel * 255.0 / 7.0);
        pixel[GREEN] = (unsigned char)round(gLevel * 255.0 / 7.0);
        pixel[BLUE]  = (unsigned char)round(bLevel * 255.0 / 3.0);
    }
    return true;
}// Quant_Uniform


///////////////////////////////////////////////////////////////////////////////
//
//      Convert the image to an 8 bit image using populosity quantization.  
//  Return success of operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Quant_Populosity()
{
    if (!ValidImage(*this)) return false;
    const int pixelCount = width * height;

    // Step 1: histogram. Keep 5 bits per channel, so there are 32 x 32 x 32 bins.
    vector<int> histogram(32 * 32 * 32, 0);
    for (int p = 0; p < pixelCount; ++p)
    {
        const unsigned char* pixel = data + p * 4;
        ++histogram[ColorBin(pixel[RED], pixel[GREEN], pixel[BLUE])];
    }

    // Step 2: the (at most) 256 most popular bins become the palette.
    // stable_sort keeps bins with equal counts in increasing bin order.
    vector<int> bins;
    for (int bin = 0; bin < 32 * 32 * 32; ++bin)
        if (histogram[bin] > 0) bins.push_back(bin);
    stable_sort(bins.begin(), bins.end(),
                [&histogram](int a, int b) { return histogram[a] > histogram[b]; });
    if (bins.size() > 256) bins.resize(256);

    vector<PaletteColor> palette;
    for (size_t i = 0; i < bins.size(); ++i)
        palette.push_back(BinColor(bins[i]));

    // Step 3: replace every pixel with the closest palette color.
    for (int p = 0; p < pixelCount; ++p)
    {
        unsigned char* pixel = data + p * 4;
        const PaletteColor& nearest = palette[NearestPaletteIndex(palette, pixel)];
        pixel[RED]   = static_cast<unsigned char>(nearest.r);
        pixel[GREEN] = static_cast<unsigned char>(nearest.g);
        pixel[BLUE]  = static_cast<unsigned char>(nearest.b);
    }
    return true;
}// Quant_Populosity


///////////////////////////////////////////////////////////////////////////////
//
//      Dither the image using a threshold of 1/2.  Return success of operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Dither_Threshold()
{
    if (!ValidImage(*this)) return false;
    for (int p = 0; p < width * height; ++p)
    {
        unsigned char* pixel = data + p * 4;
        SetGray(pixel, GrayValue(pixel) >= 128 ? 255 : 0);   // threshold 0.5
    }
    return true;
}// Dither_Threshold


///////////////////////////////////////////////////////////////////////////////
//
//      Dither image using random dithering.  Return success of operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Dither_Random()
{
    if (!ValidImage(*this)) return false;
    mt19937 randomEngine((random_device())());
    uniform_real_distribution<double> noise(-0.2, 0.2);
    for (int p = 0; p < width * height; ++p)
    {
        unsigned char* pixel = data + p * 4;
        // Add a random value in [-0.2, 0.2], then threshold at 0.5.
        const double intensity = GrayValue(pixel) / 255.0 + noise(randomEngine);
        SetGray(pixel, intensity >= 0.5 ? 255 : 0);
    }
    return true;
}// Dither_Random


///////////////////////////////////////////////////////////////////////////////
//
//      Perform Floyd-Steinberg dithering on the image.  Return success of 
//  operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Dither_FS()
{
    return FloydSteinberg(*this, false);
}// Dither_FS


///////////////////////////////////////////////////////////////////////////////
//
//      Dither the image while conserving the average brightness.  Return 
//  success of operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Dither_Bright()
{
    if (!ValidImage(*this)) return false;
    const int pixelCount = width * height;

    // Count the pixels at each gray level, and the total brightness.
    vector<int> levelCount(256, 0);
    double sum = 0.0;
    for (int p = 0; p < pixelCount; ++p)
    {
        const unsigned char gray = GrayValue(data + p * 4);
        ++levelCount[gray];
        sum += gray / 255.0;
    }

    // To keep the average brightness, this many pixels must become white.
    const int whiteCount = Min(pixelCount, static_cast<int>(floor(sum + 0.5)));

    // Walk from the brightest level down until we have enough white pixels.
    // Every pixel brighter than "threshold" is white. At the threshold level
    // itself only "whiteAtThreshold" pixels (the first ones found) are white.
    int threshold = 255, brighterCount = 0;
    while (threshold > 0 && brighterCount + levelCount[threshold] < whiteCount)
    {
        brighterCount += levelCount[threshold];
        --threshold;
    }
    int whiteAtThreshold = whiteCount - brighterCount;

    for (int p = 0; p < pixelCount; ++p)
    {
        unsigned char* pixel = data + p * 4;
        const int gray = GrayValue(pixel);
        bool white = gray > threshold;
        if (gray == threshold && whiteAtThreshold > 0)
        {
            white = true;
            --whiteAtThreshold;
        }
        SetGray(pixel, white ? 255 : 0);
    }
    return true;
}// Dither_Bright


///////////////////////////////////////////////////////////////////////////////
//
//      Perform clustered differing of the image.  Return success of operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Dither_Cluster()
{
    if (!ValidImage(*this)) return false;
    const double mask[4][4] = {
        { 0.7059, 0.3529, 0.5882, 0.2353 },
        { 0.0588, 0.9412, 0.8235, 0.4118 },
        { 0.4706, 0.7647, 0.8824, 0.1176 },
        { 0.1765, 0.5294, 0.2941, 0.6471 }
    };
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            unsigned char* pixel = data + (y * width + x) * 4;
            // The PDF's I[x][y] / mask[x % 4][y % 4] indexes rows first, so the
            // first index is the row (y). This matches the reference program.
            const double intensity = GrayValue(pixel) / 255.0;
            SetGray(pixel, intensity >= mask[y % 4][x % 4] ? 255 : 0);
        }
    return true;
}// Dither_Cluster


///////////////////////////////////////////////////////////////////////////////
//
//  Convert the image to an 8 bit image using Floyd-Steinberg dithering over
//  a uniform quantization - the same quantization as in Quant_Uniform.
//  Return success of operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Dither_Color()
{
    return FloydSteinberg(*this, true);
}// Dither_Color


///////////////////////////////////////////////////////////////////////////////
//
//      Composite the current image over the given image.  Return success of 
//  operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Comp_Over(TargaImage* pImage)
{
    return Composite(*this, pImage, COMPOSITE_OVER);
}// Comp_Over


///////////////////////////////////////////////////////////////////////////////
//
//      Composite this image "in" the given image.  See lecture notes for 
//  details.  Return success of operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Comp_In(TargaImage* pImage)
{
    return Composite(*this, pImage, COMPOSITE_IN);
}// Comp_In


///////////////////////////////////////////////////////////////////////////////
//
//      Composite this image "out" the given image.  See lecture notes for 
//  details.  Return success of operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Comp_Out(TargaImage* pImage)
{
    return Composite(*this, pImage, COMPOSITE_OUT);
}// Comp_Out


///////////////////////////////////////////////////////////////////////////////
//
//      Composite current image "atop" given image.  Return success of 
//  operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Comp_Atop(TargaImage* pImage)
{
    return Composite(*this, pImage, COMPOSITE_ATOP);
}// Comp_Atop


///////////////////////////////////////////////////////////////////////////////
//
//      Composite this image with given image using exclusive or (XOR).  Return
//  success of operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Comp_Xor(TargaImage* pImage)
{
    return Composite(*this, pImage, COMPOSITE_XOR);
}// Comp_Xor


///////////////////////////////////////////////////////////////////////////////
//
//      Calculate the difference bewteen this imag and the given one.  Image 
//  dimensions must be equal.  Return success of operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Difference(TargaImage* pImage)
{
    if (!ValidImage(*this) || !pImage || !ValidImage(*pImage))
        return false;

    if (width != pImage->width || height != pImage->height)
    {
        cout << "Difference: Images not the same size\n";
        return false;
    }// if

    for (int i = 0 ; i < width * height * 4 ; i += 4)
    {
        unsigned char        rgb1[3];
        unsigned char        rgb2[3];

        RGBA_To_RGB(data + i, rgb1);
        RGBA_To_RGB(pImage->data + i, rgb2);

        data[i] = abs(rgb1[0] - rgb2[0]);
        data[i+1] = abs(rgb1[1] - rgb2[1]);
        data[i+2] = abs(rgb1[2] - rgb2[2]);
        data[i+3] = 255;
    }

    return true;
}// Difference


///////////////////////////////////////////////////////////////////////////////
//
//      Perform 5x5 box filter on this image.  Return success of operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Filter_Box()
{
    return FilterRGB(*this, vector<double>(5, 1.0 / 5.0));
}// Filter_Box


///////////////////////////////////////////////////////////////////////////////
//
//      Perform 5x5 Bartlett filter on this image.  Return success of 
//  operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Filter_Bartlett()
{
    const double weights[5] = { 1.0 / 9, 2.0 / 9, 3.0 / 9, 2.0 / 9, 1.0 / 9 };
    return FilterRGB(*this, vector<double>(weights, weights + 5));
}// Filter_Bartlett


///////////////////////////////////////////////////////////////////////////////
//
//      Perform 5x5 Gaussian filter on this image.  Return success of 
//  operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Filter_Gaussian()
{
    return Filter_Gaussian_N(5);
}// Filter_Gaussian

///////////////////////////////////////////////////////////////////////////////
//
//      Perform NxN Gaussian filter on this image.  Return success of 
//  operation.
//
///////////////////////////////////////////////////////////////////////////////

bool TargaImage::Filter_Gaussian_N( unsigned int N )
{
    if (!ValidImage(*this) || N == 0 || N % 2 == 0 || N > INT_MAX)
        return false;
    return FilterRGB(*this, GaussianKernel(N));
}// Filter_Gaussian_N


///////////////////////////////////////////////////////////////////////////////
//
//      Perform 5x5 edge detect (high pass) filter on this image.  Return 
//  success of operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Filter_Edge()
{
    // High pass = original minus its 5x5 Gaussian low pass.
    return FilterRGB(*this, GaussianKernel(5), HIGH_PASS);
}// Filter_Edge


///////////////////////////////////////////////////////////////////////////////
//
//      Perform a 5x5 enhancement filter to this image.  Return success of 
//  operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Filter_Enhance()
{
    // Unsharp mask = original plus high pass (2 * original - blurred).
    return FilterRGB(*this, GaussianKernel(5), SHARPEN);
}// Filter_Enhance


///////////////////////////////////////////////////////////////////////////////
//
//      Run simplified version of Hertzmann's painterly image filter.
//      You probably will want to use the Draw_Stroke funciton and the
//      Stroke class to help.
// Return success of operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::NPR_Paint()
{
    if (!ValidImage(*this)) return false;

    // Simplified circular-stroke algorithm from Hertzmann, SIGGRAPH 1998,
    // section 2.1: https://mrl.cs.nyu.edu/publications/painterly98/
    const int pixelCount = width * height;

    // Paint on an opaque copy; remember the alpha to restore it at the end.
    TargaImage source(*this);
    vector<unsigned char> alpha(pixelCount);
    for (int p = 0; p < pixelCount; ++p)
    {
        alpha[p] = data[p * 4 + 3];
        unsigned char rgb[3];
        RGBA_To_RGB(source.data + p * 4, rgb);
        for (int c = 0; c < 3; ++c) source.data[p * 4 + c] = rgb[c];
        source.data[p * 4 + 3] = 255;
    }
    ClearToBlack();   // "this" is now the canvas

    const int radii[3] = { 7, 3, 1 };      // brush sizes, largest first
    const double threshold = 25.0;         // T in the paper
    mt19937 randomEngine((random_device())());
    vector<double> difference(pixelCount);

    for (int layer = 0; layer < 3; ++layer)
    {
        const int radius = radii[layer];
        const int grid = radius;           // fg = 1

        // Reference image: the source blurred with a (2r+1) x (2r+1) Gaussian.
        TargaImage reference(source);
        reference.Filter_Gaussian_N(2 * radius + 1);

        // Color distance between the canvas and the reference at every pixel.
        // An unpainted canvas must receive strokes even in dark regions.
        for (int p = 0; p < pixelCount; ++p)
            difference[p] = layer == 0 ? 1e6
                : ColorDistance(data + p * 4, reference.data + p * 4);

        // One possible stroke per grid cell, at the pixel with the largest error.
        vector<Stroke> strokes;
        for (int y = 0; y < height; y += grid)
            for (int x = 0; x < width; x += grid)
            {
                int bestX, bestY;
                const double areaError =
                    GridCellError(difference, width, height, x, y, grid / 2, bestX, bestY);
                if (areaError > threshold)
                {
                    const unsigned char* color = reference.data + (bestY * width + bestX) * 4;
                    strokes.push_back(Stroke(radius, bestX, bestY,
                                             color[0], color[1], color[2], 255));
                }
            }

        // Plan a whole layer before drawing it; randomize overlap order.
        shuffle(strokes.begin(), strokes.end(), randomEngine);
        for (size_t i = 0; i < strokes.size(); ++i) Paint_Stroke(strokes[i]);

        // After the first layer, fill any spot the circles did not fully cover
        // (image borders, antialiased rims) with the reference color.
        if (layer == 0)
            for (int p = 0; p < pixelCount; ++p)
            {
                const double uncovered = 1.0 - data[p * 4 + 3] / 255.0;
                for (int c = 0; c < 3; ++c)
                    data[p * 4 + c] = ClampByte(data[p * 4 + c] +
                                               uncovered * reference.data[p * 4 + c]);
                data[p * 4 + 3] = 255;
            }
    }

    // Restore the original transparency, with premultiplied color values.
    for (int p = 0; p < pixelCount; ++p)
    {
        for (int c = 0; c < 3; ++c)
            data[p * 4 + c] = ClampByte(data[p * 4 + c] * (alpha[p] / 255.0));
        data[p * 4 + 3] = alpha[p];
    }
    return true;
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


///////////////////////////////////////////////////////////////////////////////
//
//      Halve the dimensions of this image.  Return success of operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Half_Size()
{
    return Resize(0.5f);
}// Half_Size


///////////////////////////////////////////////////////////////////////////////
//
//      Double the dimensions of this image.  Return success of operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Double_Size()
{
    return Resize(2.0f);
}// Double_Size


///////////////////////////////////////////////////////////////////////////////
//
//      Scale the image dimensions by any positive factor, using Bartlett
//  reconstruction for both enlargement and reduction.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Resize(float scale)
{
    if (!ValidImage(*this) || !std::isfinite(scale) || scale <= 0.0f)
        return false;
    const double scaledWidth = floor(width * static_cast<double>(scale));
    const double scaledHeight = floor(height * static_cast<double>(scale));
    if (scaledWidth > INT_MAX || scaledHeight > INT_MAX) return false;
    // A very small image/factor still produces a valid one-pixel axis.
    const int newWidth = Max(1, static_cast<int>(scaledWidth));
    const int newHeight = Max(1, static_cast<int>(scaledHeight));
    const size_t newCount = static_cast<size_t>(newWidth) * newHeight;
    if (newCount > INT_MAX / 4) return false;

    unsigned char* resized = new unsigned char[newCount * 4];
    for (int y = 0; y < newHeight; ++y)
        for (int x = 0; x < newWidth; ++x)
            SampleBartlett(*this, x / static_cast<double>(scale),
                           y / static_cast<double>(scale),
                           resized + (static_cast<size_t>(y) * newWidth + x) * 4);
    delete[] data;
    data = resized;
    width = newWidth;
    height = newHeight;
    return true;
}// Resize


//////////////////////////////////////////////////////////////////////////////
//
//      Rotate the image clockwise by the given angle.  Do not resize the 
//  image.  Return success of operation.
//
///////////////////////////////////////////////////////////////////////////////
bool TargaImage::Rotate(float angleDegrees)
{
    if (!ValidImage(*this) || !std::isfinite(angleDegrees)) return false;
    const double angle = fmod(static_cast<double>(angleDegrees), 360.0) * acos(-1.0) / 180.0;
    const double cosine = cos(angle), sine = sin(angle);
    const double centerX = (width - 1) / 2.0, centerY = (height - 1) / 2.0;
    vector<unsigned char> rotated(PixelCount(*this) * 4, 0);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            const double dx = x - centerX, dy = y - centerY;
            // Inverse map a clockwise rotation in screen coordinates (y down).
            double sx = cosine * dx + sine * dy + centerX;
            double sy = -sine * dx + cosine * dy + centerY;
            if (sx < -1e-9 || sx > width - 1 + 1e-9 || sy < -1e-9 || sy > height - 1 + 1e-9)
                continue; // No source data: leave transparent black.
            sx = Max(0.0, Min(static_cast<double>(width - 1), sx));
            sy = Max(0.0, Min(static_cast<double>(height - 1), sy));
            SampleBartlett(*this, sx, sy, &rotated[(static_cast<size_t>(y) * width + x) * 4]);
        }
    memcpy(data, &rotated[0], rotated.size());
    return true;
}// Rotate


//////////////////////////////////////////////////////////////////////////////
//
//      Given a single RGBA pixel return, via the second argument, the RGB
//      equivalent composited with a black background.
//
///////////////////////////////////////////////////////////////////////////////
void TargaImage::RGBA_To_RGB(unsigned char *rgba, unsigned char *rgb)
{
    const unsigned char	BACKGROUND[3] = { 0, 0, 0 };

    unsigned char  alpha = rgba[3];

    if (alpha == 0)
    {
        rgb[0] = BACKGROUND[0];
        rgb[1] = BACKGROUND[1];
        rgb[2] = BACKGROUND[2];
    }
    else
    {
	    float	alpha_scale = (float)255 / (float)alpha;
	    int	val;
	    int	i;

	    for (i = 0 ; i < 3 ; i++)
	    {
	        val = (int)floor(rgba[i] * alpha_scale);
	        if (val < 0)
		    rgb[i] = 0;
	        else if (val > 255)
		    rgb[i] = 255;
	        else
		    rgb[i] = val;
	    }
    }
}// RGA_To_RGB


///////////////////////////////////////////////////////////////////////////////
//
//      Copy this into a new image, reversing the rows as it goes. A pointer
//  to the new image is returned.
//
///////////////////////////////////////////////////////////////////////////////
TargaImage* TargaImage::Reverse_Rows(void)
{
    if (!ValidImage(*this)) return NULL;
    TargaImage* result = new TargaImage(width, height);
    const int rowBytes = width * 4;
    // Row i of the result is row (height - 1 - i) of this image.
    for (int i = 0; i < height; i++)
        memcpy(result->data + i * rowBytes, data + (height - 1 - i) * rowBytes, rowBytes);
    return result;
}// Reverse_Rows


///////////////////////////////////////////////////////////////////////////////
//
//      Clear the image to all black.
//
///////////////////////////////////////////////////////////////////////////////
void TargaImage::ClearToBlack()
{
    if (ValidImage(*this)) memset(data, 0, PixelCount(*this) * 4);
}// ClearToBlack


///////////////////////////////////////////////////////////////////////////////
//
//      Helper function for the painterly filter; paint a stroke at
// the given location
//
///////////////////////////////////////////////////////////////////////////////
void TargaImage::Paint_Stroke(const Stroke& s) {
   const int radius = (int)s.radius;
   const int radius_squared = radius * radius;
   for (int x_off = -radius; x_off <= radius; x_off++) {
      for (int y_off = -radius; y_off <= radius; y_off++) {
         const int x_loc = (int)s.x + x_off;
         const int y_loc = (int)s.y + y_off;
         // skip pixels outside the image
         if (x_loc < 0 || x_loc >= width || y_loc < 0 || y_loc >= height)
            continue;
         unsigned char* pixel = data + (y_loc * width + x_loc) * 4;
         const int dist_squared = x_off * x_off + y_off * y_off;
         if (dist_squared <= radius_squared) {
            // inside the circle: use the stroke color
            pixel[0] = s.r;
            pixel[1] = s.g;
            pixel[2] = s.b;
            pixel[3] = s.a;
         } else if (dist_squared == radius_squared + 1) {
            // just on the edge: average with the canvas (simple antialiasing)
            pixel[0] = (pixel[0] + s.r) / 2;
            pixel[1] = (pixel[1] + s.g) / 2;
            pixel[2] = (pixel[2] + s.b) / 2;
            pixel[3] = (pixel[3] + s.a) / 2;
         }
      }
   }
}


///////////////////////////////////////////////////////////////////////////////
//
//      Build a Stroke
//
///////////////////////////////////////////////////////////////////////////////
Stroke::Stroke() {}

///////////////////////////////////////////////////////////////////////////////
//
//      Build a Stroke
//
///////////////////////////////////////////////////////////////////////////////
Stroke::Stroke(unsigned int iradius, unsigned int ix, unsigned int iy,
               unsigned char ir, unsigned char ig, unsigned char ib, unsigned char ia) :
   radius(iradius),x(ix),y(iy),r(ir),g(ig),b(ib),a(ia)
{
}
