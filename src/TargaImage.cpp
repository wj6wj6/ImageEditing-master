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
#include "ImageUtils.h"
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

    // Gray level 0~255 of a pixel (the same value To_Grayscale writes).
    unsigned char GrayValue(const unsigned char* pixel)
    {
        return static_cast<unsigned char>(Luminance(pixel));
    }

    void SetGray(unsigned char* pixel, unsigned char value)
    {
        pixel[0] = pixel[1] = pixel[2] = value;
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
