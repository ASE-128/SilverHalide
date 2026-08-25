#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "utils/ViewportMath.h"

namespace {

using lrc::viewportmath::AnchorPan;

// Derives the image-space point that lies under the widget cursor before the
// zoom (from the pre-zoom target rect), re-anchors with anchorPan() at the new
// scale, then asserts the same image point still maps back onto the cursor.
void requireAnchored(const AnchorPan& p0, double scale0, double scale1,
                     double imageW, double imageH,
                     double viewportW, double viewportH,
                     double cursorX, double cursorY) {
    const double dst0Left = viewportW / 2.0 + p0.x - imageW * scale0 / 2.0;
    const double dst0Top = viewportH / 2.0 + p0.y - imageH * scale0 / 2.0;
    const double imgX = (cursorX - dst0Left) / scale0;
    const double imgY = (cursorY - dst0Top) / scale0;

    const AnchorPan pan1 = lrc::viewportmath::anchorPan(
        cursorX, cursorY, imgX, imgY, scale1, imageW, imageH, viewportW, viewportH);

    const double widgetX =
        viewportW / 2.0 + pan1.x - imageW * scale1 / 2.0 + imgX * scale1;
    const double widgetY =
        viewportH / 2.0 + pan1.y - imageH * scale1 / 2.0 + imgY * scale1;

    REQUIRE(std::abs(widgetX - cursorX) < 1e-9);
    REQUIRE(std::abs(widgetY - cursorY) < 1e-9);
}

} // namespace

TEST_CASE("ViewportMath::anchorPan keeps image centre under widget centre", "[viewport]") {
    // Image centre (50, 50) at 2x scale under the widget centre: pan is (0, 0).
    const AnchorPan pan = lrc::viewportmath::anchorPan(
        400.0, 300.0, 50.0, 50.0, 2.0, 100.0, 100.0, 800.0, 600.0);
    REQUIRE(std::abs(pan.x - 0.0) < 1e-9);
    REQUIRE(std::abs(pan.y - 0.0) < 1e-9);
}

TEST_CASE("ViewportMath::anchorPan identity zoom keeps anchor fixed", "[viewport]") {
    requireAnchored({10.0, -20.0}, 2.0, 2.0, 1200.0, 800.0, 900.0, 700.0, 450.0, 350.0);
    requireAnchored({0.0, 0.0}, 1.0, 1.0, 100.0, 100.0, 800.0, 600.0, 400.0, 300.0);
    requireAnchored({-321.5, 87.25}, 0.75, 0.75, 640.0, 480.0, 900.0, 700.0, 130.0, 540.0);
}

TEST_CASE("ViewportMath::anchorPan zooming in keeps anchor fixed", "[viewport]") {
    requireAnchored({-150.0, 80.0}, 0.25, 2.5, 1920.0, 1080.0, 1280.0, 800.0, 640.0, 400.0);
    requireAnchored({500.0, -300.0}, 0.5, 4.0, 1000.0, 1000.0, 500.0, 500.0, 250.0, 250.0);
    requireAnchored({42.0, 42.0}, 1.0, 6.25, 800.0, 600.0, 900.0, 700.0, 300.0, 200.0);
}

TEST_CASE("ViewportMath::anchorPan zooming out keeps anchor fixed", "[viewport]") {
    requireAnchored({30.0, 30.0}, 4.0, 0.4, 800.0, 600.0, 1000.0, 800.0, 100.0, 700.0);
    requireAnchored({-20.0, 15.0}, 3.0, 0.75, 640.0, 480.0, 900.0, 700.0, 300.0, 200.0);
    requireAnchored({0.0, 0.0}, 2.0, 0.125, 1000.0, 1000.0, 800.0, 600.0, 400.0, 300.0);
}

TEST_CASE("ViewportMath::anchorPan with cursor over image centre", "[viewport]") {
    // Image fills the viewport at scale 1, so the widget centre is the image centre.
    requireAnchored({0.0, 0.0}, 1.0, 3.0, 800.0, 600.0, 800.0, 600.0, 400.0, 300.0);
    requireAnchored({123.0, -45.0}, 2.0, 0.5, 800.0, 600.0, 800.0, 600.0, 400.0, 300.0);
    requireAnchored({-100.0, 200.0}, 1.5, 1.5, 800.0, 600.0, 800.0, 600.0, 400.0, 300.0);
}

TEST_CASE("ViewportMath::anchorPan with cursor near image corner", "[viewport]") {
    // Image fills the viewport, so a cursor near (0, 0) sits near the top-left
    // image corner (a few pixels inside to stay on the image).
    requireAnchored({0.0, 0.0}, 1.0, 2.5, 800.0, 600.0, 800.0, 600.0, 3.0, 2.0);
    requireAnchored({0.0, 0.0}, 1.0, 0.4, 800.0, 600.0, 800.0, 600.0, 796.0, 597.0);
    requireAnchored({50.0, -60.0}, 1.0, 3.0, 800.0, 600.0, 800.0, 600.0, 5.0, 8.0);
}

TEST_CASE("ViewportMath::anchorPan with viewport smaller than image", "[viewport]") {
    // Image overflows a small viewport; the anchor must still hold.
    requireAnchored({-1200.0, -900.0}, 0.5, 1.5, 4000.0, 3000.0, 640.0, 480.0, 320.0, 240.0);
    requireAnchored({2000.0, 1500.0}, 2.0, 0.5, 4000.0, 3000.0, 640.0, 480.0, 100.0, 400.0);
    requireAnchored({0.0, 0.0}, 0.25, 2.0, 8000.0, 6000.0, 640.0, 480.0, 640.0, 0.0);
}

TEST_CASE("ViewportMath::anchorPan with non-integer coordinates", "[viewport]") {
    requireAnchored({-12.345, 67.891}, 1.25, 2.75, 1234.567, 987.654, 765.432, 543.210,
                    345.678, 210.987);
    requireAnchored({0.001, -0.002}, 0.4, 0.1, 1234.567, 987.654, 765.432, 543.210,
                    0.5, 543.0);
    requireAnchored({999.999, -999.999}, 2.5, 3.5, 1234.567, 987.654, 765.432, 543.210,
                    765.4, 0.1);
}

TEST_CASE("ViewportMath::anchorPan with tiny images", "[viewport]") {
    requireAnchored({0.0, 0.0}, 1.0, 8.0, 4.0, 4.0, 800.0, 600.0, 400.0, 300.0);
    requireAnchored({-0.25, 0.125}, 16.0, 0.5, 0.5, 0.5, 800.0, 600.0, 399.75, 300.25);
    requireAnchored({0.0, 0.0}, 2.0, 2.0, 1.0, 1.0, 640.0, 480.0, 320.0, 240.0);
}

TEST_CASE("ViewportMath::wheelShouldSnapBack fires only on zoom-out", "[viewport]") {
    // Zoom-out (dy < 0) at or inside the fit band snaps back.
    REQUIRE(lrc::viewportmath::wheelShouldSnapBack(-120.0, 0.9, 1.0));
    REQUIRE(lrc::viewportmath::wheelShouldSnapBack(-120.0, 1.05, 1.0));
    // Zoom-out already above the fit band does not snap back.
    REQUIRE(!lrc::viewportmath::wheelShouldSnapBack(-120.0, 1.1, 1.0));
    // Zoom-in (dy > 0) never snaps back, even inside the fit band: with a fit
    // scale >= ~1.0 an upward wheel would otherwise bounce straight back to fit
    // and wheel zoom-in would be silently dead.
    REQUIRE(!lrc::viewportmath::wheelShouldSnapBack(120.0, 0.9, 1.0));
    REQUIRE(!lrc::viewportmath::wheelShouldSnapBack(120.0, 1.04, 1.0));
    REQUIRE(!lrc::viewportmath::wheelShouldSnapBack(60.0, 0.5, 1.0));
    // Zero vertical delta (horizontal scroll) never snaps back.
    REQUIRE(!lrc::viewportmath::wheelShouldSnapBack(0.0, 0.5, 1.0));
}

TEST_CASE("ViewportMath::imagePointInBounds accepts clicks on the image", "[viewport]") {
    REQUIRE(lrc::viewportmath::imagePointInBounds(0.0, 0.0, 1920.0, 1080.0));
    REQUIRE(lrc::viewportmath::imagePointInBounds(1919.0, 1079.0, 1920.0, 1080.0));
    REQUIRE(lrc::viewportmath::imagePointInBounds(640.0, 360.0, 1920.0, 1080.0));
    // Half-open range: the right/bottom edge itself is out of bounds.
    REQUIRE(!lrc::viewportmath::imagePointInBounds(1920.0, 0.0, 1920.0, 1080.0));
    REQUIRE(!lrc::viewportmath::imagePointInBounds(0.0, 1080.0, 1920.0, 1080.0));
    // Letterbox / negative coordinates are out of bounds.
    REQUIRE(!lrc::viewportmath::imagePointInBounds(-1.0, 0.0, 1920.0, 1080.0));
    REQUIRE(!lrc::viewportmath::imagePointInBounds(0.0, -0.5, 1920.0, 1080.0));
    REQUIRE(!lrc::viewportmath::imagePointInBounds(3000.0, 400.0, 1920.0, 1080.0));
    // Null image (zero dimensions): nothing is in bounds.
    REQUIRE(!lrc::viewportmath::imagePointInBounds(0.0, 0.0, 0.0, 0.0));
}
