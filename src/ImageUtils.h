///////////////////////////////////////////////////////////////////////////////
//
//      ImageUtils.h
//
//      Small helper functions shared by TargaImage.cpp and TargaImageNPR.cpp.
//
///////////////////////////////////////////////////////////////////////////////

#ifndef _IMAGE_UTILS_H_
#define _IMAGE_UTILS_H_

#include "TargaImage.h"
#include <stddef.h>
#include <limits.h>

inline bool ValidImage(const TargaImage& image)
{
    return image.data && image.width > 0 && image.height > 0 &&
        static_cast<size_t>(image.width) * image.height <= INT_MAX / 4;
}

// Number of pixels (width * height). Use this whenever a loop only needs the
// pixel index p, and not the (x, y) position. ValidImage guarantees that
// width * height * 4 fits in an int, so an int is enough.
inline int PixelCount(const TargaImage& image)
{
    return image.width * image.height;
}

inline unsigned char ClampByte(double value)
{
    if (value <= 0.0) return 0;
    if (value >= 255.0) return 255;
    // Suppress roundoff such as 99.99999999999999 on a constant image.
    return static_cast<unsigned char>(value + 1e-9);
}

inline double Luminance(const unsigned char* pixel)
{
    return 0.299 * pixel[0] + 0.587 * pixel[1] + 0.114 * pixel[2];
}

// Reflect about pixel centers at the boundary; also works for 1-pixel axes.
inline int Reflect(long long coordinate, int length)
{
    if (length <= 1) return 0;
    const long long period = 2LL * (length - 1);
    coordinate %= period;
    if (coordinate < 0) coordinate += period;
    return static_cast<int>(coordinate < length ? coordinate : period - coordinate);
}

#endif
