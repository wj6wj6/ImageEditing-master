#include "TargaImage.h"
#include "ScriptHandler.h"
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>

namespace
{
    int checks = 0;
    void Check(bool condition, const char* message)
    {
        ++checks;
        if (!condition) throw std::runtime_error(message);
    }

    bool Same(const TargaImage& a, const TargaImage& b)
    {
        return a.width == b.width && a.height == b.height &&
            std::memcmp(a.data, b.data, static_cast<size_t>(a.width) * a.height * 4) == 0;
    }

    void Fixture(TargaImage& image, bool transparent = false)
    {
        for (int y = 0; y < image.height; ++y)
            for (int x = 0; x < image.width; ++x)
            {
                const int p = (y * image.width + x) * 4;
                const int alpha = transparent ? (x % 5) * 63 : 255;
                image.data[p] = static_cast<unsigned char>((x * 7 % 256) * alpha / 255);
                image.data[p + 1] = static_cast<unsigned char>((y * 11 % 256) * alpha / 255);
                image.data[p + 2] = static_cast<unsigned char>((x > y ? 220 : 40) * alpha / 255);
                image.data[p + 3] = static_cast<unsigned char>(alpha);
            }
    }

    void ReproducibilityAndStyle()
    {
        TargaImage original(128, 96);
        Fixture(original);
        TargaImage a(original), b(original), seed(original), scale(original);
        Check(a.NPR_Paint_Advanced(), "default rendering failed");
        Check(b.NPR_Paint_Advanced() && Same(a, b), "same seed must reproduce the result");
        Check(!Same(a, original), "advanced NPR must change a nonuniform image");
        Check(seed.NPR_Paint_Advanced(1.0f, 42) && !Same(a, seed), "seed must affect the brush pattern");
        Check(scale.NPR_Paint_Advanced(2.0f) && !Same(a, scale), "scale must change the rendering");
        Check(a.width == original.width && a.height == original.height, "NPR must retain dimensions");
        for (int p = 0; p < a.width * a.height; ++p)
            Check(a.data[p * 4 + 3] == 255, "opaque input must have no transparent holes");
    }

    void TransparencyAndTinyImages()
    {
        for (int w = 1; w <= 5; ++w)
            for (int h = 1; h <= 5; ++h)
            {
                TargaImage image(w, h);
                Fixture(image, true);
                TargaImage before(image);
                Check(image.NPR_Paint_Advanced(3.0f, 0), "tiny/transparent rendering failed");
                for (int p = 0; p < w * h; ++p)
                {
                    const int a = image.data[p * 4 + 3];
                    Check(a == before.data[p * 4 + 3], "original alpha must be preserved");
                    for (int c = 0; c < 3; ++c)
                        Check(image.data[p * 4 + c] <= a, "output must remain premultiplied");
                }
            }
        TargaImage red(48, 48);
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 48; ++x)
            {
                const int p = (y * 48 + x) * 4;
                const int a = x >= 12 && x < 36 && y >= 12 && y < 36 ? 128 : 0;
                red.data[p] = red.data[p + 3] = static_cast<unsigned char>(a);
                red.data[p + 1] = red.data[p + 2] = 0;
            }
        Check(red.NPR_Paint_Advanced(2.0f), "alpha boundary rendering failed");
        for (int p = 0; p < 48 * 48; ++p)
            if (red.data[p * 4 + 3])
                Check(red.data[p * 4] >= 110, "transparent black must not darken the red boundary");

        TargaImage flat(40, 30);
        std::memset(flat.data, 255, 40 * 30 * 4);
        Check(flat.NPR_Paint_Advanced(), "zero-gradient image must render");
        for (int p = 0; p < 40 * 30; ++p)
            Check(flat.data[p * 4] > 220 && flat.data[p * 4 + 3] == 255,
                  "uniform painting must remain filled at the border");
    }

    void CartoonAndWatercolor()
    {
        TargaImage source(96, 72);
        Fixture(source, true);
        TargaImage cartoon(source), water(source), repeat(source), varied(source);
        Check(cartoon.NPR_Cartoon(), "cartoon rendering failed");
        Check(water.NPR_Watercolor(), "watercolor rendering failed");
        Check(repeat.NPR_Watercolor() && Same(water, repeat), "watercolor seed must reproduce the result");
        Check(varied.NPR_Watercolor(1.7f, 42) && !Same(water, varied), "watercolor controls must affect the result");
        Check(!Same(cartoon, source) && !Same(water, source) && !Same(cartoon, water),
              "each style must produce its own transformation");
        for (int p = 0; p < source.width * source.height; ++p)
            for (const TargaImage* output : { &cartoon, &water })
            {
                const int a = output->data[p * 4 + 3];
                Check(a == source.data[p * 4 + 3], "all styles must preserve source alpha");
                for (int c = 0; c < 3; ++c)
                    Check(output->data[p * 4 + c] <= a, "all styles must retain premultiplied RGB");
            }
        TargaImage ramp(256, 5);
        for (int y = 0; y < 5; ++y)
            for (int x = 0; x < 256; ++x)
            {
                const int p = (y * 256 + x) * 4;
                ramp.data[p] = ramp.data[p + 1] = ramp.data[p + 2] = static_cast<unsigned char>(x);
                ramp.data[p + 3] = 255;
            }
        Check(ramp.NPR_Cartoon(), "cartoon grayscale ramp failed");
        std::set<unsigned int> colors;
        for (int x = 0; x < 256; ++x)
        {
            const int p = (2 * 256 + x) * 4;
            colors.insert((ramp.data[p] << 16) | (ramp.data[p + 1] << 8) | ramp.data[p + 2]);
        }
        Check(colors.size() >= 4 && colors.size() <= 8, "cartoon must create a few flat value bands");

        TargaImage boundary(48, 24);
        for (int y = 0; y < 24; ++y)
            for (int x = 0; x < 48; ++x)
            {
                const int p = (y * 48 + x) * 4;
                boundary.data[p] = x < 24 ? 187 : 0;
                boundary.data[p + 1] = x < 24 ? 0 : 95;
                boundary.data[p + 2] = 0;
                boundary.data[p + 3] = 255;
            }
        Check(boundary.NPR_Cartoon(), "cartoon chromatic edge failed");
        bool hasInk = false;
        for (int x = 22; x <= 25; ++x)
        {
            const int p = (12 * 48 + x) * 4;
            hasInk = hasInk || (boundary.data[p] < 30 && boundary.data[p + 1] < 30);
        }
        Check(hasInk, "equal-luminance color boundaries must be outlined");

        for (int w = 1; w <= 3; ++w)
            for (int h = 1; h <= 3; ++h)
            {
                TargaImage a(w, h);
                Fixture(a, true);
                TargaImage b(a), before(a);
                Check(a.NPR_Cartoon(3.0f) && b.NPR_Watercolor(0.5f, 0), "new styles must handle tiny images");
                for (int p = 0; p < w * h; ++p)
                    Check(a.data[p * 4 + 3] == before.data[p * 4 + 3] &&
                          b.data[p * 4 + 3] == before.data[p * 4 + 3], "tiny image alpha changed");
            }

        TargaImage* parsed = new TargaImage(source);
        Check(CScriptHandler::HandleCommand("npr-cartoon", parsed) && Same(*parsed, cartoon),
              "cartoon command must dispatch the correct method");
        TargaImage unchanged(*parsed);
        for (const char* command : { "npr-cartoon nan", "npr-cartoon 0", "npr-cartoon 1 2",
                 "npr-watercolor 1 -1", "npr-watercolor inf", "npr-watercolor 1 2 extra" })
            Check(!CScriptHandler::HandleCommand(command, parsed) && Same(*parsed, unchanged),
                  "invalid new-style commands must leave the image intact");
        delete parsed;
        parsed = new TargaImage(source);
        Check(CScriptHandler::HandleCommand("npr-watercolor", parsed) && Same(*parsed, water),
              "watercolor command must dispatch the correct method");
        delete parsed;
        TargaImage empty;
        Check(!empty.NPR_Cartoon() && !empty.NPR_Watercolor(), "empty new-style input must fail");
        TargaImage invalid(source);
        Check(!invalid.NPR_Cartoon(-1) && !invalid.NPR_Watercolor(4) && Same(invalid, source),
              "invalid direct style parameters must not mutate the image");
    }

    void InvalidInputAndCommands()
    {
        TargaImage empty;
        Check(!empty.NPR_Paint_Advanced(), "empty image must fail safely");
        TargaImage input(12, 9);
        Fixture(input);
        TargaImage before(input);
        const float invalid[] = { 0.0f, -1.0f, 0.49f, 3.01f,
            std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() };
        for (float value : invalid)
            Check(!input.NPR_Paint_Advanced(value) && Same(input, before),
                  "invalid scale must fail without changing the image");

        TargaImage* image = new TargaImage(input);
        Check(CScriptHandler::HandleCommand("npr-paint-advanced", image), "default command failed");
        Check(before.NPR_Paint_Advanced() && Same(*image, before), "default command must use default parameters");
        const char* invalidCommands[] = {
            "npr-paint-advanced -1", "npr-paint-advanced 0.49", "npr-paint-advanced 3.01",
            "npr-paint-advanced nan", "npr-paint-advanced inf", "npr-paint-advanced word",
            "npr-paint-advanced 1x", "npr-paint-advanced 1 -1", "npr-paint-advanced 1 4x",
            "npr-paint-advanced 1 4294967296", "npr-paint-advanced 1 1 extra"
        };
        for (const char* command : invalidCommands)
            Check(!CScriptHandler::HandleCommand(command, image) && Same(*image, before),
                  "invalid command must be rejected without changing the image");
        delete image;
        image = new TargaImage(input);
        TargaImage expected(input);
        Check(expected.NPR_Paint_Advanced(0.5f, 4294967295u) &&
              CScriptHandler::HandleCommand("npr-paint-advanced 0.5 4294967295", image) && Same(*image, expected),
              "optional scale and maximum unsigned seed must reach the renderer");
        delete image;
    }
}

int main()
{
    try
    {
        ReproducibilityAndStyle();
        TransparencyAndTinyImages();
        CartoonAndWatercolor();
        InvalidInputAndCommands();
        std::cout << "PASS: " << checks << " NPR and command assertions\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
