#include "engine/ColorManager.h"
#include "engine/CropTransform.h"
#include "engine/Pipeline.h"
#include "utils/ProfileIO.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>

TEST_CASE("Crop transform applies normalized crop without mutating input",
          "[crop]") {
    auto source = lrc::ImageBuffer::create(4, 2, lrc::PixelFormat::F32);
    for (int y = 0; y < source->height(); ++y) {
        for (int x = 0; x < source->width(); ++x) {
            const float value = static_cast<float>(x) / 3.0f;
            source->setPixel(x, y, 0, value);
            source->setPixel(x, y, 1, value);
            source->setPixel(x, y, 2, value);
        }
    }

    const auto cropped = lrc::applyCropAndStraighten(
        *source, lrc::CropRect{0.25f, 0.0f, 0.75f, 1.0f}, 0.0);
    REQUIRE(cropped);
    CHECK(cropped->width() == 2);
    CHECK(cropped->height() == 2);
    CHECK(cropped->pixel(0, 0, 0) == Catch::Approx(1.0f / 3.0f));
    CHECK(cropped->pixel(1, 0, 0) == Catch::Approx(2.0f / 3.0f));
    CHECK(source->width() == 4);
    CHECK(source->pixel(0, 0, 0) == 0.0f);
}

TEST_CASE("Crop transform rejects invalid geometry safely", "[crop]") {
    auto source = lrc::ImageBuffer::create(2, 2, lrc::PixelFormat::F32);
    source->setPixel(0, 0, 0, 0.75f);
    const auto unchanged = lrc::applyCropAndStraighten(
        *source, lrc::CropRect{0.8f, 0.0f, 0.2f, 1.0f}, 0.0);
    REQUIRE(unchanged);
    CHECK(unchanged->width() == source->width());
    CHECK(unchanged->pixel(0, 0, 0) == 0.75f);
}

TEST_CASE("Crop and straighten persist and invalidate incremental output",
          "[crop][pipeline][profileio]") {
    lrc::Adjustments adjustments;
    adjustments.cropRect = {0.0f, 0.0f, 0.75f, 1.0f};
    adjustments.straightenAngle = 2.5;
    const auto restored =
        lrc::ProfileIO::fromJson(lrc::ProfileIO::toJson(adjustments));
    CHECK(restored == adjustments);

    auto colorManager = std::make_shared<lrc::ColorManager>();
    lrc::Pipeline pipeline(colorManager);
    auto source = lrc::ImageBuffer::create(4, 2, lrc::PixelFormat::F32);
    for (int y = 0; y < source->height(); ++y) {
        for (int x = 0; x < source->width(); ++x) {
            const float value = static_cast<float>(x + y) / 5.0f;
            source->setPixel(x, y, 0, value);
            source->setPixel(x, y, 1, value);
            source->setPixel(x, y, 2, value);
        }
    }
    const auto output = pipeline.renderPreview(
        *source, adjustments, lrc::Size{4, 2}, 4, 7);
    REQUIRE(output);
    CHECK(output->width() == 3);
    CHECK(output->height() == 2);

    lrc::Adjustments changed = adjustments;
    changed.cropRect.right = 0.5f;
    CHECK(changed.hashStageF() != adjustments.hashStageF());
    const auto croppedAgain = pipeline.renderPreview(
        *source, changed, lrc::Size{4, 2}, 4, 7);
    REQUIRE(croppedAgain);
    CHECK(croppedAgain->width() == 2);
}
