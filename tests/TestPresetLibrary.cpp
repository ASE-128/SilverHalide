#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "engine/Adjustments.h"
#include "engine/ZoneCurve.h"
#include "utils/PresetLibrary.h"
#include "utils/ProfileIO.h"

using namespace Catch;
using namespace lrc;

namespace {

// Deletes the directory (and everything in it) on destruction, also when an
// assertion fails mid-test. A unique suffix keeps parallel test processes from
// colliding on the same path.
struct ScopedTempDir {
    explicit ScopedTempDir(const char* tag) {
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        root = std::filesystem::temp_directory_path() /
               ("lrc_presetlib_" + std::string(tag) + "_" +
                std::to_string(stamp));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
    }
    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    std::filesystem::path root;
};

// Writes raw text to `path` (bypasses PresetLibrary so tests can plant
// corrupt / foreign files directly).
bool writeFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

// A non-trivial stack: several panels + a tone curve + an HSL channel.
Adjustments sampleStack() {
    Adjustments a;
    a.exposure = 0.5;
    a.contrast = 25.0;
    a.highlights = -40.0;
    a.clarity = 30.0;
    a.vibrance = 5.0;
    a.sharpness = 40.0;
    a.curve = zonecurve::fromOutputs(0.2f, 0.4f, 0.6f, 0.8f);
    a.hsl[static_cast<size_t>(ColorRange::Green)].hue = 8.0;
    a.splitTone.shadowSaturation = 30.0;
    return a;
}

} // namespace

TEST_CASE("PresetLibrary: save/list/load/overwrite/delete round-trip",
          "[presetlib]") {
    ScopedTempDir dir("roundtrip");
    PresetLibrary lib(dir.root);

    // An empty library lists nothing.
    REQUIRE(lib.listPresets().empty());
    REQUIRE_FALSE(lib.loadPreset("Missing").has_value());
    REQUIRE_FALSE(lib.deletePreset("Missing"));  // deleting a ghost is not ok

    // Save, then the preset appears in the list and round-trips field-by-field.
    const Adjustments a = sampleStack();
    REQUIRE(lib.savePreset("Golden Hour", a));
    REQUIRE(lib.exists("Golden Hour"));
    const auto names = lib.listPresets();
    REQUIRE(names.size() == 1);
    REQUIRE(names[0] == "Golden Hour");

    const auto loaded = lib.loadPreset("Golden Hour");
    REQUIRE(loaded.has_value());
    REQUIRE(*loaded == a);

    // Saving again under the same name overwrites (names stay unique).
    Adjustments tweaked = a;
    tweaked.exposure = -0.75;
    REQUIRE(lib.savePreset("Golden Hour", tweaked));
    REQUIRE(lib.listPresets().size() == 1);
    const auto reloaded = lib.loadPreset("Golden Hour");
    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->exposure == Approx(-0.75).margin(1e-6));
    REQUIRE(reloaded->contrast == Approx(25.0).margin(1e-6));

    // Delete: the preset disappears from both list and disk.
    REQUIRE(lib.deletePreset("Golden Hour"));
    REQUIRE_FALSE(lib.exists("Golden Hour"));
    REQUIRE(lib.listPresets().empty());
    REQUIRE_FALSE(lib.loadPreset("Golden Hour").has_value());
    // A second delete of the same name reports failure.
    REQUIRE_FALSE(lib.deletePreset("Golden Hour"));
}

TEST_CASE("PresetLibrary: exportPreset copies to an arbitrary path",
          "[presetlib]") {
    ScopedTempDir dir("export");
    PresetLibrary lib(dir.root);

    const Adjustments a = sampleStack();
    REQUIRE(lib.savePreset("Share Me", a));

    // Export to a location outside the library, with a different extension.
    const auto dest = dir.root.parent_path() / "shared_elsewhere.xmp";
    REQUIRE(lib.exportPreset("Share Me", dest));
    const auto copied = ProfileIO::loadPreset(dest);
    REQUIRE(copied.has_value());
    REQUIRE(*copied == a);

    // The library itself is untouched by an export.
    REQUIRE(lib.listPresets().size() == 1);

    // Exporting a missing preset fails cleanly.
    REQUIRE_FALSE(lib.exportPreset("NoSuchPreset", dest));

    std::error_code ec;
    std::filesystem::remove(dest, ec);
}

TEST_CASE("PresetLibrary: invalid names are rejected and sanitized",
          "[presetlib]") {
    ScopedTempDir dir("names");
    PresetLibrary lib(dir.root);
    const Adjustments a = Adjustments::defaults();

    // Empty / whitespace-only names are invalid.
    REQUIRE_FALSE(PresetLibrary::isValidName(""));
    REQUIRE_FALSE(PresetLibrary::isValidName("   "));
    // Path separators and Windows-illegal characters are rejected.
    REQUIRE_FALSE(PresetLibrary::isValidName("a/b"));
    REQUIRE_FALSE(PresetLibrary::isValidName("a\\b"));
    REQUIRE_FALSE(PresetLibrary::isValidName("a:b"));
    REQUIRE_FALSE(PresetLibrary::isValidName("a*b"));
    // Hidden files (leading dot) would break the name <-> file mapping.
    REQUIRE_FALSE(PresetLibrary::isValidName(".hidden"));

    REQUIRE(PresetLibrary::isValidName("Golden Hour"));
    REQUIRE(PresetLibrary::isValidName("portrait-v1"));

    // Rejected names never touch the disk.
    REQUIRE_FALSE(lib.savePreset("a/b", a));
    REQUIRE(lib.listPresets().empty());

    // sanitizeName maps illegal characters to '_' deterministically and trims.
    REQUIRE(PresetLibrary::sanitizeName("a/b:c") == "a_b_c");
    REQUIRE(PresetLibrary::sanitizeName("  fine name  ") == "fine name");
    REQUIRE(PresetLibrary::sanitizeName("///") == "___");
    REQUIRE(PresetLibrary::sanitizeName("").empty());
}

TEST_CASE("PresetLibrary: corrupted preset files do not crash and are skipped",
          "[presetlib]") {
    ScopedTempDir dir("corrupt");
    PresetLibrary lib(dir.root);

    // A corrupt .lrp file loads as nullopt (no exception / crash).
    REQUIRE(writeFile(lib.pathFor("broken"), "this is not json {"));
    REQUIRE_FALSE(lib.loadPreset("broken").has_value());

    // Load of a valid preset next to the broken one still works.
    const Adjustments a = sampleStack();
    REQUIRE(lib.savePreset("ok", a));
    const auto loaded = lib.loadPreset("ok");
    REQUIRE(loaded.has_value());
    REQUIRE(*loaded == a);

    // Foreign files and .tmp leftovers are ignored by the listing; a manual
    // valid edit still wins, so a corrupted preset is user-fixable.
    REQUIRE(writeFile(lib.pathFor("broken"), "{}"));  // valid JSON now
    const auto repaired = lib.loadPreset("broken");
    REQUIRE(repaired.has_value());
    REQUIRE(*repaired == Adjustments::defaults());

    // Stale temp file (atomic-write leftover) is neither listed nor loadable.
    REQUIRE(writeFile(dir.root / "ghost.lrp.tmp", "{}"));
    REQUIRE(writeFile(dir.root / ".hidden.lrp", "{}"));
    const auto names = lib.listPresets();
    REQUIRE(std::find(names.begin(), names.end(), "broken") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "ghost") == names.end());
    REQUIRE(std::find(names.begin(), names.end(), ".hidden") == names.end());
}

TEST_CASE("PresetLibrary: default root resolves to a usable directory",
          "[presetlib]") {
    // The user-preset root must be a usable directory path on this platform,
    // and constructing a library on it must not throw (it may already contain
    // presets from a real session, so no emptiness is asserted).
    const auto root = PresetLibrary::defaultRoot();
    REQUIRE_FALSE(root.empty());
    PresetLibrary lib(root);
    (void)lib.listPresets();
}
