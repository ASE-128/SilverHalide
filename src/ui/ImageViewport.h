#pragma once

/**
 * @brief Central preview area.
 *
 * Renders the current preview QImage with a fit / 1:1 / zoomed mapping,
 * letterboxing with a neutral checkerboard. Mouse wheel zooms around the
 * cursor; dragging pans when zoomed in. Also hosts the mask/spot editing tools,
 * the Before/After + reference compare split, and the colour-sampler tools.
 * Lives on the UI thread; it communicates with the engine and catalog only
 * through signals.
 */

#include <cstddef>

#include <QColor>
#include <QImage>
#include <QKeyEvent>
#include <QPoint>
#include <QPointF>
#include <QSize>
#include <QWidget>

#include "engine/Adjustments.h"

class QPainter;

class ImageViewport : public QWidget {
    Q_OBJECT
public:
    enum class ZoomMode { Fit, FitWidth, OneToOne };

    // Before/After + reference view split modes. Off renders the
    // single current image; LeftRight / TopBottom split the widget around a
    // draggable divider; SingleToggle shows before or after in the full widget
    // (\ flips it); Reference shows a read-only reference photo on the left and
    // the current photo on the right.
    enum class CompareMode : uint8_t {
        Off = 0,
        LeftRight = 1,
        TopBottom = 2,
        SingleToggle = 3,
        Reference = 4,
    };

    explicit ImageViewport(QWidget* parent = nullptr);

    void setImage(const QImage& image);
    const QImage& image() const { return image_; }

    // Records the native (full-resolution) pixel size of the current image so
    // a downscaled preview can be framed at a true 1:1 (1 native pixel = 1
    // physical pixel) while the full-res decode is still in flight. Clear with
    // an empty QSize (e.g. when switching to another image).
    void setNativeImageSize(const QSize& size);
    QSize nativeSize() const { return nativeSize_; }

    // Swaps in a new image (usually the full-resolution buffer) while keeping
    // the current zoom mode/level and the image point under the viewport centre
    // fixed. Regular setImage() still resets the view.
    void setImageKeepView(const QImage& image);

    // LR-style Alt-key diagnostic overlay: while set, it is drawn in place of
    // the preview (clipping mask, detail mask, edge mask). Clear with an empty
    // image.
    void setDiagnosticOverlay(const QImage& overlay);
    const QImage& diagnosticOverlay() const { return overlay_; }

    // B-1 content-aware crop guidance: draws a dashed rectangle over the preview
    // at the normalized [0,1] rect suggested by the last auto-develop run. It is
    // pure guidance -- it never writes to the adjustment stack or alters pixels
    // (the pipeline has no crop-rendering stage yet). Clear with clearAutoCropSuggestion.
    void setAutoCropSuggestion(const QRectF& normalizedRect);
    void clearAutoCropSuggestion();

    void setZoomMode(ZoomMode mode);
    ZoomMode zoomMode() const { return zoomMode_; }
    void zoomIn();
    void zoomOut();
    void resetView();
    // Toggles between 1:1 and the last fit mode (Fit/FitWidth). Z key.
    void toggleOneToOne();
    double zoomFactor() const { return zoom_; }

    // Targeted (point) colour sampler: while active, press + drag on the image
    // picks the colour range under the cursor and adjusts its saturation
    // (horizontal drag) and luminance (vertical drag), LR-style.
    void setSamplerActive(bool active);
    bool samplerActive() const { return samplerActive_; }
    // Base saturation/luminance the drag deltas are added to (called by
    // MainWindow when it receives colorSampled).
    void setSamplerBase(int rangeIndex, double saturation, double luminance);

    // Point-colour reference sampler: when active, a single click on the image
    // samples the pixel colour and emits it via pointColorSampled.
    void setPointColorSamplerActive(bool active);

    // White-balance eyedropper: when active, a single click samples the pixel
    // colour (sRGB display colour) and emits it via whiteBalanceSampled
    // (MainWindow converts it to linear and runs white-balance estimation).
    void setWhiteBalanceSamplerActive(bool active);
    bool whiteBalanceSamplerActive() const { return whiteBalanceActive_; }

    // Colour-range sampler: when active, a single click samples the pixel's
    // display colour (sRGB) and emits it via colorRangeSampled (MainWindow
    // derives the colour-range mask weights from it).
    void setColorRangeSamplerActive(bool active);
    // Sets the current mask's reference colour (sRGB display colour) so the
    // overlay can compute colour-range weights.
    void setMaskColorReference(const QColor& srgb);
    bool colorRangeSamplerActive() const { return colorRangeActive_; }

    // Luminance-range sampler: when active, a single click computes the
    // pixel's relative luma L = 0.2126R + 0.7152G + 0.0722B (linear space) and
    // emits it via luminanceSampled (0-100 percent; MainWindow writes it into
    // the current mask's luminanceRef).
    void setLuminanceSamplerActive(bool active);
    bool luminanceSamplerActive() const { return luminanceActive_; }

    // --- Local (masked) adjustment editing ---------------------------------
    // Selects the mask tool. `tool` is lrc::MaskType; None (=0) disables and
    // hides the red weight overlay.
    void setMaskTool(lrc::MaskType tool);
    // Sets the mask whose weight overlay is drawn and which the active tool
    // edits (geometry + local adjustments live in this draft).
    void setMaskDraft(const lrc::Mask& mask);
    // Brush dab radius, normalized against the shorter image dimension.
    void setMaskBrushSize(double normalizedRadius);
    // Brush dab softness in [0,1]: 0 is a hard dab, 1 is fully soft. Default 0.5.
    void setMaskBrushSoftness(double softness);
    double maskBrushSoftness() const { return maskBrushSoftness_; }

    // Label for the next click-prompt (MobileSAM) point added by the mask tool:
    // 1 = foreground (include), 0 = background (exclude). Defaults to foreground.
    void setMaskPromptLabel(int label) { maskPromptLabel_ = label ? 1 : 0; }
    int maskPromptLabel() const { return maskPromptLabel_; }

    // Controls whether the red weight overlay is drawn (shortcut O toggles).
    // Defaults to true.
    void setMaskOverlayVisible(bool visible);
    bool maskOverlayVisible() const { return maskOverlayVisible_; }

    // --- Spot removal / healing brush --------------------------------------
    // Arms the spot tool (mutually exclusive with the mask tool and the
    // eyedroppers): the cursor becomes a crosshair, a click creates a target
    // circle, Alt+click places / drags the source circle, Delete or right-click
    // deletes the selected spot. Overlays are drawn only while active.
    void setSpotToolActive(bool active);
    bool spotToolActive() const { return spotToolActive_; }
    // Mode used for newly created spots (lrc::SpotType::Heal / Clone).
    void setSpotType(lrc::SpotType type);
    lrc::SpotType spotType() const { return spotType_; }
    // Radius used for newly created spots, normalized against the image's
    // longer edge (clamped to [0.001, 0.5]).
    void setSpotRadius(double normalizedRadius);
    double spotRadius() const { return spotRadius_; }
    // Replaces the current spot-removal list (external update, e.g. a different
    // photo or an undo/redo). Selection is kept if it still fits.
    void setSpotRemovals(const std::vector<lrc::SpotRemoval>& spots);
    const std::vector<lrc::SpotRemoval>& spotRemovals() const {
        return spotRemovals_;
    }
    // "Visualize spots" temporary display mode: draws an inverted, high-
    // contrast copy of the preview so dust specks stand out. Does not change
    // any parameters.
    void setSpotVisualizeVisible(bool visible);
    bool spotVisualizeVisible() const { return spotVisualize_; }

    // --- Before/After + reference view --------------------------------------
    // Supplies the two renderings for Before/After comparison. `before` is the
    // zero-adjustment render, `after` the current render (usually the same
    // QImage the viewport already holds via setImage). Neither is modified.
    void setBeforeAfterImages(const QImage& before, const QImage& after);
    // Switches the split mode: 0 = Off, 1 = LeftRight, 2 = TopBottom,
    // 3 = SingleToggle. Reference mode is entered via setReferenceMode.
    void setBeforeAfterMode(int mode);
    CompareMode compareMode() const { return compareMode_; }
    // "\" behaviour: in SingleToggle mode it flips before/after; otherwise it
    // enters SingleToggle showing the Before image.
    void toggleBeforeAfter();
    // Reference view: `image` is the read-only reference photo shown in the
    // left pane (right pane stays the current photo). It cannot be edited.
    void setReferenceImage(const QImage& image);
    void setReferenceMode(bool on);

signals:
    // Currently unconnected project-wide (dead signal): kept for the toolbar's
    // zoom-level display to connect to in the future, and harmless because a
    // Qt signal with no receivers is a no-op.
    void zoomChanged(double factor);
    // Emitted whenever the zoom mode changes (Fit / FitWidth / OneToOne) so the
    // toolbar can stay in sync. Fired by setZoomMode and by the wheel / click-
    // drag paths that switch directly into 1:1.
    void zoomModeChanged(ZoomMode mode);
    // Emitted when the viewport needs (true) or no longer needs (false) the
    // full-resolution image. Deduplicated — only emitted on state changes.
    // Fired by explicit zoom intent (Ctrl+1 / double-click / wheel past 100%)
    // and by a click-drag into 1:1; not by an image swap.
    void fullResRequested(bool needed);
    // Emitted once on press: the colour range nearest the sampled pixel.
    void colorSampled(int colorRangeIndex, const QColor& color);
    // Emitted while dragging: absolute saturation/luminance offsets (-100..100).
    void colorAdjusted(int colorRangeIndex, double saturation, double luminance);
    // Emitted on release (persist the change).
    void colorAdjustedFinished();
    // Point-colour reference sampler (sampled colour emitted on click).
    void pointColorSampled(const QColor& color);
    // White-balance sample: the pixel's display colour (sRGB QColor).
    void whiteBalanceSampled(const QColor& color);
    // Colour-range sample: the pixel's display colour (sRGB QColor).
    void colorRangeSampled(const QColor& color);
    // Luminance-range sample: the pixel's relative luma (0-100 percent, linear
    // Rec.709 luma).
    void luminanceSampled(double luma);
    // The user edited the geometry of the current mask draft (live while
    // dragging/painting).
    void maskGeometryChanged(const lrc::Mask& mask);
    // The user released the drag / ended the brush stroke.
    void maskEditFinished();
    // The logical render resolution the viewport currently needs for its image
    // (widget size x devicePixelRatio, in device pixels). Emitted on resize and
    // on zoom-mode changes; PreviewWorker subscribes (via MainWindow, which the
    // coordinator wires) and re-renders the Fit/<=1:1 preview at this resolution
    // instead of the fixed 1920px pipeline.
    void viewportResolutionChanged(const QSize& size);

    // The user created / moved / deleted a spot. Emitted live while a circle is
    // dragged and once more on release; carries the full updated list. MainWindow
    // applies it to the Adjustments and re-renders.
    void spotRemovalsEdited(const std::vector<lrc::SpotRemoval>& spots);
    // The spot tool armed/disarmed state (cursor + overlay toggles).
    void spotToolActiveChanged(bool active);
    // The Before/After / reference split mode changed (toolbar sync).
    void compareModeChanged(CompareMode mode);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    QRectF targetRect() const;
    // True when the view auto-fits the widget (Fit or FitWidth).
    bool isFitLike() const {
        return zoomMode_ == ZoomMode::Fit || zoomMode_ == ZoomMode::FitWidth;
    }
    // Display scale currently applied to the image (pixels-per-image-pixel).
    // In OneToOne mode this accounts for devicePixelRatio and the native-size
    // ratio, so a downscaled preview is framed at a true 1:1.
    double currentScale() const;
    // The "fit" scale the current widget size would produce (contain-fit).
    // Used by the wheel handler to decide when zooming out should snap back.
    double fitWheelScale() const;
    // Deduplicated fullResRequested emitter.
    void setFullResRequested(bool needed);
    void setZoom(double factor);
    // The logical render size (widget size x devicePixelRatio).
    QSize logicalRenderSize() const;
    // Deduplicated viewportResolutionChanged emitter.
    void emitViewportResolutionChanged();
    QPointF widgetToImage(const QPointF& pos) const;
    void sampleAt(const QPointF& pos);
    // Point-colour reference sample: samples the pixel at widgetPos and emits
    // pointColorSampled.
    void samplePointColor(const QPointF& widgetPos);
    // White-balance sample: samples the pixel at widgetPos and emits
    // whiteBalanceSampled.
    void sampleWhiteBalance(const QPointF& widgetPos);
    // Colour-range sample: samples the pixel at widgetPos and emits
    // colorRangeSampled.
    void sampleColorRange(const QPointF& widgetPos);
    // Luminance-range sample: samples the pixel at widgetPos, computes the
    // linear relative luma and emits luminanceSampled (0-100 percent).
    void sampleLuminance(const QPointF& widgetPos);
    // Normalized image coordinates (nx, ny in [0,1]) -> widget coordinates.
    QPointF normToWidget(const QPointF& n) const;
    // Rebuilds the red weight overlay for the current mask draft.
    void rebuildMaskOverlay();
    void beginMaskGesture(const QPointF& widgetPos);
    void moveMaskGesture(const QPointF& widgetPos);
    void endMaskGesture();
    void emitMaskGeometry();

    // --- Spot removal / healing brush --------------------------------------
    // Widget coordinates -> normalized image coords ([0,1], clamped).
    QPointF widgetToNorm(const QPointF& pos) const;
    // Screen radius of a spot's circle = radius x longest image edge x scale.
    double spotWidgetRadius(const lrc::SpotRemoval& spot) const;
    // Top-most spot whose target or source circle contains widgetPos, or -1.
    // `source` is set true when the hit circle is the source circle.
    int spotHitTest(const QPointF& widgetPos, bool* source) const;
    void beginSpotGesture(const QPointF& widgetPos, bool alt);
    void moveSpotGesture(const QPointF& widgetPos);
    void endSpotGesture();
    void deleteSpot(int index);
    void emitSpotRemovals();
    void paintSpotOverlay(QPainter& painter);
    // Rebuilds the cached inverted + high-contrast visualization image.
    void rebuildSpotViz();
    // Restores the cursor after a pan/drag release based on the active tool.
    void applyToolCursor();

    // --- Before/After + reference view --------------------------------------
    void paintCompare(QPainter& painter);
    void drawDivider(QPainter& painter);
    // The shared target rect used for Before/After split rendering (falls back
    // to a contain-fit rect of the after image when image_ is null).
    QRectF compareTargetRect() const;
    QRectF centerRect(const QSizeF& scaled, const QRectF& pane) const;
    // 0 = none, 1 = vertical divider (LeftRight / Reference), 2 = horizontal.
    int dividerHit(const QPointF& pos) const;
    void updateDivider(const QPointF& pos);
    const QImage& afterImage() const;

    QImage image_;
    QImage overlay_;  // LR Alt-key diagnostic overlay (drawn in place of the image)
    // B-1 content-aware crop guidance box (normalized [0,1]) and its enable flag.
    bool autoCropActive_ = false;
    QRectF autoCropNorm_;
    QSize nativeSize_;      // native full-res size of the current image
    QSize lastViewportResolution_;  // dedup for viewportResolutionChanged()
    bool fullResRequested_ = false;  // dedup for fullResRequested()
    ZoomMode zoomMode_ = ZoomMode::Fit;
    ZoomMode lastFitMode_ = ZoomMode::Fit;  // fit mode before entering 1:1
    double zoom_ = 1.0;
    QPointF panOffset_;
    QPoint lastDragPos_;
    bool dragging_ = false;
    bool spacePan_ = false;  // Space held: left-drag pans without a mode switch

    bool samplerActive_ = false;
    int sampledRange_ = -1;
    QPointF samplerStart_;
    QPointF samplerMark_;   // last sampled position (for the overlay)
    bool hasSamplerMark_ = false;
    double samplerBaseSat_ = 0.0;
    double samplerBaseLum_ = 0.0;

    bool pointColorActive_ = false;
    QPointF pointColorMark_;   // fixed sample-point marker (does not follow mouse)
    bool hasPointColorMark_ = false;

    bool whiteBalanceActive_ = false;   // white-balance eyedropper active (W key).

    bool colorRangeActive_ = false;   // colour-range sampler active.
    QColor colorRangeSrgbRef_;        // current reference colour sRGB (for overlay).
    QPointF colorRangeMark_;          // sample marker position (widget pos).
    bool hasColorRangeMark_ = false;  // whether a sample marker exists.

    bool luminanceActive_ = false;    // luminance-range sampler active.
    QPointF luminanceMark_;           // sample marker position (widget pos).
    bool hasLuminanceMark_ = false;   // whether a sample marker exists.

    lrc::MaskType maskTool_ = lrc::MaskType::None;
    lrc::Mask maskDraft_;
    QImage maskOverlay_;     // red weight overlay (downscaled, <=480 px edge)
    bool maskOverlayVisible_ = true;   // whether the red weight overlay is drawn (shortcut O).
    bool maskDragging_ = false;
    QPointF maskPress_;      // widget pos at gesture start
    QPointF maskLastDab_;    // last brush dab (widget pos)
    QPointF lastMousePos_;   // current mouse pos (brush cursor)
    double maskBrushRadius_ = 0.05;
    double maskBrushSoftness_ = 0.5;  // brush dab softness (0..1)
    int maskPromptLabel_ = 1;         // label for the next click-prompt point (1=fg,0=bg)
    size_t lastOverlayHash_ = 0;      // geometry hash of the built mask overlay

    // --- Spot removal / healing brush --------------------------------------
    bool spotToolActive_ = false;
    lrc::SpotType spotType_ = lrc::SpotType::Heal;
    double spotRadius_ = 0.02;        // normalized against the longer edge
    std::vector<lrc::SpotRemoval> spotRemovals_;
    int selectedSpot_ = -1;           // index of the selected spot (-1 = none)
    int draggingSpot_ = -1;           // index of the spot being dragged
    bool draggingSpotSource_ = false; // true = dragging the source circle
    QPointF spotDragLastNorm_;        // last normalized drag position
    bool spotVisualize_ = false;      // inverted high-contrast display mode
    QImage spotViz_;                  // cached visualization image
    bool spotVizValid_ = false;       // spotViz_ matches the current image_

    // --- Before/After + reference view --------------------------------------
    QImage beforeImage_;              // zero-adjustment render
    QImage afterImage_;               // current render (falls back to image_)
    QImage referenceImage_;           // read-only reference photo
    CompareMode compareMode_ = CompareMode::Off;
    double divider_ = 0.5;            // divider position, 0..1 of the widget
    bool dividerDragging_ = false;
    bool singleShowsBefore_ = false;  // SingleToggle: which side is shown
};
