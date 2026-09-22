"""End-to-end codec checks against Pillow, through the real load/save commands.

Requires Pillow. Run with --exe PATH to test a different build configuration.
"""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from PIL import Image


EXE = Path(__file__).resolve().parents[1] / "build/Debug/ImageEditing.exe"


def pattern(size=(19, 13)):
    image = Image.new("RGB", size)
    image.putdata([
        ((x * 17 + y * 3) % 256, (x * 5 + y * 29) % 256, (x * 11 + y * 7) % 256)
        for y in range(size[1]) for x in range(size[0])
    ])
    return image


class ImageFormatTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="p1-image-formats-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def run_commands(self, commands, *, expect_error=False, after=None):
        (self.root / "commands.txt").write_text("\n".join(commands) + "\n", encoding="utf-8")
        scripts = ["commands.txt"]
        if after is not None:
            (self.root / "after.txt").write_text("\n".join(after) + "\n", encoding="utf-8")
            scripts.append("after.txt")
        result = subprocess.run(
            [str(EXE), "-headless", *scripts], cwd=self.root,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30,
        )
        output = result.stdout.decode("utf-8", errors="replace").strip()
        self.assertEqual(result.returncode, 0, output)
        if expect_error:
            self.assertTrue(output, "Expected an error diagnostic")
        else:
            self.assertEqual(output, "")
        return output

    def read(self, filename, expected_format):
        with Image.open(self.root / filename) as image:
            self.assertEqual(image.format, expected_format)
            return image.convert("RGBA")

    def assert_pixels_close(self, actual, expected, tolerance=0):
        self.assertEqual(actual.size, expected.size)
        a, b = actual.convert("RGBA").tobytes(), expected.convert("RGBA").tobytes()
        difference = max(abs(x - y) for x, y in zip(a, b))
        self.assertLessEqual(difference, tolerance)

    def assert_visible_close(self, actual, expected, tolerance=1):
        self.assertEqual(actual.getchannel("A").tobytes(), expected.getchannel("A").tobytes())
        for background in [(0, 0, 0, 255), (255, 255, 255, 255)]:
            matte = Image.new("RGBA", expected.size, background)
            self.assert_pixels_close(
                Image.alpha_composite(matte, actual),
                Image.alpha_composite(matte, expected), tolerance,
            )

    def test_opaque_png_roundtrip_and_orientation(self):
        source = pattern()
        source.save(self.root / "source.png")
        self.run_commands(["load source.png", "save output.png"])
        self.assert_pixels_close(self.read("output.png", "PNG"), source)
        self.assertTrue((self.root / "output.png").read_bytes().startswith(b"\x89PNG\r\n\x1a\n"))

    def test_transparent_png_preserves_alpha_and_visible_colors(self):
        source = pattern((9, 4)).convert("RGBA")
        source.putalpha(Image.frombytes("L", source.size, bytes([0, 1, 17, 64, 128, 200, 254, 255, 0] * 4)))
        source.save(self.root / "transparent.png")
        self.run_commands(["load transparent.png", "save output.png"])
        self.assert_visible_close(self.read("output.png", "PNG"), source)

    def test_grayscale_and_grayscale_alpha_png(self):
        for mode in ["L", "LA"]:
            with self.subTest(mode=mode):
                source = pattern().convert(mode)
                if mode == "LA":
                    source.putalpha(Image.new("L", source.size, 128))
                source.save(self.root / "source.png")
                self.run_commands(["load source.png", "save output.png"])
                self.assert_visible_close(self.read("output.png", "PNG"), source.convert("RGBA"))

    def test_palette_png_transparency(self):
        source = Image.new("P", (3, 2))
        source.putpalette([255, 0, 0, 0, 255, 0, 0, 0, 255] + [0] * (768 - 9))
        source.putdata([0, 1, 2, 2, 0, 1])
        source.info["transparency"] = bytes([255, 0, 128])
        source.save(self.root / "palette.png")
        self.run_commands(["load palette.png", "save output.png"])
        self.assert_visible_close(self.read("output.png", "PNG"), source.convert("RGBA"))

    def test_png_is_editable_with_existing_operations(self):
        source = pattern()
        source.save(self.root / "source.png")
        self.run_commands(["load source.png", "gray", "save gray.png", "half", "save half.png"])
        result = self.read("gray.png", "PNG")
        for old, new in zip(source.getdata(), result.getdata()):
            luminance = int(0.299 * old[0] + 0.587 * old[1] + 0.114 * old[2])
            self.assertEqual(new[0], new[1])
            self.assertEqual(new[1], new[2])
            self.assertLessEqual(abs(new[0] - luminance), 1)
            self.assertEqual(new[3], 255)
        self.assertEqual(self.read("half.png", "PNG").size, (source.width // 2, source.height // 2))

    def test_jpeg_rgb_grayscale_and_progressive_loading(self):
        source = Image.new("RGB", (64, 48))
        source.putdata([(x * 3, y * 4, (x + y) * 2) for y in range(48) for x in range(64)])
        for mode, progressive in [("RGB", False), ("L", False), ("RGB", True)]:
            with self.subTest(mode=mode, progressive=progressive):
                source.convert(mode).save(self.root / "source.jpg", quality=95, progressive=progressive)
                expected = self.read("source.jpg", "JPEG")
                self.run_commands(["load source.jpg", "save decoded.png"])
                self.assert_pixels_close(self.read("decoded.png", "PNG"), expected, tolerance=4)

    def test_jpeg_export_white_background_and_no_image_mutation(self):
        source = Image.new("RGBA", (96, 48), (210, 50, 20, 0))
        source.paste((210, 50, 20, 128), (32, 0, 64, 48))
        source.paste((20, 70, 180, 255), (64, 0, 96, 48))
        source.save(self.root / "source.png")
        self.run_commands([
            "load source.png", "save before.png", "save result.jpg",
            "save result.JpEg", "save after.png", "load result.jpg", "save decoded.png",
        ])
        expected = Image.alpha_composite(Image.new("RGBA", source.size, "white"), source)
        for name in ["result.jpg", "result.JpEg"]:
            actual = self.read(name, "JPEG")
            self.assertEqual(actual.size, source.size)
            for x in [16, 48, 80]:
                error = max(abs(a - b) for a, b in zip(actual.getpixel((x, 24)), expected.getpixel((x, 24))))
                self.assertLessEqual(error, 4)
        self.assertEqual((self.root / "before.png").read_bytes(), (self.root / "after.png").read_bytes())
        self.assert_pixels_close(self.read("decoded.png", "PNG"), self.read("result.jpg", "JPEG"), tolerance=4)

    def test_case_insensitive_extensions_and_utf8_filenames(self):
        source = pattern()
        source.save(self.root / "測試.PnG")
        self.run_commands(["load 測試.PnG", "save 輸出.PNG", "save 照片.JPG", "load 照片.JPG", "save decoded.png"])
        self.assert_pixels_close(self.read("輸出.PNG", "PNG"), source)
        self.assertEqual(self.read("照片.JPG", "JPEG").size, source.size)
        self.assertEqual(self.read("decoded.png", "PNG").size, source.size)

    def test_tga_png_conversion_and_legacy_suffixless_files(self):
        source = pattern()
        source.save(self.root / "source.tga")
        self.run_commands([
            "load source.tga", "save from-tga.png", "load from-tga.png", "save back.TGA",
            "save suffixless", "load suffixless", "save legacy.png",
        ])
        self.assert_pixels_close(self.read("from-tga.png", "PNG"), source, tolerance=1)
        self.assert_pixels_close(self.read("back.TGA", "TGA"), source, tolerance=2)
        self.assert_pixels_close(self.read("legacy.png", "PNG"), source, tolerance=2)

    def test_transparent_png_to_tga_preserves_visible_colors(self):
        source = pattern().convert("RGBA")
        source.putalpha(Image.new("L", source.size, 128))
        source.save(self.root / "source.png")
        self.run_commands(["load source.png", "save output.tga", "load output.tga", "save output.png"])
        self.assert_visible_close(self.read("output.tga", "TGA"), source, tolerance=1)
        self.assert_visible_close(self.read("output.png", "PNG"), source, tolerance=2)

    def test_missing_and_corrupt_load_preserve_current_image(self):
        source = pattern()
        source.save(self.root / "source.png")
        (self.root / "broken.jpg").write_bytes(b"This is not a JPEG file.")
        for filename in ["missing.png", "broken.jpg"]:
            with self.subTest(filename=filename):
                output = self.run_commands(
                    ["load source.png", f"load {filename}"], expect_error=True,
                    after=["save preserved.png"],
                )
                self.assertIn("Image load error:", output)
                self.assert_pixels_close(self.read("preserved.png", "PNG"), source)

    def test_unsupported_save_does_not_overwrite_or_continue_script(self):
        pattern().save(self.root / "source.png")
        (self.root / "existing.bmp").write_bytes(b"keep this file")
        output = self.run_commands(
            ["load source.png", "save existing.bmp", "save should-not-exist.png"], expect_error=True,
        )
        self.assertIn("Unsupported image extension", output)
        self.assertEqual((self.root / "existing.bmp").read_bytes(), b"keep this file")
        self.assertFalse((self.root / "should-not-exist.png").exists())

    def test_save_to_missing_directory_fails_cleanly(self):
        pattern().save(self.root / "source.png")
        for extension in ["png", "jpg"]:
            with self.subTest(extension=extension):
                output = self.run_commands(
                    ["load source.png", f"save missing/output.{extension}"], expect_error=True,
                )
                self.assertIn("Unable to save image:", output)
                self.assertFalse((self.root / f"missing/output.{extension}").exists())

    def test_single_pixel_png(self):
        source = Image.new("RGBA", (1, 1), (40, 180, 90, 128))
        source.save(self.root / "source.png")
        self.run_commands(["load source.png", "save result.png"])
        self.assert_visible_close(self.read("result.png", "PNG"), source)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, add_help=False)
    parser.add_argument("--exe", type=Path, default=EXE)
    args, remaining = parser.parse_known_args()
    EXE = args.exe.resolve()
    if not EXE.is_file():
        parser.error(f"Build the application first; executable not found: {EXE}")
    unittest.main(argv=[sys.argv[0], *remaining])
