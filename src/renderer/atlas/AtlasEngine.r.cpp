// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AtlasEngine.h"

#include "dwrite.h"

// #### NOTE ####
// If you see any code in here that contains "_api." you might be seeing a race condition.
// The AtlasEngine::Present() method is called on a background thread without any locks,
// while any of the API methods (like AtlasEngine::Invalidate) might be called concurrently.
// The usage of the _r field is safe as its members are in practice
// only ever written to by the caller of Present() (the "Renderer" class).
// The _api fields on the other hand are concurrently written to by others.

#pragma warning(disable : 4100) // '...': unreferenced formal parameter
#pragma warning(disable : 4127)
// Disable a bunch of warnings which get in the way of writing performant code.
#pragma warning(disable : 26429) // Symbol 'data' is never tested for nullness, it can be marked as not_null (f.23).
#pragma warning(disable : 26446) // Prefer to use gsl::at() instead of unchecked subscript operator (bounds.4).
#pragma warning(disable : 26459) // You called an STL function '...' with a raw pointer parameter at position '...' that may be unsafe [...].
#pragma warning(disable : 26481) // Don't use pointer arithmetic. Use span instead (bounds.1).
#pragma warning(disable : 26482) // Only index into arrays using constant expressions (bounds.2).

using namespace Microsoft::Console::Render::Atlas;

// https://en.wikipedia.org/wiki/Inversion_list
template<size_t N>
constexpr bool isInInversionList(const std::array<wchar_t, N>& ranges, wchar_t needle)
{
    const auto beg = ranges.begin();
    const auto end = ranges.end();
    decltype(ranges.begin()) it;

    // Linear search is faster than binary search for short inputs.
    if constexpr (N < 16)
    {
        it = std::find_if(beg, end, [=](wchar_t v) { return needle < v; });
    }
    else
    {
        it = std::upper_bound(beg, end, needle);
    }

    const auto idx = it - beg;
    return (idx & 1) != 0;
}

template<typename T = D2D1_COLOR_F>
constexpr T colorFromU32(uint32_t rgba)
{
    const auto r = static_cast<float>((rgba >> 0) & 0xff) / 255.0f;
    const auto g = static_cast<float>((rgba >> 8) & 0xff) / 255.0f;
    const auto b = static_cast<float>((rgba >> 16) & 0xff) / 255.0f;
    const auto a = static_cast<float>((rgba >> 24) & 0xff) / 255.0f;
    return { r, g, b, a };
}

static AtlasEngine::f32r getGlyphRunBlackBox(const DWRITE_GLYPH_RUN& glyphRun, float baselineX, float baselineY)
{
    DWRITE_FONT_METRICS fontMetrics;
    glyphRun.fontFace->GetMetrics(&fontMetrics);

    std::unique_ptr<DWRITE_GLYPH_METRICS[]> glyphRunMetricsHeap;
    std::array<DWRITE_GLYPH_METRICS, 8> glyphRunMetricsStack;
    DWRITE_GLYPH_METRICS* glyphRunMetrics = glyphRunMetricsStack.data();

    if (glyphRun.glyphCount > glyphRunMetricsStack.size())
    {
        glyphRunMetricsHeap = std::make_unique_for_overwrite<DWRITE_GLYPH_METRICS[]>(glyphRun.glyphCount);
        glyphRunMetrics = glyphRunMetricsHeap.get();
    }

    glyphRun.fontFace->GetDesignGlyphMetrics(glyphRun.glyphIndices, glyphRun.glyphCount, glyphRunMetrics, false);

    float const fontScale = glyphRun.fontEmSize / fontMetrics.designUnitsPerEm;
    AtlasEngine::f32r accumulatedBounds{
        FLT_MAX,
        FLT_MAX,
        FLT_MIN,
        FLT_MIN,
    };

    for (uint32_t i = 0; i < glyphRun.glyphCount; ++i)
    {
        const auto& glyphMetrics = glyphRunMetrics[i];
        const auto glyphAdvance = glyphRun.glyphAdvances ? glyphRun.glyphAdvances[i] : glyphMetrics.advanceWidth * fontScale;

        const auto left = static_cast<float>(glyphMetrics.leftSideBearing) * fontScale;
        const auto top = static_cast<float>(glyphMetrics.topSideBearing - glyphMetrics.verticalOriginY) * fontScale;
        const auto right = static_cast<float>(gsl::narrow_cast<INT32>(glyphMetrics.advanceWidth) - glyphMetrics.rightSideBearing) * fontScale;
        const auto bottom = static_cast<float>(gsl::narrow_cast<INT32>(glyphMetrics.advanceHeight) - glyphMetrics.bottomSideBearing - glyphMetrics.verticalOriginY) * fontScale;

        if (left < right && top < bottom)
        {
            auto glyphX = baselineX;
            auto glyphY = baselineY;
            if (glyphRun.glyphOffsets)
            {
                glyphX += glyphRun.glyphOffsets[i].advanceOffset;
                glyphY -= glyphRun.glyphOffsets[i].ascenderOffset;
            }

            accumulatedBounds.left = std::min(accumulatedBounds.left, left + glyphX);
            accumulatedBounds.top = std::min(accumulatedBounds.top, top + glyphY);
            accumulatedBounds.right = std::max(accumulatedBounds.right, right + glyphX);
            accumulatedBounds.bottom = std::max(accumulatedBounds.bottom, bottom + glyphY);
        }

        baselineX += glyphAdvance;
    }

    return accumulatedBounds;
}

#pragma region IRenderEngine

// Present() is called without the console buffer lock being held.
// --> Put as much in here as possible.
[[nodiscard]] HRESULT AtlasEngine::Present() noexcept
try
{
    const til::rect fullRect{ 0, 0, _r.cellCount.x, _r.cellCount.y };

    // A change in the selection or background color (etc.) forces a full redraw.
    if (WI_IsFlagSet(_r.invalidations, RenderInvalidations::ConstBuffer) || _r.customPixelShader)
    {
        _r.dirtyRect = fullRect;
    }

    if (!_r.dirtyRect)
    {
        return S_OK;
    }

    // See documentation for IDXGISwapChain2::GetFrameLatencyWaitableObject method:
    // > For every frame it renders, the app should wait on this handle before starting any rendering operations.
    // > Note that this requirement includes the first frame the app renders with the swap chain.
    assert(debugGeneralPerformance || _r.frameLatencyWaitableObjectUsed);

    if (_r.d2dMode)
    {
        if (!_r.d2dRenderTarget)
        {
            {
                wil::com_ptr<ID3D11Texture2D> buffer;
                THROW_IF_FAILED(_r.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), buffer.put_void()));

                const auto surface = buffer.query<IDXGISurface>();

                D2D1_RENDER_TARGET_PROPERTIES props{};
                props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
                props.pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED };
                props.dpiX = static_cast<float>(_r.dpi);
                props.dpiY = static_cast<float>(_r.dpi);
                wil::com_ptr<ID2D1RenderTarget> renderTarget;
                THROW_IF_FAILED(_sr.d2dFactory->CreateDxgiSurfaceRenderTarget(surface.get(), &props, renderTarget.addressof()));
                _r.d2dRenderTarget = renderTarget.query<ID2D1DeviceContext>();
                _r.d2dRenderTarget4 = renderTarget.query<ID2D1DeviceContext4>();

                // In case _api.realizedAntialiasingMode is D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE we'll
                // continuously adjust it in AtlasEngine::_drawGlyph. See _drawGlyph.
                _r.d2dRenderTarget->SetTextAntialiasMode(static_cast<D2D1_TEXT_ANTIALIAS_MODE>(_api.realizedAntialiasingMode));
            }
            {
                static constexpr D2D1_COLOR_F color{ 1, 1, 1, 1 };
                THROW_IF_FAILED(_r.d2dRenderTarget->CreateSolidColorBrush(&color, nullptr, _r.brush.put()));
                _r.brushColor = 0xffffffff;
            }
            {
                D2D1_BITMAP_PROPERTIES props{};
                props.pixelFormat = { DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED };
                props.dpiX = static_cast<float>(_r.dpi);
                props.dpiY = static_cast<float>(_r.dpi);
                THROW_IF_FAILED(_r.d2dRenderTarget->CreateBitmap({ _r.cellCount.x, _r.cellCount.y }, props, _r.d2dBackgroundBitmap.put()));
                THROW_IF_FAILED(_r.d2dRenderTarget->CreateBitmapBrush(_r.d2dBackgroundBitmap.get(), _r.d2dBackgroundBrush.put()));
                _r.d2dBackgroundBrush->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                _r.d2dBackgroundBrush->SetTransform(D2D1::Matrix3x2F::Scale(_r.fontMetrics.cellSize.x, _r.fontMetrics.cellSize.y));
            }
        }

        _r.d2dRenderTarget->BeginDraw();

        _r.d2dBackgroundBitmap->CopyFromMemory(nullptr, _r.backgroundBitmap.data(), _r.cellCount.x * 4);
        _r.d2dRenderTarget->FillRectangle({ 0, 0, _r.cellCount.x * _r.cellSizeDIP.x, _r.cellCount.y * _r.cellSizeDIP.y }, _r.d2dBackgroundBrush.get());

        size_t y = 0;
        for (const auto& row : _r.rows)
        {
            for (const auto& m : row.mappings)
            {
                DWRITE_GLYPH_RUN glyphRun{};
                glyphRun.fontFace = m.fontFace.get();
                glyphRun.fontEmSize = m.fontEmSize;
                glyphRun.glyphCount = m.glyphsTo - m.glyphsFrom;
                glyphRun.glyphIndices = &row.glyphIndices[m.glyphsFrom];
                glyphRun.glyphAdvances = &row.glyphAdvances[m.glyphsFrom];
                glyphRun.glyphOffsets = &row.glyphOffsets[m.glyphsFrom];

                const D2D1_POINT_2F baseline{
                    0, // TODO
                    _r.cellSizeDIP.y * y + _r.fontMetrics.baselineInDIP,
                };

                _drawGlyphRun(baseline, &glyphRun, _r.brush.get());
            }

            y++;
        }

        THROW_IF_FAILED(_r.d2dRenderTarget->EndDraw());
    }
    else
    {
    }

    if (false && _r.dirtyRect != fullRect)
    {
        auto dirtyRectInPx = _r.dirtyRect;
        dirtyRectInPx.left *= _r.fontMetrics.cellSize.x;
        dirtyRectInPx.top *= _r.fontMetrics.cellSize.y;
        dirtyRectInPx.right *= _r.fontMetrics.cellSize.x;
        dirtyRectInPx.bottom *= _r.fontMetrics.cellSize.y;

        RECT scrollRect{};
        POINT scrollOffset{};
        DXGI_PRESENT_PARAMETERS params{
            .DirtyRectsCount = 1,
            .pDirtyRects = dirtyRectInPx.as_win32_rect(),
        };

        if (_r.scrollOffset)
        {
            scrollRect = {
                0,
                std::max<til::CoordType>(0, _r.scrollOffset),
                _r.cellCount.x,
                _r.cellCount.y + std::min<til::CoordType>(0, _r.scrollOffset),
            };
            scrollOffset = {
                0,
                _r.scrollOffset,
            };

            scrollRect.top *= _r.fontMetrics.cellSize.y;
            scrollRect.right *= _r.fontMetrics.cellSize.x;
            scrollRect.bottom *= _r.fontMetrics.cellSize.y;

            scrollOffset.y *= _r.fontMetrics.cellSize.y;

            params.pScrollRect = &scrollRect;
            params.pScrollOffset = &scrollOffset;
        }

        THROW_IF_FAILED(_r.swapChain->Present1(1, 0, &params));
    }
    else
    {
        THROW_IF_FAILED(_r.swapChain->Present(1, 0));
    }

    _r.waitForPresentation = true;

    if (!_r.dxgiFactory->IsCurrent())
    {
        WI_SetFlag(_api.invalidations, ApiInvalidations::Device);
    }

    return S_OK;
}
catch (const wil::ResultException& exception)
{
    // TODO: this writes to _api.
    return _handleException(exception);
}
CATCH_RETURN()

[[nodiscard]] bool AtlasEngine::RequiresContinuousRedraw() noexcept
{
    return debugGeneralPerformance || _r.requiresContinuousRedraw;
}

void AtlasEngine::WaitUntilCanRender() noexcept
{
    // IDXGISwapChain2::GetFrameLatencyWaitableObject returns an auto-reset event.
    // Once we've waited on the event, waiting on it again will block until the timeout elapses.
    // _r.waitForPresentation guards against this.
    if (std::exchange(_r.waitForPresentation, false))
    {
        WaitForSingleObjectEx(_r.frameLatencyWaitableObject.get(), 100, true);
#ifndef NDEBUG
        _r.frameLatencyWaitableObjectUsed = true;
#endif
    }
}

#pragma endregion

bool AtlasEngine::_drawGlyphRun(D2D_POINT_2F baselineOrigin, const DWRITE_GLYPH_RUN* glyphRun, ID2D1SolidColorBrush* foregroundBrush) const noexcept
{
    static constexpr auto measuringMode = DWRITE_MEASURING_MODE_NATURAL;
    static constexpr auto formats =
        DWRITE_GLYPH_IMAGE_FORMATS_TRUETYPE |
        DWRITE_GLYPH_IMAGE_FORMATS_CFF |
        DWRITE_GLYPH_IMAGE_FORMATS_COLR |
        DWRITE_GLYPH_IMAGE_FORMATS_SVG |
        DWRITE_GLYPH_IMAGE_FORMATS_PNG |
        DWRITE_GLYPH_IMAGE_FORMATS_JPEG |
        DWRITE_GLYPH_IMAGE_FORMATS_TIFF |
        DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8;

    wil::com_ptr<IDWriteColorGlyphRunEnumerator1> enumerator;

    // If ID2D1DeviceContext4 isn't supported, we'll exit early below.
    auto hr = DWRITE_E_NOCOLOR;

    if (_r.d2dRenderTarget4)
    {
        D2D_MATRIX_3X2_F transform;
        _r.d2dRenderTarget4->GetTransform(&transform);
        float dpiX, dpiY;
        _r.d2dRenderTarget4->GetDpi(&dpiX, &dpiY);
        transform = transform * D2D1::Matrix3x2F::Scale(dpiX, dpiY);

        // Support for ID2D1DeviceContext4 implies support for IDWriteFactory4.
        // ID2D1DeviceContext4 is required for drawing below.
        hr = _sr.dwriteFactory4->TranslateColorGlyphRun(baselineOrigin, glyphRun, nullptr, formats, measuringMode, nullptr, 0, &enumerator);
    }

    if (hr == DWRITE_E_NOCOLOR)
    {
        _r.d2dRenderTarget->DrawGlyphRun(baselineOrigin, glyphRun, foregroundBrush, measuringMode);
        return false;
    }

    THROW_IF_FAILED(hr);

    const auto previousAntialiasingMode = _r.d2dRenderTarget4->GetTextAntialiasMode();
    _r.d2dRenderTarget4->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    const auto cleanup = wil::scope_exit([&]() {
        _r.d2dRenderTarget4->SetTextAntialiasMode(previousAntialiasingMode);
    });

    wil::com_ptr<ID2D1SolidColorBrush> solidBrush;

    for (;;)
    {
        BOOL hasRun;
        THROW_IF_FAILED(enumerator->MoveNext(&hasRun));
        if (!hasRun)
        {
            break;
        }

        const DWRITE_COLOR_GLYPH_RUN1* colorGlyphRun;
        THROW_IF_FAILED(enumerator->GetCurrentRun(&colorGlyphRun));

        ID2D1Brush* runBrush;
        if (colorGlyphRun->paletteIndex == /*DWRITE_NO_PALETTE_INDEX*/ 0xffff)
        {
            runBrush = foregroundBrush;
        }
        else
        {
            if (!solidBrush)
            {
                THROW_IF_FAILED(_r.d2dRenderTarget4->CreateSolidColorBrush(colorGlyphRun->runColor, &solidBrush));
            }
            else
            {
                solidBrush->SetColor(colorGlyphRun->runColor);
            }
            runBrush = solidBrush.get();
        }

        switch (colorGlyphRun->glyphImageFormat)
        {
        case DWRITE_GLYPH_IMAGE_FORMATS_NONE:
            break;
        case DWRITE_GLYPH_IMAGE_FORMATS_PNG:
        case DWRITE_GLYPH_IMAGE_FORMATS_JPEG:
        case DWRITE_GLYPH_IMAGE_FORMATS_TIFF:
        case DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8:
            _r.d2dRenderTarget4->DrawColorBitmapGlyphRun(colorGlyphRun->glyphImageFormat, baselineOrigin, &colorGlyphRun->glyphRun, colorGlyphRun->measuringMode, D2D1_COLOR_BITMAP_GLYPH_SNAP_OPTION_DEFAULT);
            break;
        case DWRITE_GLYPH_IMAGE_FORMATS_SVG:
            _r.d2dRenderTarget4->DrawSvgGlyphRun(baselineOrigin, &colorGlyphRun->glyphRun, runBrush, nullptr, 0, colorGlyphRun->measuringMode);
            break;
        default:
            _r.d2dRenderTarget4->DrawGlyphRun(baselineOrigin, &colorGlyphRun->glyphRun, colorGlyphRun->glyphRunDescription, runBrush, colorGlyphRun->measuringMode);
            break;
        }
    }

    return true;
}

void AtlasEngine::_drawGlyph(GlyphCacheEntry& entry, f32 fontEmSize)
{
    DWRITE_GLYPH_RUN glyphRun{};
    glyphRun.fontFace = entry.fontFace;
    glyphRun.fontEmSize = fontEmSize;
    glyphRun.glyphCount = 1;
    glyphRun.glyphIndices = &entry.glyphIndex;

    auto box = getGlyphRunBlackBox(glyphRun, 0, 0);
    if (box.left >= box.right || box.top >= box.bottom)
    {
        return;
    }

    box.left = roundf(box.left * _r.pixelPerDIP) - 1.0f;
    box.top = roundf(box.top * _r.pixelPerDIP) - 1.0f;
    box.right = roundf(box.right * _r.pixelPerDIP) + 1.0f;
    box.bottom = roundf(box.bottom * _r.pixelPerDIP) + 1.0f;

    stbrp_rect rect{};
    rect.w = gsl::narrow_cast<int>(box.right - box.left);
    rect.h = gsl::narrow_cast<int>(box.bottom - box.top);
    if (!stbrp_pack_rects(&_r.rectPacker, &rect, 1))
    {
        __debugbreak();
        return;
    }

    const D2D1_POINT_2F baseline{
        (rect.x - box.left) * _r.dipPerPixel,
        (rect.y - box.top) * _r.dipPerPixel,
    };
    const auto colorGlyph = _drawGlyphRun(baseline, &glyphRun, _r.brush.get());

    entry.xy.x = gsl::narrow_cast<u16>(rect.x);
    entry.xy.y = gsl::narrow_cast<u16>(rect.y);
    entry.wh.x = gsl::narrow_cast<u16>(rect.w);
    entry.wh.y = gsl::narrow_cast<u16>(rect.h);
    entry.offset.x = gsl::narrow_cast<u16>(box.left);
    entry.offset.y = gsl::narrow_cast<u16>(box.top);
    entry.colorGlyph = colorGlyph;
}
