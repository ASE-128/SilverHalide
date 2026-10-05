#include "ui/ImageViewport.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QWheelEvent>

#include "app/Constants.h"
#include "engine/Pipeline.h"
#include "utils/MathUtils.h"
#include "utils/ViewportMath.h"

namespace {

// Default offset of a newly created spot's source point from its
// target, expressed as a multiple of the spot's radius (placed to the right).
constexpr double kSpotDefaultSourceOffset = 2.5;

// Handle radius (widget px) added to a spot circle during hit-testing
// so a small circle stays grabbable.
constexpr double kSpotHandlePx = 6.0;

// Snap range (widget px) for grabbing the Before/After divider.
constexpr double kDividerGrabPx = 4.0;

// sRGB display value -> linear RGB (EOTF), the same transfer the renderer
// applies before the linear-space sub-mask weights. Shared by the red weight
// overlay and the luminance eyedropper.
float srgbToLinearChannel(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// Euclidean distance between two points (QPointF has no length() in Qt 6).
double pointDistSq(const QPointF& a, const QPointF& b) {
    const QPointF d = a - b;
    return d.x() * d.x() + d.y() * d.y();
}

// Invert a display channel (255 - v) and push the contrast around mid-grey so
// dust specks pop against the background. Used by the "visualize spots" mode.
int invertHighContrast(int v) {
    const double n = 1.0 - v / 255.0;      // invert
    const double c = (n - 0.5) * 2.0 + 0.5; // high-contrast around 50%
    return static_cast<int>(lrc::clamp(c, 0.0, 1.0) * 255.0);
}

// Hue angle (degrees, [0,360)) of an RGB colour.
float rgbHueDeg(const QColor& c) {
    const float r = c.redF(), g = c.greenF(), b = c.blueF();
    const float mx = std::max({r, g, b});
    const float mn = std::min({r, g, b});
    const float range = mx - mn;
    if (range < 1e-6f)
        return 0.0f;
    float h = 0.0f;
    if (mx == r)
        h = 60.0f * (g - b) / range;
    else if (mx == g)
        h = 60.0f * (b - r) / range + 120.0f;
    else
        h = 60.0f * (r - g) / range + 240.0f;
    if (h < 0.0f)
        h += 360.0f;
    return h;
}

} // namespace

// Paints the classic photo-editor checkerboard behind transparent areas.
void paintCheckerboard(QPainter& painter, const QRect& rect) {
    const int cell = 10;
    QColor a(48, 48, 48);
    QColor b(56, 56, 56);
    for (int y = rect.top(); y < rect.bottom(); y += cell) {
        for (int x = rect.left(); x < rect.right(); x += cell) {
            const bool even = (((x - rect.left()) / cell) +
                               ((y - rect.top()) / cell)) % 2 == 0;
            painter.fillRect(QRect(x, y, cell, cell), even ? a : b);
        }
    }
}

// Neutral canvas gray painted behind opaque imagery (#333333).
//
// A patterned backdrop leaks into the viewer's judgement of exposure and white
// balance, so the checkerboard is only meaningful when the photo itself has
// transparency to disclose. Opaque photos get this flat tone instead, which
// also separates the image area from the surrounding chrome (#2b2b2b).
constexpr int kCanvasGray = 51;

// Fills the photo canvas.
//
// @param showCheckerboard True when the displayed image has an alpha channel,
//        where a flat fill would hide the fact that pixels are transparent.
void paintCanvasBackground(QPainter& painter, const QRect& rect,
                           bool showCheckerboard) {
    if (showCheckerboard) {
        paintCheckerboard(painter, rect);
        return;
    }
    painter.fillRect(rect, QColor(kCanvasGray, kCanvasGray, kCanvasGray));
}

ImageViewport::ImageViewport(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    // Coalesces the AI overlay's segmentation passes while a mask parameter is
    // dragged; see aiOverlayField().
    aiFieldTimer_.setSingleShot(true);
    aiFieldTimer_.setInterval(lrc::app::throttle::kAiOverlayFieldDebounceMs);
    connect(&aiFieldTimer_, &QTimer::timeout, this,
            &ImageViewport::onAiFieldTimer);
}

void ImageViewport::setImage(const QImage& image) {
    image_ = image;
    // A new preview image is a fresh context: no outstanding full-res request.
    fullResRequested_ = false;
    // New image -> invalidate the mask overlay hash so the next rebuild
    // re-evaluates the geometry against the new dimensions. The overlay image
    // itself stays until a rebuild so mid-stroke preview updates don't flicker.
    lastOverlayHash_ = 0;
    // The AI overlay memo is keyed on the image as well; drop it and cancel any
    // deferred recompute so a stale segmentation cannot land on the new frames.
    aiFieldTimer_.stop();
    aiField_ = nullptr;
    // New image -> the cached spot-visualization copy is stale.
    spotViz_ = QImage();
    spotVizValid_ = false;
    resetView();
    // #8: resetView() keeps zoomMode_ (it only resets zoom_/panOffset_), so if
    // the view is pinned at 1:1 the fresh fullResRequested_ = false above would
    // leave the new image on its downsampled preview forever — no path would
    // ever re-request the full-res frame. Re-request it here (the emitted
    // signal is deduped and MainWindow's onFullResRequested decodes the new
    // image); a null image must not request anything.
    if (!image_.isNull() && zoomMode_ == ZoomMode::OneToOne)
        setFullResRequested(true);
    update();
}

void ImageViewport::setNativeImageSize(const QSize& size) {
    nativeSize_ = size;
    update();
}

void ImageViewport::setImageKeepView(const QImage& image) {
    if (image.isNull()) {
        setImage(image);
        return;
    }
    // Remember the image-space point under the viewport centre as a normalized
    // coordinate. The preview and the full-res buffer share the same aspect
    // ratio, so relative coordinates stay valid across the swap.
    QPointF norm(0.5, 0.5);
    if (!image_.isNull() && image_.width() > 0 && image_.height() > 0) {
        const QPointF centre(size().width() / 2.0, size().height() / 2.0);
        const QPointF imgPt = widgetToImage(centre);
        norm = QPointF(imgPt.x() / image_.width(),
                       imgPt.y() / image_.height());
    }
    const ZoomMode mode = zoomMode_;
    const double zoom = zoom_;
    image_ = image;
    // The sub-mask weight overlay depends on the pixel colours, so a swapped
    // image invalidates the cached overlay (mirrors setImage).
    lastOverlayHash_ = 0;
    // A same-image re-render (edit preview) changes pixels but not the mask or
    // the overlay geometry, and it happens on every coarse preview frame while a
    // slider is dragged. Dropping the AI memo here would turn each of those
    // frames into a fresh segmentation pass -- exactly what the debounce exists
    // to prevent -- so keep the memo and let the timer coalesce the refresh.
    if (maskTool_ != lrc::MaskType::None && aiField_)
        aiFieldTimer_.start();
    // New image -> the cached spot-visualization copy is stale.
    spotViz_ = QImage();
    spotVizValid_ = false;
    zoomMode_ = mode;
    zoom_ = zoom;
    // Recompute the pan offset so the remembered image point stays centred.
    const QSizeF scaled = QSizeF(image_.size()) * currentScale();
    panOffset_ = QPointF((0.5 - norm.x()) * scaled.width(),
                         (0.5 - norm.y()) * scaled.height());
    update();
}

void ImageViewport::setDiagnosticOverlay(const QImage& overlay) {
    overlay_ = overlay;
    update();
}

void ImageViewport::setAutoCropSuggestion(const QRectF& normalizedRect) {
    autoCropActive_ = normalizedRect.isValid();
    autoCropNorm_ = normalizedRect;
    update();
}

void ImageViewport::clearAutoCropSuggestion() {
    if (autoCropActive_) {
        autoCropActive_ = false;
        update();
    }
}

void ImageViewport::setSamplerActive(bool active) {
    if (active) {
        whiteBalanceActive_ = false;  // 与白平衡吸管互斥 (mutual exclusion).
        pointColorActive_ = false;    // 与点颜色吸管互斥 (mutual exclusion).
        luminanceActive_ = false;     // 与明亮度吸管互斥 (mutual exclusion).
        hasPointColorMark_ = false;
        spotToolActive_ = false;      // 与污点工具互斥 (mutual exclusion).
    }
    samplerActive_ = active;
    hasSamplerMark_ = false;
    sampledRange_ = -1;
    setCursor(active ? Qt::CrossCursor : Qt::ArrowCursor);
    update();
}

void ImageViewport::setSamplerBase(int rangeIndex, double saturation,
                                   double luminance) {
    sampledRange_ = rangeIndex;
    samplerBaseSat_ = saturation;
    samplerBaseLum_ = luminance;
}

void ImageViewport::setPointColorSamplerActive(bool active) {
    if (active) {
        whiteBalanceActive_ = false;  // 与白平衡吸管互斥 (mutual exclusion).
        samplerActive_ = false;       // 与 HSL 吸管互斥 (mutual exclusion).
        luminanceActive_ = false;     // 与明亮度吸管互斥 (mutual exclusion).
        hasSamplerMark_ = false;
        sampledRange_ = -1;
        spotToolActive_ = false;      // 与污点工具互斥 (mutual exclusion).
    }
    pointColorActive_ = active;
    hasPointColorMark_ = false;
    setCursor(active ? Qt::CrossCursor : Qt::ArrowCursor);
    update();
}

void ImageViewport::setWhiteBalanceSamplerActive(bool active) {
    if (active) {
        // Mutual exclusion: activating white balance disables HSL / point colour /
        // luminance samplers.
        samplerActive_ = false;
        pointColorActive_ = false;
        luminanceActive_ = false;
        hasSamplerMark_ = false;
        sampledRange_ = -1;
        hasPointColorMark_ = false;
        spotToolActive_ = false;      // 与污点工具互斥 (mutual exclusion).
    }
    whiteBalanceActive_ = active;
    setCursor(active ? Qt::CrossCursor : Qt::ArrowCursor);
    update();
}

void ImageViewport::setColorRangeSamplerActive(bool active) {
    if (active) {
        // Mutual exclusion: activating colour range disables white balance / HSL / point
        // colour / luminance samplers.
        whiteBalanceActive_ = false;
        samplerActive_ = false;
        pointColorActive_ = false;
        luminanceActive_ = false;
        hasSamplerMark_ = false;
        sampledRange_ = -1;
        hasPointColorMark_ = false;
        spotToolActive_ = false;      // 与污点工具互斥 (mutual exclusion).
    }
    colorRangeActive_ = active;
    if (!active)
        hasColorRangeMark_ = false;
    setCursor(active ? Qt::CrossCursor : Qt::ArrowCursor);
    update();
}

void ImageViewport::setLuminanceSamplerActive(bool active) {
    if (active) {
        // Mutual exclusion: activating luminance disables white balance / HSL / point
        // colour / colour range samplers.
        whiteBalanceActive_ = false;
        samplerActive_ = false;
        pointColorActive_ = false;
        colorRangeActive_ = false;
        hasSamplerMark_ = false;
        sampledRange_ = -1;
        hasPointColorMark_ = false;
        hasColorRangeMark_ = false;
        spotToolActive_ = false;      // 与污点工具互斥 (mutual exclusion).
    }
    luminanceActive_ = active;
    if (!active)
        hasLuminanceMark_ = false;
    setCursor(active ? Qt::CrossCursor : Qt::ArrowCursor);
    update();
}

void ImageViewport::setMaskColorReference(const QColor& srgb) {
    colorRangeSrgbRef_ = srgb;
    if (maskTool_ == lrc::MaskType::ColorRange) {
        rebuildMaskOverlay();
        update();
    }
}

QPointF ImageViewport::widgetToImage(const QPointF& pos) const {
    if (image_.isNull())
        return {};
    const QRectF dst = targetRect();
    if (dst.width() <= 0.0 || dst.height() <= 0.0)
        return {};
    const double u = (pos.x() - dst.left()) / dst.width();
    const double v = (pos.y() - dst.top()) / dst.height();
    return QPointF(u * image_.width(), v * image_.height());
}

void ImageViewport::sampleAt(const QPointF& pos) {
    if (image_.isNull())
        return;
    const QPointF img = widgetToImage(pos);
    const int ix = std::clamp(static_cast<int>(img.x()), 0, image_.width() - 1);
    const int iy = std::clamp(static_cast<int>(img.y()), 0, image_.height() - 1);
    const QColor color = image_.pixelColor(ix, iy);
    const int range = lrc::nearestColorRange(rgbHueDeg(color));
    samplerStart_ = pos;
    samplerMark_ = pos;
    hasSamplerMark_ = true;
    emit colorSampled(range, color);
    update();
}

void ImageViewport::samplePointColor(const QPointF& widgetPos) {
    if (image_.isNull())
        return;
    const QPointF img = widgetToImage(widgetPos);
    const int ix = std::clamp(static_cast<int>(img.x()), 0, image_.width() - 1);
    const int iy = std::clamp(static_cast<int>(img.y()), 0, image_.height() - 1);
    const QColor color = image_.pixelColor(ix, iy);
    pointColorMark_ = widgetPos;
    hasPointColorMark_ = true;
    emit pointColorSampled(color);
    update();
}

void ImageViewport::sampleWhiteBalance(const QPointF& widgetPos) {
    if (image_.isNull())
        return;
    const QPointF img = widgetToImage(widgetPos);
    const int ix = std::clamp(static_cast<int>(img.x()), 0, image_.width() - 1);
    const int iy = std::clamp(static_cast<int>(img.y()), 0, image_.height() - 1);
    const QColor color = image_.pixelColor(ix, iy);
    emit whiteBalanceSampled(color);
}

void ImageViewport::sampleColorRange(const QPointF& widgetPos) {
    if (image_.isNull())
        return;
    const QPointF img = widgetToImage(widgetPos);
    const int ix = static_cast<int>(img.x());
    const int iy = static_cast<int>(img.y());
    if (ix < 0 || iy < 0 || ix >= image_.width() || iy >= image_.height())
        return;  // 越界点击忽略 (out-of-bounds click ignored).
    const QColor c(image_.pixelColor(ix, iy));
    colorRangeSrgbRef_ = c;
    hasColorRangeMark_ = true;
    colorRangeMark_ = widgetPos;
    emit colorRangeSampled(c);
    update();
}

void ImageViewport::sampleLuminance(const QPointF& widgetPos) {
    if (image_.isNull())
        return;
    const QPointF img = widgetToImage(widgetPos);
    const int ix = static_cast<int>(img.x());
    const int iy = static_cast<int>(img.y());
    if (ix < 0 || iy < 0 || ix >= image_.width() || iy >= image_.height())
        return;  // 越界点击忽略 (out-of-bounds click ignored).
    const QColor c(image_.pixelColor(ix, iy));
    // The luminance reference is defined in *linear* space (the pipeline's
    // sub-mask weight evaluates L against the linear ProPhoto base), so the
    // display sRGB sample is linearized first (Rec.709 luma coefficients).
    const float L = 0.2126f * srgbToLinearChannel(c.redF()) +
                    0.7152f * srgbToLinearChannel(c.greenF()) +
                    0.0722f * srgbToLinearChannel(c.blueF());
    luminanceMark_ = widgetPos;
    hasLuminanceMark_ = true;
    emit luminanceSampled(static_cast<double>(L) * 100.0);  // 0-100 percent.
    update();
}

// --- Mask (local adjustment) editing ----------------------------------------

void ImageViewport::setMaskTool(lrc::MaskType tool) {
    maskTool_ = tool;
    maskDragging_ = false;
    // Tool switch: invalidate the cached overlay hash so the next rebuild
    // starts fresh for the new tool's geometry (forced rebuild).
    lastOverlayHash_ = 0;
    if (tool == lrc::MaskType::None) {
        maskOverlay_ = QImage();
        setCursor(Qt::ArrowCursor);
    } else {
        spotToolActive_ = false;  // 与污点工具互斥 (mutual exclusion).
        setCursor(Qt::CrossCursor);
        rebuildMaskOverlay();
    }
    update();
}

void ImageViewport::setMaskDraft(const lrc::Mask& mask) {
    maskDraft_ = mask;
    rebuildMaskOverlay();
    update();
}

void ImageViewport::setMaskBrushSize(double normalizedRadius) {
    maskBrushRadius_ =
        lrc::clamp(normalizedRadius, 0.005, 0.5);
    update();
}

void ImageViewport::setMaskBrushSoftness(double softness) {
    maskBrushSoftness_ = lrc::clamp(softness, 0.0, 1.0);
    update();
}

void ImageViewport::setMaskOverlayVisible(bool visible) {
    maskOverlayVisible_ = visible;
    update();
}

QPointF ImageViewport::normToWidget(const QPointF& n) const {
    const QRectF dst = targetRect();
    return QPointF(dst.left() + n.x() * dst.width(),
                   dst.top() + n.y() * dst.height());
}

std::shared_ptr<const std::vector<float>>
ImageViewport::aiOverlayField(int ow, int oh) {
    if (ow <= 0 || oh <= 0 || image_.isNull())
        return {};
    const size_t geomHash = maskDraft_.geometryHash();
    const qint64 imageKey = image_.cacheKey();
    if (aiField_ && aiFieldHash_ == geomHash && aiFieldImageKey_ == imageKey &&
        aiFieldW_ == ow && aiFieldH_ == oh)
        return aiField_;

    // Cache miss. When a field for the same image and overlay size is still
    // available, the change came from a mask parameter (feather / threshold /
    // click prompt) rather than from a new context: keep showing those weights
    // and coalesce the recompute. A model-backed segmentation would otherwise
    // run once per slider tick on the GUI thread and freeze the window.
    // The image key is deliberately not required to match: setImageKeepView
    // swaps in a re-rendered preview of the same photo, and the refresh is
    // handled by the timer started there. A genuinely new photo goes through
    // setImage, which clears the memo outright.
    if (aiField_ && aiFieldW_ == ow && aiFieldH_ == oh) {
        aiFieldTimer_.start();
        return aiField_;
    }
    // No usable previous field (first build, new image or new size): the overlay
    // would be blank, so pay for the pass now instead of deferring it.
    aiFieldTimer_.stop();
    return recomputeAiField(ow, oh);
}

std::shared_ptr<const std::vector<float>>
ImageViewport::recomputeAiField(int ow, int oh) {
    if (ow <= 0 || oh <= 0 || image_.isNull())
        return {};
    // Segment a small copy of the preview: AiMaskGenerator accepts any
    // ImageBuffer format and normalises internally, and sizing the input to the
    // overlay keeps the inference input as small as what we can display.
    const QImage small =
        image_.scaled(ow, oh, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
            .convertToFormat(QImage::Format_RGB888);
    if (small.isNull())
        return {};
    auto src = lrc::ImageBuffer::create(ow, oh, lrc::PixelFormat::U8);
    if (!src)
        return {};
    auto& dst = src->data();
    for (int y = 0; y < oh; ++y) {
        std::memcpy(dst.data() + static_cast<size_t>(y) * ow * 3,
                    small.constScanLine(y), static_cast<size_t>(ow) * 3);
    }

    aiField_ = lrc::Pipeline::aiMaskWeightField(maskDraft_, *src);
    aiFieldHash_ = maskDraft_.geometryHash();
    aiFieldImageKey_ = image_.cacheKey();
    aiFieldW_ = ow;
    aiFieldH_ = oh;
    return aiField_;
}

void ImageViewport::onAiFieldTimer() {
    if (image_.isNull() || aiFieldW_ <= 0 || aiFieldH_ <= 0)
        return;
    recomputeAiField(aiFieldW_, aiFieldH_);
    // The overlay on screen was painted from the stale weights, and its
    // geometry hash already equals the current draft's, so clear the dedup key
    // to make the recomputed field visible.
    lastOverlayHash_ = 0;
    rebuildMaskOverlay();
    update();
}

void ImageViewport::rebuildMaskOverlay() {
    if (image_.isNull() || maskTool_ == lrc::MaskType::None) {
        maskOverlay_ = QImage();
        return;
    }
    if (maskDraft_.type != maskTool_) {
        maskOverlay_ = QImage();  // stale overlay from another tool: clear it
        return;
    }
    // Colour-range overlays depend on the live reference colour rather than on
    // mask geometry, so they must bypass the geometry-hash dedup below.
    const bool colorRangeMask = maskDraft_.type == lrc::MaskType::ColorRange;
    // A geometry mask with colour/luminance sub-mask modifiers draws its combined
    // weight (geometry x sub-masks), which depends on the pixel colours as well
    // as the geometry.
    const bool hasSubMask =
        maskDraft_.useColorRange || maskDraft_.useLuminance;
    lrc::ColorRangeMask colorRange = maskDraft_.color;
    // A colour-range mask is inert until a reference colour has been sampled:
    // Pipeline::colorRangeWeight returns 0 without one. Forcing hasReference
    // with an unset reference (the previous behaviour) produced a meaningless
    // speckle over dark pixels instead of an empty overlay.
    if (colorRangeMask && colorRangeSrgbRef_.isValid()) {
        colorRange.hasReference = true;
        colorRange.refR = srgbToLinearChannel(colorRangeSrgbRef_.redF());
        colorRange.refG = srgbToLinearChannel(colorRangeSrgbRef_.greenF());
        colorRange.refB = srgbToLinearChannel(colorRangeSrgbRef_.blueF());
    }
    // Geometry + sub-mask parameters unchanged since the last build: keep the
    // existing overlay. Sub-mask references / ranges / mode live in the draft
    // (covered by geometryHash) and a swapped image resets lastOverlayHash_, so
    // the cached overlay cannot go stale. The standalone colour-range mask
    // bypasses this (its reference is separate viewport state).
    if (!colorRangeMask && !maskOverlay_.isNull() &&
        maskDraft_.geometryHash() == lastOverlayHash_)
        return;
    const int w = image_.width();
    const int h = image_.height();
    if (w <= 0 || h <= 0)
        return;
    // Downscale: cap the overlay's longer edge at 480 px (aspect kept) so the
    // per-pixel weight fill stays cheap on large images.
    const int maxDim = std::max(w, h);
    int ow = w, oh = h;
    if (maxDim > 480) {
        const double r = 480.0 / maxDim;
        ow = std::max(1, static_cast<int>(w * r));
        oh = std::max(1, static_cast<int>(h * r));
    }
    if (!colorRangeMask && !hasSubMask) {
        // AI masks are content-driven: their weight comes from the pixels, so
        // they route through the engine's AI entry point (model when available,
        // heuristic otherwise). Everything else rasterises from geometry alone.
        auto field = maskDraft_.type == lrc::MaskType::AiMask
                         ? aiOverlayField(ow, oh)
                         : lrc::Pipeline::brushWeightField(maskDraft_, ow, oh);
        if (!field)
            return;
        maskOverlay_ = QImage(ow, oh, QImage::Format_ARGB32_Premultiplied);
        maskOverlay_.fill(Qt::transparent);
        for (int y = 0; y < oh; ++y) {
            for (int x = 0; x < ow; ++x) {
                const float weight = (*field)[y * ow + x];
                if (weight <= 0.01f)
                    continue;
                // LR-style red overlay; stronger weight -> more opaque.
                const int alpha = static_cast<int>(weight * 150.0f);
                maskOverlay_.setPixel(x, y, qRgba(255, 0, 0, alpha));
            }
        }
        lastOverlayHash_ = maskDraft_.geometryHash();
        return;
    }
    // Colour-range mask: weight comes from per-pixel hue/sat/luma distance to
    // the sampled reference colour (no brushWeightField / engine cache).
    // Sub-mask geometry mask: weight comes from the cached geometry field
    // combined per pixel with the colour/luminance sub-mask weights (product
    // for intersection, max for union). Using the cached field (instead of
    // Pipeline::maskWeight per pixel) keeps a multi-dab brush overlay cheap.
    // An AI mask is content-driven in this branch too: it needs the same engine
    // entry point, because brushWeightField has no pixels to segment and would
    // paint its constant 0.5 placeholder -- i.e. the whole frame selected --
    // multiplied by the sub-mask, while the render applies the real field.
    std::shared_ptr<const std::vector<float>> field;
    if (!colorRangeMask) {
        field = maskDraft_.type == lrc::MaskType::AiMask
                    ? aiOverlayField(ow, oh)
                    : lrc::Pipeline::brushWeightField(maskDraft_, ow, oh);
        if (!field) {
            // Nothing to combine the sub-mask with. Leave the overlay empty
            // rather than substituting a whole-frame weight of 1.
            maskOverlay_ = QImage();
            lastOverlayHash_ = 0;
            return;
        }
    }
    maskOverlay_ = QImage(ow, oh, QImage::Format_ARGB32_Premultiplied);
    maskOverlay_.fill(Qt::transparent);
    for (int y = 0; y < oh; ++y) {
        for (int x = 0; x < ow; ++x) {
            const size_t i = static_cast<size_t>(y) * ow + x;
            const QColor px = image_.pixelColor(std::min(w - 1, (x * w) / ow),
                                                std::min(h - 1, (y * h) / oh));
            const float lr = srgbToLinearChannel(px.redF());
            const float lg = srgbToLinearChannel(px.greenF());
            const float lb = srgbToLinearChannel(px.blueF());
            float wgt;
            if (colorRangeMask) {
                wgt = lrc::Pipeline::colorRangeWeight(colorRange, lr, lg, lb);
            } else {
                const float geo = field ? (*field)[i] : 1.0f;
                if (maskDraft_.maskUnion) {
                    float best = geo;
                    if (maskDraft_.useColorRange)
                        best = std::max(
                            best, lrc::Pipeline::colorSubMaskWeight(
                                      maskDraft_, lr, lg, lb));
                    if (maskDraft_.useLuminance)
                        best = std::max(
                            best, lrc::Pipeline::luminanceSubMaskWeight(
                                      maskDraft_, lr, lg, lb));
                    wgt = best;
                } else {
                    wgt = geo;
                    if (maskDraft_.useColorRange)
                        wgt *= lrc::Pipeline::colorSubMaskWeight(
                            maskDraft_, lr, lg, lb);
                    if (maskDraft_.useLuminance)
                        wgt *= lrc::Pipeline::luminanceSubMaskWeight(
                            maskDraft_, lr, lg, lb);
                }
            }
            if (wgt <= 0.01f)
                continue;
            // LR-style red overlay; stronger weight -> more opaque.
            const int alpha = static_cast<int>(wgt * 150.0f);
            maskOverlay_.setPixel(x, y, qRgba(255, 0, 0, alpha));
        }
    }
    // The standalone colour-range overlay's reference lives in the viewport
    // (colorRangeSrgbRef_, not in the draft), so it must rebuild every frame.
    // Sub-mask overlays keep the geometry-hash dedup: the reference / range /
    // mode all live in the draft, and a swapped image resets lastOverlayHash_
    // (setImage / setImageKeepView), so a stale overlay cannot survive a
    // content change.
    lastOverlayHash_ = colorRangeMask ? 0 : maskDraft_.geometryHash();
}

void ImageViewport::beginMaskGesture(const QPointF& widgetPos) {
    if (image_.isNull())
        return;
    const QPointF img = widgetToImage(widgetPos);
    const QPointF n(static_cast<float>(img.x()) / image_.width(),
                    static_cast<float>(img.y()) / image_.height());
    maskPress_ = widgetPos;
    maskDragging_ = true;
    switch (maskTool_) {
        case lrc::MaskType::LinearGradient:
            maskDraft_.type = lrc::MaskType::LinearGradient;
            maskDraft_.linear.startX = static_cast<float>(n.x());
            maskDraft_.linear.startY = static_cast<float>(n.y());
            maskDraft_.linear.endX = static_cast<float>(n.x());
            maskDraft_.linear.endY = static_cast<float>(n.y());
            break;
        case lrc::MaskType::RadialGradient:
            maskDraft_.type = lrc::MaskType::RadialGradient;
            maskDraft_.radial.centerX = static_cast<float>(n.x());
            maskDraft_.radial.centerY = static_cast<float>(n.y());
            maskDraft_.radial.radiusX = 0.0f;
            maskDraft_.radial.radiusY = 0.0f;
            break;
        case lrc::MaskType::Brush:
            maskDraft_.type = lrc::MaskType::Brush;
            maskDraft_.brush.push_back(lrc::BrushDab{
                static_cast<float>(n.x()), static_cast<float>(n.y()),
                static_cast<float>(maskBrushRadius_),
                static_cast<float>(maskBrushSoftness_)});
            maskLastDab_ = widgetPos;
            break;
        default:
            // Click-prompt (MobileSAM) mask: a press adds one SamPoint at the
            // click, labelled foreground/background by the armed prompt mode.
            if (maskTool_ == lrc::MaskType::AiMask &&
                maskDraft_.type == lrc::MaskType::AiMask &&
                maskDraft_.ai.target == lrc::AiMaskTarget::Prompt) {
                maskDraft_.ai.points.push_back(lrc::SamPoint{
                    static_cast<float>(n.x()), static_cast<float>(n.y()),
                    maskPromptLabel_});
            }
            break;
    }
    rebuildMaskOverlay();
    emitMaskGeometry();
    update();
}

void ImageViewport::moveMaskGesture(const QPointF& widgetPos) {
    if (image_.isNull())
        return;
    const QPointF img = widgetToImage(widgetPos);
    const QPointF n(static_cast<float>(img.x()) / image_.width(),
                    static_cast<float>(img.y()) / image_.height());
    switch (maskTool_) {
        case lrc::MaskType::LinearGradient:
            maskDraft_.linear.endX = static_cast<float>(n.x());
            maskDraft_.linear.endY = static_cast<float>(n.y());
            break;
        case lrc::MaskType::RadialGradient: {
            maskDraft_.radial.radiusX =
                std::fabs(static_cast<float>(n.x()) - maskDraft_.radial.centerX);
            maskDraft_.radial.radiusY =
                std::fabs(static_cast<float>(n.y()) - maskDraft_.radial.centerY);
            break;
        }
        case lrc::MaskType::Brush: {
            // Add a dab only when the pointer moved enough in widget space
            // (4 px manhattan) AND far enough in image space (normalized
            // spacing >= 0.6x brush radius, so large brushes merge dabs).
            const bool widgetFar =
                (widgetPos - maskLastDab_).manhattanLength() >= 4.0;
            bool imageFar = false;
            const int minDim = std::min(image_.width(), image_.height());
            if (minDim > 0) {
                const QPointF now = widgetToImage(widgetPos);
                const QPointF last = widgetToImage(maskLastDab_);
                const double dx = now.x() - last.x();
                const double dy = now.y() - last.y();
                const double spacing = std::sqrt(dx * dx + dy * dy) /
                                       static_cast<double>(minDim);
                imageFar = spacing >= 0.6 * maskBrushRadius_;
            }
            if (widgetFar && imageFar) {
                maskDraft_.brush.push_back(lrc::BrushDab{
                    static_cast<float>(n.x()), static_cast<float>(n.y()),
                    static_cast<float>(maskBrushRadius_),
                    static_cast<float>(maskBrushSoftness_)});
                maskLastDab_ = widgetPos;
            }
            break;
        }
        default:
            break;
    }
    rebuildMaskOverlay();
    emitMaskGeometry();
    update();
}

void ImageViewport::endMaskGesture() {
    if (!maskDragging_)
        return;
    maskDragging_ = false;
    rebuildMaskOverlay();
    emitMaskGeometry();
    emit maskEditFinished();
    update();
}

void ImageViewport::emitMaskGeometry() {
    if (maskDraft_.type != lrc::MaskType::None)
        emit maskGeometryChanged(maskDraft_);
}

void ImageViewport::resetView() {
    zoom_ = 1.0;
    panOffset_ = QPointF(0, 0);
    update();
}

void ImageViewport::setZoom(double factor) {
    zoom_ = qBound(0.05, factor, 32.0);
    emit zoomChanged(zoom_);
    update();
}

void ImageViewport::zoomIn() {
    if (isFitLike())
        setZoomMode(ZoomMode::OneToOne);  // 从适合模式进入 1:1 (leave fit into 1:1).
    setZoom(zoom_ * 1.25);
}

void ImageViewport::zoomOut() {
    if (isFitLike())
        return;  // 已在适合模式，无法再缩小 (already at fit).
    if (zoom_ <= 1.0)
        setZoomMode(lastFitMode_);  // 1:1 再缩小回到适合模式 (back to fit).
    else
        setZoom(zoom_ / 1.25);
}

void ImageViewport::toggleOneToOne() {
    zoomMode_ == ZoomMode::OneToOne ? setZoomMode(lastFitMode_)
                                    : setZoomMode(ZoomMode::OneToOne);
}

void ImageViewport::setZoomMode(ZoomMode mode) {
    // Remember the last fit mode (Fit/FitWidth) before entering 1:1 so a
    // double-click or Z key can return to it.
    if (mode == ZoomMode::OneToOne && isFitLike())
        lastFitMode_ = zoomMode_;
    zoomMode_ = mode;
    emit zoomModeChanged(zoomMode_);
    if (isFitLike()) {
        zoom_ = 1.0; // fit is computed in currentScale()/targetRect()
        panOffset_ = QPointF(0, 0); // 回到适合模式时居中图像 (center on fit)
        setFullResRequested(false); // leaving 1:1 releases the full-res buffer
    } else {
        zoom_ = 1.0; // OneToOne at exactly 100% (1 image pixel = 1 physical)
        // Deduped: re-entering 1:1 for the same image is a no-op, but a fresh
        // image (fullResRequested_ reset by setImage) re-requests full-res.
        setFullResRequested(true);
    }
    update();
    // A fit <-> 1:1 transition changes which source the preview worker should use;
    // report the current logical render size (deduplicated).
    emitViewportResolutionChanged();
}

double ImageViewport::currentScale() const {
    if (image_.isNull())
        return 1.0;
    const QSizeF img = QSizeF(image_.size());
    if (img.isEmpty())
        return 1.0;
    if (isFitLike()) {
        const QSizeF avail = QSizeF(size());
        if (zoomMode_ == ZoomMode::FitWidth)
            return avail.width() / img.width(); // fill width only
        return qMin(avail.width() / img.width(), avail.height() / img.height());
    }
    // 1:1 mapping: zoom_ * devicePixelRatio (1 image pixel = 1 physical
    // pixel), further scaled so a downscaled preview is framed at the native
    // resolution. nativeSize_ empty -> factor 1 (degenerate, no crash).
    double scale = zoom_ * devicePixelRatioF();
    if (!nativeSize_.isEmpty() && img.width() > 0.0)
        scale *= double(nativeSize_.width()) / img.width();
    return scale;
}

// The scale that "fit" would produce for the current widget size (used by the
// wheel handler to decide when zooming out should return to a fit mode).
double ImageViewport::fitWheelScale() const {
    if (image_.isNull() || image_.width() <= 0 || image_.height() <= 0)
        return 1.0;
    const QSizeF avail(size().width(), size().height());
    const QSizeF img(image_.width(), image_.height());
    return qMin(avail.width() / img.width(), avail.height() / img.height());
}

void ImageViewport::setFullResRequested(bool needed) {
    if (fullResRequested_ == needed)
        return;
    fullResRequested_ = needed;
    emit fullResRequested(needed);
}

QSize ImageViewport::logicalRenderSize() const {
    const qreal dpr = devicePixelRatioF();
    return QSize(qCeil(size().width() * dpr), qCeil(size().height() * dpr));
}

void ImageViewport::emitViewportResolutionChanged() {
    const QSize size = logicalRenderSize();
    if (size.isEmpty())
        return;  // not laid out yet (0x0); a later resize will emit a real size
    if (size == lastViewportResolution_)
        return;
    lastViewportResolution_ = size;
    emit viewportResolutionChanged(size);
}

QRectF ImageViewport::targetRect() const {
    if (image_.isNull())
        return QRectF();
    const QSizeF avail = QSizeF(size());
    const QSizeF img = QSizeF(image_.size());
    if (img.isEmpty())
        return QRectF();

    const double scale = currentScale();
    const QSizeF scaled = img * scale;
    const QPointF center =
        QPointF(avail.width() / 2.0, avail.height() / 2.0) + panOffset_;
    return QRectF(center - QPointF(scaled.width() / 2.0,
                                   scaled.height() / 2.0),
                  scaled);
}

void ImageViewport::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), palette().window().color());

    // Before/After + reference split rendering owns the whole widget.
    if (compareMode_ != CompareMode::Off) {
        paintCompare(painter);
        return;
    }

    if (image_.isNull()) {
        painter.setPen(Qt::gray);
        painter.drawText(rect(), Qt::AlignCenter,
                         tr("未选择图片。请先导入文件夹。"));
        return;
    }
    const QRectF dst = targetRect();
    paintCanvasBackground(painter, rect(), image_.hasAlphaChannel());
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (spotVisualize_) {
        // "Visualize spots": inverted + high-contrast copy so dust specks stand out.
        // Rebuilt lazily when the flag or image changes.
        if (!spotVizValid_)
            rebuildSpotViz();
        if (!spotViz_.isNull())
            painter.drawImage(dst, spotViz_, QRectF(spotViz_.rect()));
        else
            painter.drawImage(dst, image_, QRectF(image_.rect()));
    } else if (!overlay_.isNull() && overlay_.size() == image_.size()) {
        // Alt-key diagnostic overlay replaces the preview (LR behaviour).
        painter.drawImage(dst, overlay_, QRectF(overlay_.rect()));
    } else {
        painter.drawImage(dst, image_, QRectF(image_.rect()));
    }
    // Subtle border to separate the image from the background.
    painter.setPen(QColor(0, 0, 0, 120));
    painter.drawRect(dst.adjusted(0, 0, -1, -1));

    // B-1 content-aware crop guidance: dashed box over the suggested normalized
    // rect. Pure guidance -- never applies a crop to the pixels.
    if (autoCropActive_ && autoCropNorm_.isValid()) {
        const QPointF tl = normToWidget(autoCropNorm_.topLeft());
        const QPointF br = normToWidget(autoCropNorm_.bottomRight());
        painter.setPen(QPen(QColor(80, 200, 255, 235), 2.0, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(tl, br));
    }

    // Spot circles + cursor ring when the spot tool is armed.
    if (spotToolActive_)
        paintSpotOverlay(painter);

    // Targeted-colour sampler overlay: crosshair at the sampled pixel.
    if (samplerActive_ && hasSamplerMark_) {
        const QPointF p = samplerMark_;
        painter.setPen(QPen(QColor(255, 255, 255, 220), 1.0));
        painter.drawEllipse(p, 9, 9);
        painter.drawLine(QPointF(p.x() - 13, p.y()), QPointF(p.x() - 4, p.y()));
        painter.drawLine(QPointF(p.x() + 4, p.y()), QPointF(p.x() + 13, p.y()));
        painter.drawLine(QPointF(p.x(), p.y() - 13), QPointF(p.x(), p.y() - 4));
        painter.drawLine(QPointF(p.x(), p.y() + 4), QPointF(p.x(), p.y() + 13));
    }

    // Fixed point-colour reference mark; it does not follow the mouse.
    if (pointColorActive_ && hasPointColorMark_) {
        const QPointF p = pointColorMark_;
        painter.setPen(QPen(QColor(120, 220, 255, 220), 1.0));
        painter.drawEllipse(p, 9, 9);
        painter.drawLine(QPointF(p.x() - 13, p.y()), QPointF(p.x() - 4, p.y()));
        painter.drawLine(QPointF(p.x() + 4, p.y()), QPointF(p.x() + 13, p.y()));
        painter.drawLine(QPointF(p.x(), p.y() - 13), QPointF(p.x(), p.y() - 4));
        painter.drawLine(QPointF(p.x(), p.y() + 4), QPointF(p.x(), p.y() + 13));
    }

    // Local (masked) adjustment: red weight overlay + geometry guides.
    if (maskTool_ != lrc::MaskType::None && !maskOverlay_.isNull()) {
        // O key toggles the red weight overlay (geometry guides stay visible).
        if (maskOverlayVisible_)
            painter.drawImage(dst, maskOverlay_, QRectF(maskOverlay_.rect()));
        painter.setPen(QPen(QColor(255, 255, 255, 235), 1.0, Qt::DashLine));
        // Guides are outlines. Filling them (the previous behaviour) painted a
        // translucent white blob over the mask area, hiding both the photo and
        // the red weight overlay; only the small handle dots stay filled.
        painter.setBrush(Qt::NoBrush);
        if (maskTool_ == lrc::MaskType::LinearGradient) {
            const QPointF a = normToWidget(
                QPointF(maskDraft_.linear.startX, maskDraft_.linear.startY));
            const QPointF b = normToWidget(
                QPointF(maskDraft_.linear.endX, maskDraft_.linear.endY));
            painter.drawLine(a, b);
            painter.setBrush(QColor(255, 255, 255, 210));
            painter.drawEllipse(a, 4, 4);
            painter.drawEllipse(b, 4, 4);
            painter.setBrush(Qt::NoBrush);
        } else if (maskTool_ == lrc::MaskType::RadialGradient) {
            const QPointF c = normToWidget(
                QPointF(maskDraft_.radial.centerX, maskDraft_.radial.centerY));
            const double sx = dst.width() / image_.width();
            const double sy = dst.height() / image_.height();
            const double rx = maskDraft_.radial.radiusX * image_.width() * sx;
            const double ry = maskDraft_.radial.radiusY * image_.height() * sy;
            painter.drawEllipse(QRectF(c.x() - rx, c.y() - ry, rx * 2, ry * 2));
            painter.setBrush(QColor(255, 255, 255, 210));
            painter.drawEllipse(c, 4, 4);
            painter.setBrush(Qt::NoBrush);
        } else if (maskTool_ == lrc::MaskType::Brush) {
            // Brush cursor ring at the pointer.
            const double scale =
                qMin(dst.width() / image_.width(),
                     dst.height() / image_.height());
            const double r = maskBrushRadius_ *
                             qMin(image_.width(), image_.height()) * scale;
            painter.drawEllipse(lastMousePos_, r, r);
        }
    }

    // Click-prompt (MobileSAM) markers: green = foreground, red = background.
    // Drawn independently of the red weight overlay (the viewport cannot
    // rasterize the semantic mask without the model — Pipeline does that).
    if (maskTool_ == lrc::MaskType::AiMask &&
        maskDraft_.type == lrc::MaskType::AiMask &&
        maskDraft_.ai.target == lrc::AiMaskTarget::Prompt) {
        for (const auto& pt : maskDraft_.ai.points) {
            const QPointF p = normToWidget(QPointF(pt.nx, pt.ny));
            painter.setPen(QPen(pt.label ? QColor(0, 230, 118, 235)
                                         : QColor(255, 60, 60, 235), 2.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(p, 7, 7);
            painter.drawLine(QPointF(p.x() - 11, p.y()), QPointF(p.x() + 11, p.y()));
            painter.drawLine(QPointF(p.x(), p.y() - 11), QPointF(p.x(), p.y() + 11));
        }
    }

    // Colour-range sample mark; mirrors the point-colour mark drawing.
    if (colorRangeActive_ && hasColorRangeMark_) {
        const QPointF p = colorRangeMark_;
        painter.setPen(QPen(QColor(120, 220, 255, 220), 1.0));
        painter.drawEllipse(p, 9, 9);
        painter.drawLine(QPointF(p.x() - 13, p.y()), QPointF(p.x() - 4, p.y()));
        painter.drawLine(QPointF(p.x() + 4, p.y()), QPointF(p.x() + 13, p.y()));
        painter.drawLine(QPointF(p.x(), p.y() - 13), QPointF(p.x(), p.y() - 4));
        painter.drawLine(QPointF(p.x(), p.y() + 4), QPointF(p.x(), p.y() + 13));
    }

    // Luminance range sample mark; mirrors the point-colour mark drawing.
    if (luminanceActive_ && hasLuminanceMark_) {
        const QPointF p = luminanceMark_;
        painter.setPen(QPen(QColor(255, 210, 120, 220), 1.0));
        painter.drawEllipse(p, 9, 9);
        painter.drawLine(QPointF(p.x() - 13, p.y()), QPointF(p.x() - 4, p.y()));
        painter.drawLine(QPointF(p.x() + 4, p.y()), QPointF(p.x() + 13, p.y()));
        painter.drawLine(QPointF(p.x(), p.y() - 13), QPointF(p.x(), p.y() - 4));
        painter.drawLine(QPointF(p.x(), p.y() + 4), QPointF(p.x(), p.y() + 13));
    }
}

void ImageViewport::resizeEvent(QResizeEvent*) {
    // Report the new logical render resolution so the preview worker can re-render
    // the Fit view at exactly this size.
    emitViewportResolutionChanged();
    if (isFitLike())
        update();
}

void ImageViewport::wheelEvent(QWheelEvent* event) {
    const int dy = event->angleDelta().y();
    if (dy == 0) {
        // Horizontal scroll (or a delta with no vertical ticks): ignore — no
        // mode switch, no zoom, no full-res request.
        event->ignore();
        return;
    }
    const double factor = std::pow(1.0015, dy);
    // Anchor the image point under the cursor across the zoom step.
    const QPointF cursor = event->position();
    const QPointF imgPt = widgetToImage(cursor);
    // Capture the pre-entry scale BEFORE switching out of a fit mode: once the
    // mode is 1:1, currentScale() reflects the 1:1 mapping (>= dpr) and the
    // full-res threshold below would never fire for a fit->1:1 wheel entry.
    const double prev = currentScale();
    if (isFitLike()) {
        lastFitMode_ = zoomMode_;
        zoomMode_ = ZoomMode::OneToOne; // wheel entry: no full-res emit here
        emit zoomModeChanged(zoomMode_);
    }
    setZoom(zoom_ * factor);
    const double cur = currentScale();
    // Return to fit when zooming out near the fit scale. Snap-back fires only on the
    // zoom-OUT direction: when the fit scale is
    // >= ~1.0 the 1:1 scale already sits in the fit band, so without the
    // direction guard an upward wheel would bounce straight back to fit and
    // wheel zoom-in would be silently dead.
    if (zoomMode_ == ZoomMode::OneToOne &&
        lrc::viewportmath::wheelShouldSnapBack(dy, cur, fitWheelScale())) {
        setZoomMode(lastFitMode_);
        event->accept();
        return;
    }
    // Cross the 100% threshold (displayed scale >= devicePixelRatio): request
    // the full-res image; drop below ~95%: release it. Deduped by
    // setFullResRequested().
    const double dpr = devicePixelRatioF();
    if (prev < dpr && cur >= dpr)
        setFullResRequested(true);
    else if (prev > dpr && cur <= dpr * 0.95)
        setFullResRequested(false);
    // Recompute the pan offset so the cursor-anchored image point stays fixed.
    if (!image_.isNull() && imgPt.x() >= 0.0 && imgPt.y() >= 0.0 &&
        imgPt.x() < image_.width() && imgPt.y() < image_.height()) {
        const lrc::viewportmath::AnchorPan pan =
            lrc::viewportmath::anchorPan(
                cursor.x(), cursor.y(), imgPt.x(), imgPt.y(),
                currentScale(), image_.width(), image_.height(),
                size().width(), size().height());
        panOffset_ = QPointF(pan.x, pan.y);
    }
    event->accept();
}

void ImageViewport::mousePressEvent(QMouseEvent* event) {
    // Space/middle-button pan: no zoom-mode switch, no full-res request,
    // and it takes priority over mask/sampler handling.
    if (event->button() == Qt::MiddleButton ||
        (spacePan_ && event->button() == Qt::LeftButton)) {
        dragging_ = true;
        lastDragPos_ = event->pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    // Dragging the Before/After divider (LeftRight / TopBottom / Reference). Takes
    // priority over zoom-into-1:1 so the seam stays grabby.
    if (event->button() == Qt::LeftButton && compareMode_ != CompareMode::Off) {
        const int hit = dividerHit(event->pos());
        if (hit != 0) {
            dividerDragging_ = true;
            setCursor(hit == 1 ? Qt::SplitHCursor : Qt::SplitVCursor);
            event->accept();
            return;
        }
    }
    if (event->button() == Qt::RightButton) {
        if (spotToolActive_) {
            bool hitSource = false;
            const int idx = spotHitTest(event->pos(), &hitSource);
            if (idx >= 0) {
                deleteSpot(idx);  // right-click deletes the spot under the cursor
                event->accept();
                return;
            }
        }
        QWidget::mousePressEvent(event);
        return;
    }
    if (event->button() == Qt::LeftButton) {
        if (spotToolActive_) {
            beginSpotGesture(event->pos(),
                             (event->modifiers() & Qt::AltModifier) != 0);
            event->accept();
            return;
        }
        if (maskTool_ != lrc::MaskType::None) {
            beginMaskGesture(event->pos());
            event->accept();
            return;
        }
        if (whiteBalanceActive_) {
            sampleWhiteBalance(event->pos());
            event->accept();
            return;
        }
        if (pointColorActive_) {
            samplePointColor(event->pos());
            event->accept();
            return;
        }
        if (colorRangeActive_) {
            sampleColorRange(event->pos());
            event->accept();
            return;
        }
        if (luminanceActive_) {
            sampleLuminance(event->pos());
            event->accept();
            return;
        }
        if (samplerActive_) {
            sampleAt(event->pos());
            event->accept();
            return;
        }
        if (isFitLike()) {
            const QPointF clickImg = widgetToImage(event->pos());  // fit 映射下的图像坐标 (image-space click).
            // Only a click ON the image drags into 1:1. A letterbox click
            // (image-space coords outside [0,width) x [0,height)) is ignored so
            // a blank area can't switch modes or push the image off-screen.
            if (!lrc::viewportmath::imagePointInBounds(
                    clickImg.x(), clickImg.y(),
                    image_.width(), image_.height())) {
                event->ignore();
                return;
            }
            lastFitMode_ = zoomMode_;
            zoomMode_ = ZoomMode::OneToOne;
            emit zoomModeChanged(zoomMode_);
            setFullResRequested(true); // click-drag into 1:1 requests full-res
            // Centre the clicked pixel in the viewport.
            const double scale = currentScale();
            panOffset_ = QPointF(scale * (image_.width() * 0.5 - clickImg.x()),
                                 scale * (image_.height() * 0.5 - clickImg.y()));
        }
        dragging_ = true;
        lastDragPos_ = event->pos();
        event->accept();
    }
}

void ImageViewport::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton)
        return;
    if (image_.isNull())
        return;
    // Mask / sampler / spot tools own the click: keep their priority.
    if (spotToolActive_ || maskTool_ != lrc::MaskType::None ||
        whiteBalanceActive_ || pointColorActive_ || colorRangeActive_ ||
        luminanceActive_ || samplerActive_)
        return;
    if (isFitLike())
        setZoomMode(ZoomMode::OneToOne);
    else
        setZoomMode(lastFitMode_);
    event->accept();
}

void ImageViewport::mouseMoveEvent(QMouseEvent* event) {
    lastMousePos_ = event->pos();
    // Dragging the Before/After divider.
    if (dividerDragging_) {
        updateDivider(event->pos());
        event->accept();
        return;
    }
    // Dragging a spot's target or source circle.
    if (draggingSpot_ >= 0) {
        moveSpotGesture(event->pos());
        event->accept();
        return;
    }
    // Hover cursor over the divider seam; moving off it restores the active tool's
    // cursor via applyToolCursor (which no-ops during a drag).
    if (compareMode_ != CompareMode::Off) {
        const int hit = dividerHit(event->pos());
        if (hit != 0) {
            setCursor(hit == 1 ? Qt::SplitHCursor : Qt::SplitVCursor);
        } else {
            applyToolCursor();
        }
    }
    if (maskDragging_ && maskTool_ != lrc::MaskType::None) {
        moveMaskGesture(event->pos());
        event->accept();
        return;
    }
    if (samplerActive_ && (event->buttons() & Qt::LeftButton) &&
        sampledRange_ >= 0) {
        // Horizontal drag adjusts saturation, vertical drag adjusts luminance.
        const QPointF delta = event->pos() - samplerStart_;
        const double sat = lrc::clamp(
            samplerBaseSat_ + delta.x() * 0.25, -100.0, 100.0);
        const double lum = lrc::clamp(
            samplerBaseLum_ - delta.y() * 0.25, -100.0, 100.0);
        samplerMark_ = event->pos();
        emit colorAdjusted(sampledRange_, sat, lum);
        update();
        event->accept();
        return;
    }
    if (dragging_) {
        const QPointF delta = event->pos() - lastDragPos_;
        panOffset_ += delta;
        lastDragPos_ = event->pos();
        update();
    }
}

void ImageViewport::mouseReleaseEvent(QMouseEvent* event) {
    // End a space/middle-button pan. The cursor stays a closed hand while
    // Space is held; otherwise it returns to the active tool's cursor.
    if (event->button() == Qt::MiddleButton ||
        (event->button() == Qt::LeftButton && spacePan_)) {
        dragging_ = false;
        if (!spacePan_)
            applyToolCursor();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        if (dividerDragging_) {
            dividerDragging_ = false;
            applyToolCursor();
            event->accept();
            return;
        }
        if (draggingSpot_ >= 0) {
            endSpotGesture();
            event->accept();
            return;
        }
        if (maskDragging_) {
            endMaskGesture();
            event->accept();
            return;
        }
        if (samplerActive_ && sampledRange_ >= 0) {
            emit colorAdjustedFinished();
            event->accept();
            return;
        }
        dragging_ = false;
        if (!spacePan_)
            applyToolCursor();
        event->accept();
    }
}

void ImageViewport::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space) {
        // Space: hold to pan (cursor becomes a closed hand).
        spacePan_ = true;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Left) {
        panOffset_ += QPointF(-40, 0);
        update();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Right) {
        panOffset_ += QPointF(40, 0);
        update();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Up) {
        panOffset_ += QPointF(0, -40);
        update();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Down) {
        panOffset_ += QPointF(0, 40);
        update();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_O) {
        // O: toggle the red mask weight overlay visibility.
        setMaskOverlayVisible(!maskOverlayVisible_);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_W) {
        // W: toggle the white-balance eyedropper.
        setWhiteBalanceSamplerActive(!whiteBalanceActive_);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Delete) {
        // Delete: remove the selected spot.
        if (spotToolActive_ && selectedSpot_ >= 0 &&
            selectedSpot_ < static_cast<int>(spotRemovals_.size())) {
            deleteSpot(selectedSpot_);
            event->accept();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

void ImageViewport::keyReleaseEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space) {
        spacePan_ = false;
        if (!dragging_ && !maskDragging_)
            applyToolCursor();
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

// --- Spot removal / healing brush -------------------------------------------

void ImageViewport::setSpotToolActive(bool active) {
    if (active) {
        // Mutual exclusion: disarm the eyedroppers and the mask tool so only
        // the spot tool owns the pointer (mirrors the sampler setters).
        samplerActive_ = false;
        whiteBalanceActive_ = false;
        pointColorActive_ = false;
        colorRangeActive_ = false;
        luminanceActive_ = false;
        hasSamplerMark_ = false;
        sampledRange_ = -1;
        hasPointColorMark_ = false;
        hasColorRangeMark_ = false;
        hasLuminanceMark_ = false;
        if (maskTool_ != lrc::MaskType::None)
            setMaskTool(lrc::MaskType::None);
        setCursor(Qt::CrossCursor);
    } else {
        // A tool switch can happen mid-drag (MainWindow disarms the tool while
        // a circle is being moved); end the gesture so the list is committed.
        draggingSpot_ = -1;
        draggingSpotSource_ = false;
        setCursor(Qt::ArrowCursor);
    }
    if (spotToolActive_ == active)
        return;
    spotToolActive_ = active;
    emit spotToolActiveChanged(spotToolActive_);
    update();
}

void ImageViewport::setSpotType(lrc::SpotType type) {
    spotType_ = type;
    update();
}

void ImageViewport::setSpotRadius(double normalizedRadius) {
    spotRadius_ = lrc::clamp(normalizedRadius, 0.001, 0.5);
    update();
}

void ImageViewport::setSpotRemovals(const std::vector<lrc::SpotRemoval>& spots) {
    spotRemovals_ = spots;
    if (selectedSpot_ >= static_cast<int>(spotRemovals_.size()))
        selectedSpot_ = -1;
    if (draggingSpot_ >= static_cast<int>(spotRemovals_.size()))
        draggingSpot_ = -1;
    update();
}

void ImageViewport::setSpotVisualizeVisible(bool visible) {
    if (spotVisualize_ == visible) {
        update();
        return;
    }
    spotVisualize_ = visible;
    if (visible) {
        // Rebuild lazily on the next paint against the current image.
        spotViz_ = QImage();
        spotVizValid_ = false;
    }
    update();
}

QPointF ImageViewport::widgetToNorm(const QPointF& pos) const {
    if (image_.isNull() || image_.width() <= 0 || image_.height() <= 0)
        return QPointF(0, 0);
    const QPointF img = widgetToImage(pos);
    return QPointF(lrc::clamp(img.x() / image_.width(), 0.0, 1.0),
                   lrc::clamp(img.y() / image_.height(), 0.0, 1.0));
}

double ImageViewport::spotWidgetRadius(const lrc::SpotRemoval& spot) const {
    if (image_.isNull())
        return 0.0;
    const double longest = static_cast<double>(
        std::max(image_.width(), image_.height()));
    return spot.radius * longest * currentScale();
}

int ImageViewport::spotHitTest(const QPointF& widgetPos, bool* source) const {
    if (image_.isNull())
        return -1;
    // Iterate topmost first so overlapping spots grab the one drawn last.
    for (int i = static_cast<int>(spotRemovals_.size()) - 1; i >= 0; --i) {
        const lrc::SpotRemoval& s = spotRemovals_[static_cast<size_t>(i)];
        if (!s.visible)
            continue;
        const QPointF t = normToWidget(QPointF(s.targetX, s.targetY));
        const QPointF src = normToWidget(QPointF(s.sourceX, s.sourceY));
        const double r = spotWidgetRadius(s) + kSpotHandlePx;
        const double r2 = r * r;
        if (pointDistSq(widgetPos, t) <= r2) {
            if (source)
                *source = false;
            return i;
        }
        if (pointDistSq(widgetPos, src) <= r2) {
            if (source)
                *source = true;
            return i;
        }
    }
    return -1;
}

void ImageViewport::beginSpotGesture(const QPointF& widgetPos, bool alt) {
    if (image_.isNull())
        return;
    // Reject letterbox clicks (outside the image) so spots cannot be created
    // or grabbed in the surrounding background.
    const QPointF imgPt = widgetToImage(widgetPos);
    if (!lrc::viewportmath::imagePointInBounds(
            imgPt.x(), imgPt.y(), image_.width(), image_.height()))
        return;
    const QPointF n = widgetToNorm(widgetPos);
    bool hitSource = false;
    const int idx = spotHitTest(widgetPos, &hitSource);

    if (alt) {
        // Alt+click on the source circle: select it and start dragging it.
        if (idx >= 0 && hitSource) {
            selectedSpot_ = idx;
            draggingSpot_ = idx;
            draggingSpotSource_ = true;
            spotDragLastNorm_ = n;
            return;
        }
        // Alt+click on a spot's target circle: place its source at the click
        // and start dragging the source.
        if (idx >= 0) {
            lrc::SpotRemoval& s = spotRemovals_[static_cast<size_t>(idx)];
            s.sourceX = static_cast<float>(n.x());
            s.sourceY = static_cast<float>(n.y());
            selectedSpot_ = idx;
            draggingSpot_ = idx;
            draggingSpotSource_ = true;
            spotDragLastNorm_ = n;
            emitSpotRemovals();
            update();
            return;
        }
        return;  // Alt+click on empty space: nothing to attach a source to
    }

    // Plain click on the source circle selects the spot without moving it
    // (dragging the source requires Alt).
    if (idx >= 0 && hitSource) {
        selectedSpot_ = idx;
        update();
        return;
    }
    // Plain click on the target circle: select and start dragging the target
    // (the source moves along, keeping its offset).
    if (idx >= 0) {
        selectedSpot_ = idx;
        draggingSpot_ = idx;
        draggingSpotSource_ = false;
        spotDragLastNorm_ = n;
        return;
    }
    // Click on empty image area: create a new spot with the target at the
    // click and the source offset to the right of it.
    lrc::SpotRemoval spot;
    spot.type = spotType_;
    spot.targetX = static_cast<float>(n.x());
    spot.targetY = static_cast<float>(n.y());
    const double off = kSpotDefaultSourceOffset * spotRadius_;
    spot.sourceX = static_cast<float>(lrc::clamp(n.x() + off, 0.0, 1.0));
    spot.sourceY = spot.targetY;
    spot.radius = static_cast<float>(spotRadius_);
    spot.visible = true;
    spotRemovals_.push_back(spot);
    selectedSpot_ = static_cast<int>(spotRemovals_.size()) - 1;
    emitSpotRemovals();
    update();
}

void ImageViewport::moveSpotGesture(const QPointF& widgetPos) {
    if (draggingSpot_ < 0 ||
        static_cast<size_t>(draggingSpot_) >= spotRemovals_.size())
        return;
    const QPointF n = widgetToNorm(widgetPos);
    lrc::SpotRemoval& s = spotRemovals_[static_cast<size_t>(draggingSpot_)];
    if (draggingSpotSource_) {
        s.sourceX = static_cast<float>(lrc::clamp(n.x(), 0.0, 1.0));
        s.sourceY = static_cast<float>(lrc::clamp(n.y(), 0.0, 1.0));
    } else {
        // Move the target; the source translates by the same (clamped) delta so
        // the source-target offset is preserved.
        const QPointF d = n - spotDragLastNorm_;
        const double newTx = lrc::clamp(s.targetX + d.x(), 0.0, 1.0);
        const double newTy = lrc::clamp(s.targetY + d.y(), 0.0, 1.0);
        const double ddx = newTx - s.targetX;
        const double ddy = newTy - s.targetY;
        s.targetX = static_cast<float>(newTx);
        s.targetY = static_cast<float>(newTy);
        s.sourceX = static_cast<float>(lrc::clamp(s.sourceX + ddx, 0.0, 1.0));
        s.sourceY = static_cast<float>(lrc::clamp(s.sourceY + ddy, 0.0, 1.0));
    }
    spotDragLastNorm_ = n;
    emitSpotRemovals();
    update();
}

void ImageViewport::endSpotGesture() {
    if (draggingSpot_ < 0)
        return;
    draggingSpot_ = -1;
    draggingSpotSource_ = false;
    emitSpotRemovals();
    update();
}

void ImageViewport::deleteSpot(int index) {
    if (index < 0 || static_cast<size_t>(index) >= spotRemovals_.size())
        return;
    spotRemovals_.erase(spotRemovals_.begin() + index);
    if (selectedSpot_ == index) {
        selectedSpot_ = -1;
    } else if (selectedSpot_ > index) {
        --selectedSpot_;
    }
    if (draggingSpot_ == index) {
        draggingSpot_ = -1;
        draggingSpotSource_ = false;
    } else if (draggingSpot_ > index) {
        --draggingSpot_;
    }
    emitSpotRemovals();
    update();
}

void ImageViewport::emitSpotRemovals() {
    emit spotRemovalsEdited(spotRemovals_);
}

void ImageViewport::paintSpotOverlay(QPainter& painter) {
    if (image_.isNull())
        return;
    // Draw the created circles, topmost last so the selected spot is grabbable.
    for (int i = 0; i < static_cast<int>(spotRemovals_.size()); ++i) {
        const lrc::SpotRemoval& s = spotRemovals_[static_cast<size_t>(i)];
        if (!s.visible)
            continue;
        const QPointF t = normToWidget(QPointF(s.targetX, s.targetY));
        const QPointF src = normToWidget(QPointF(s.sourceX, s.sourceY));
        const double r = spotWidgetRadius(s);
        const bool sel = (i == selectedSpot_);

        // Connector line between the source and target circles.
        painter.setPen(QPen(QColor(255, 255, 255, 150), 1.0, Qt::DashLine));
        painter.drawLine(t, src);

        // Target circle: solid ring (the editable side).
        painter.setPen(QPen(QColor(255, 255, 255, 235), sel ? 2.0 : 1.5,
                            Qt::SolidLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(t, r, r);

        // Source circle: dashed ring (moved with Alt).
        painter.setPen(QPen(QColor(255, 180, 80, 230), sel ? 2.0 : 1.5,
                            Qt::DashLine));
        painter.drawEllipse(src, r, r);

        if (sel) {
            // Selection highlight ring around the target.
            painter.setPen(QPen(QColor(120, 220, 255, 220), 1.0, Qt::SolidLine));
            painter.drawEllipse(t, r + 4.0, r + 4.0);
        }
    }

    // Cursor ring showing the current radius while hovering (skipped during a
    // drag so the ring does not chase the moving circle).
    if (draggingSpot_ >= 0)
        return;
    const double cursorR = spotRadius_ *
                           std::max(image_.width(), image_.height()) *
                           currentScale();
    painter.setPen(QPen(QColor(255, 255, 255, 200), 1.0, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(lastMousePos_, cursorR, cursorR);
    // Small crosshair at the pointer.
    painter.setPen(QPen(QColor(255, 255, 255, 220), 1.0, Qt::SolidLine));
    painter.drawLine(QPointF(lastMousePos_.x() - 5, lastMousePos_.y()),
                     QPointF(lastMousePos_.x() + 5, lastMousePos_.y()));
    painter.drawLine(QPointF(lastMousePos_.x(), lastMousePos_.y() - 5),
                     QPointF(lastMousePos_.x(), lastMousePos_.y() + 5));
}

void ImageViewport::rebuildSpotViz() {
    spotViz_ = QImage();
    spotVizValid_ = false;
    if (image_.isNull())
        return;
    // Cap the visualization's longer edge so toggling the mode stays snappy
    // even at 1:1; Qt scales the copy up to the target rect on draw. Fit-mode
    // previews are already <= 2048px, so they keep full detail.
    const int maxDim = std::max(image_.width(), image_.height());
    constexpr int kVizMaxDim = 2048;
    int ow = image_.width();
    int oh = image_.height();
    if (maxDim > kVizMaxDim) {
        const double r = static_cast<double>(kVizMaxDim) / maxDim;
        ow = std::max(1, static_cast<int>(image_.width() * r));
        oh = std::max(1, static_cast<int>(image_.height() * r));
    }
    // Opaque copy of the source for fast scan-line access.
    const QImage src = image_.convertToFormat(QImage::Format_RGB32);
    const int sw = src.width();
    const int sh = src.height();
    QImage viz(ow, oh, QImage::Format_RGB32);
    for (int y = 0; y < oh; ++y) {
        const QRgb* sline = reinterpret_cast<const QRgb*>(
            src.constScanLine((y * sh) / oh));
        QRgb* vline = reinterpret_cast<QRgb*>(viz.scanLine(y));
        for (int x = 0; x < ow; ++x) {
            const QRgb p = sline[(x * sw) / ow];
            vline[x] = qRgb(invertHighContrast(qRed(p)),
                            invertHighContrast(qGreen(p)),
                            invertHighContrast(qBlue(p)));
        }
    }
    spotViz_ = viz;
    spotVizValid_ = true;
}

void ImageViewport::applyToolCursor() {
    if (spacePan_ || dragging_ || dividerDragging_ || draggingSpot_ >= 0 ||
        maskDragging_)
        return;  // a drag is in progress; keep its cursor
    if (spotToolActive_ || samplerActive_ || whiteBalanceActive_ ||
        pointColorActive_ || colorRangeActive_ || luminanceActive_ ||
        maskTool_ != lrc::MaskType::None) {
        setCursor(Qt::CrossCursor);
    } else {
        setCursor(Qt::ArrowCursor);
    }
}

// --- Before/After + reference view -------------------------------------------

void ImageViewport::setBeforeAfterImages(const QImage& before,
                                         const QImage& after) {
    beforeImage_ = before;
    afterImage_ = after;
    update();
}

void ImageViewport::setBeforeAfterMode(int mode) {
    auto m = static_cast<CompareMode>(mode);
    if (m != CompareMode::Off && m != CompareMode::LeftRight &&
        m != CompareMode::TopBottom && m != CompareMode::SingleToggle)
        m = CompareMode::Off;
    if (compareMode_ == m) {
        update();
        return;
    }
    compareMode_ = m;
    dividerDragging_ = false;
    if (compareMode_ == CompareMode::Off)
        applyToolCursor();
    emit compareModeChanged(compareMode_);
    update();
}

void ImageViewport::toggleBeforeAfter() {
    if (compareMode_ == CompareMode::SingleToggle) {
        singleShowsBefore_ = !singleShowsBefore_;
    } else {
        // "\" enters single-toggle comparison showing the Before image.
        compareMode_ = CompareMode::SingleToggle;
        singleShowsBefore_ = true;
        emit compareModeChanged(compareMode_);
    }
    update();
}

void ImageViewport::setReferenceImage(const QImage& image) {
    referenceImage_ = image;
    update();
}

void ImageViewport::setReferenceMode(bool on) {
    const CompareMode target = on ? CompareMode::Reference : CompareMode::Off;
    if (compareMode_ == target) {
        update();
        return;
    }
    compareMode_ = target;
    dividerDragging_ = false;
    if (!on)
        applyToolCursor();
    emit compareModeChanged(compareMode_);
    update();
}

const QImage& ImageViewport::afterImage() const {
    return image_.isNull() ? afterImage_ : image_;
}

QRectF ImageViewport::compareTargetRect() const {
    if (!image_.isNull())
        return targetRect();
    if (!afterImage_.isNull()) {
        const QSizeF avail = QSizeF(size());
        const QSizeF img = QSizeF(afterImage_.size());
        if (img.isEmpty())
            return QRectF();
        const double scale = qMin(avail.width() / img.width(),
                                  avail.height() / img.height());
        const QSizeF scaled = img * scale;
        return QRectF(QPointF((avail.width() - scaled.width()) / 2.0,
                              (avail.height() - scaled.height()) / 2.0),
                      scaled);
    }
    return QRectF();
}

QRectF ImageViewport::centerRect(const QSizeF& scaled,
                                 const QRectF& pane) const {
    return QRectF(QPointF(pane.center().x() - scaled.width() / 2.0,
                          pane.center().y() - scaled.height() / 2.0),
                  scaled);
}

int ImageViewport::dividerHit(const QPointF& pos) const {
    if (compareMode_ == CompareMode::LeftRight ||
        compareMode_ == CompareMode::Reference) {
        const qreal x = divider_ * width();
        return std::fabs(pos.x() - x) <= kDividerGrabPx ? 1 : 0;
    }
    if (compareMode_ == CompareMode::TopBottom) {
        const qreal y = divider_ * height();
        return std::fabs(pos.y() - y) <= kDividerGrabPx ? 2 : 0;
    }
    return 0;  // Off and SingleToggle have no draggable seam
}

void ImageViewport::updateDivider(const QPointF& pos) {
    if (compareMode_ == CompareMode::LeftRight ||
        compareMode_ == CompareMode::Reference) {
        if (width() > 0)
            divider_ = lrc::clamp(pos.x() / width(), 0.05, 0.95);
    } else if (compareMode_ == CompareMode::TopBottom) {
        if (height() > 0)
            divider_ = lrc::clamp(pos.y() / height(), 0.05, 0.95);
    }
    update();
}

void ImageViewport::drawDivider(QPainter& painter) {
    painter.save();
    painter.setPen(QPen(QColor(255, 255, 255, 220), 1.0));
    if (compareMode_ == CompareMode::LeftRight ||
        compareMode_ == CompareMode::Reference) {
        const qreal x = divider_ * width();
        painter.drawLine(QPointF(x, 0), QPointF(x, height()));
    } else if (compareMode_ == CompareMode::TopBottom) {
        const qreal y = divider_ * height();
        painter.drawLine(QPointF(0, y), QPointF(width(), y));
    }
    // SingleToggle has no seam.
    painter.restore();
}

void ImageViewport::paintCompare(QPainter& painter) {
    // Both halves show the same photo, so the surround follows the same alpha
    // rule as the single-image path.
    paintCanvasBackground(painter, rect(),
                          beforeImage_.hasAlphaChannel() ||
                              afterImage().hasAlphaChannel());
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    switch (compareMode_) {
    case CompareMode::Off:
        break;
    case CompareMode::SingleToggle: {
        // Full-widget flip between before and after ("\").
        const QImage& shown = singleShowsBefore_ ? beforeImage_ : afterImage();
        if (!shown.isNull()) {
            const QRectF dst = compareTargetRect();
            if (!dst.isEmpty())
                painter.drawImage(dst, shown, QRectF(shown.rect()));
        }
        break;
    }
    case CompareMode::LeftRight: {
        // Both halves share the current zoom/pan mapping (targetRect); the
        // clip split at the divider shows before on the left, after on the
        // right. Before/after are same-size previews, so no distortion.
        const QRectF dst = compareTargetRect();
        const qreal dx = divider_ * width();
        painter.save();
        painter.setClipRect(QRectF(0, 0, dx, height()));
        if (!beforeImage_.isNull() && !dst.isEmpty())
            painter.drawImage(dst, beforeImage_, QRectF(beforeImage_.rect()));
        painter.restore();
        painter.save();
        painter.setClipRect(QRectF(dx, 0, width() - dx, height()));
        const QImage& after = afterImage();
        if (!after.isNull() && !dst.isEmpty())
            painter.drawImage(dst, after, QRectF(after.rect()));
        painter.restore();
        drawDivider(painter);
        break;
    }
    case CompareMode::TopBottom: {
        const QRectF dst = compareTargetRect();
        const qreal dy = divider_ * height();
        painter.save();
        painter.setClipRect(QRectF(0, 0, width(), dy));
        if (!beforeImage_.isNull() && !dst.isEmpty())
            painter.drawImage(dst, beforeImage_, QRectF(beforeImage_.rect()));
        painter.restore();
        painter.save();
        painter.setClipRect(QRectF(0, dy, width(), height() - dy));
        const QImage& after = afterImage();
        if (!after.isNull() && !dst.isEmpty())
            painter.drawImage(dst, after, QRectF(after.rect()));
        painter.restore();
        drawDivider(painter);
        break;
    }
    case CompareMode::Reference: {
        // Read-only reference on the left, current photo on the right. Locked
        // sync: both panes share the smaller of their two contain-fit scales so
        // features are compared at the same magnification without distortion.
        const qreal dx = divider_ * width();
        const QRectF leftPane(0, 0, dx, height());
        const QRectF rightPane(dx, 0, width() - dx, height());
        const QImage& ref = referenceImage_;
        const QImage& after = afterImage();
        double scale = 1.0;
        if (!ref.isNull() && !after.isNull()) {
            const double refFit = qMin(leftPane.width() / ref.width(),
                                       leftPane.height() / ref.height());
            const double curFit = qMin(rightPane.width() / after.width(),
                                       rightPane.height() / after.height());
            scale = qMin(refFit, curFit);
        }
        if (!ref.isNull()) {
            const QRectF r = centerRect(QSizeF(ref.size()) * scale, leftPane);
            painter.drawImage(r, ref, QRectF(ref.rect()));
        }
        if (!after.isNull()) {
            const QRectF r = centerRect(QSizeF(after.size()) * scale, rightPane);
            painter.drawImage(r, after, QRectF(after.rect()));
        }
        drawDivider(painter);
        break;
    }
    }
}
