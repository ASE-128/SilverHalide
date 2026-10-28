#include "utils/AdjustmentTransfer.h"

#include "engine/Adjustments.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Adjustment transfer preserves fields outside selected categories",
          "[adjustments][sync]") {
    lrc::Adjustments source;
    source.exposure = 1.25;
    source.temperature = 18.0;
    source.sharpness = 55.0;
    source.grain = 24.0;

    lrc::Adjustments target;
    target.exposure = -0.5;
    target.temperature = -12.0;
    target.sharpness = 12.0;
    target.grain = 3.0;

    lrc::copyAdjustmentCategories(source, target,
                                  lrc::AdjustmentCategory::Basic |
                                      lrc::AdjustmentCategory::Detail);

    CHECK(target.exposure == 1.25);
    CHECK(target.sharpness == 55.0);
    CHECK(target.temperature == -12.0);
    CHECK(target.grain == 3.0);
}

TEST_CASE("Adjustment transfer copies complex data only by category",
          "[adjustments][sync]") {
    lrc::Adjustments source;
    source.curve = {{0.0f, 0.1f}, {1.0f, 0.9f}};
    source.colorTargets.push_back(lrc::ColorTarget{});
    source.colorTargets.back().enabled = true;
    source.cropRect = {0.1f, 0.2f, 0.8f, 0.9f};
    source.straightenAngle = 2.5;
    source.masks.emplace_back();
    source.masks.back().opacity = 0.25;
    source.spotRemovals.push_back(lrc::SpotRemoval{});
    source.spotRemovals.back().targetX = 0.8f;

    lrc::Adjustments target;
    target.masks.emplace_back();
    target.masks.back().opacity = 0.75;
    target.cropRect = {0.0f, 0.0f, 0.6f, 1.0f};
    target.straightenAngle = 0.0;
    target.spotRemovals.push_back(lrc::SpotRemoval{});
    target.spotRemovals.back().targetX = 0.2f;

    lrc::copyAdjustmentCategories(source, target,
                                  lrc::AdjustmentCategory::ToneCurve |
                                      lrc::AdjustmentCategory::Color |
                                      lrc::AdjustmentCategory::Crop);
    CHECK(target.curve == source.curve);
    CHECK(target.colorTargets == source.colorTargets);
    CHECK(target.cropRect == source.cropRect);
    CHECK(target.straightenAngle == source.straightenAngle);
    CHECK(target.masks.front().opacity == 0.75);
    CHECK(target.spotRemovals.front().targetX == 0.2f);

    lrc::copyAdjustmentCategories(source, target,
                                  lrc::AdjustmentCategory::Masks |
                                      lrc::AdjustmentCategory::SpotRemoval);
    CHECK(target.masks == source.masks);
    CHECK(target.spotRemovals == source.spotRemovals);
}
