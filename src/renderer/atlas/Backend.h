#pragma once

#include <til/generational.h>

#include "common.h"

namespace Microsoft::Console::Render::Atlas
{
    struct TargetSettings
    {
        HWND hwnd = nullptr;
        bool enableTransparentBackground = false;
    };

    struct FontSettings
    {
        wil::com_ptr<IDWriteFontCollection> fontCollection;
        std::wstring fontName;
        std::vector<DWRITE_FONT_FEATURE> fontFeatures;
        std::vector<DWRITE_FONT_AXIS_VALUE> fontAxisValues;
        float baselineInDIP = 0.0f;
        float fontSizeInDIP = 0.0f;
        f32 advanceScale = 0;
        u16x2 cellSize;
        u16 fontWeight = 0;
        u16 underlinePos = 0;
        u16 underlineWidth = 0;
        u16 strikethroughPos = 0;
        u16 strikethroughWidth = 0;
        u16x2 doubleUnderlinePos;
        u16 thinLineWidth = 0;
        u16 dpi = 0;
    };

    struct CursorSettings
    {
        ATLAS_POD_OPS(CursorSettings)

        u32 cursorColor = INVALID_COLOR;
        u16 cursorType = gsl::narrow_cast<u16>(CursorType::Legacy);
        u8 heightPercentage = 20;
        u8 _padding = 0;
    };

    struct MiscellaneousSettings
    {
        u32 backgroundColor = 0;
        u32 selectionColor = 0x7fffffff;
        u8 antialiasingMode = D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE;
        std::wstring customPixelShaderPath;
        bool useRetroTerminalEffect = false;
    };

    struct Settings
    {
        static auto invalidated() noexcept
        {
            return til::generational<Settings>{
                til::generation_t{ 1 },
                til::generational<TargetSettings>{ til::generation_t{ 1 } },
                til::generational<FontSettings>{ til::generation_t{ 1 } },
                til::generational<CursorSettings>{ til::generation_t{ 1 } },
                til::generational<MiscellaneousSettings>{ til::generation_t{ 1 } },
            };
        }

        til::generational<TargetSettings> target;
        til::generational<FontSettings> font;
        til::generational<CursorSettings> cursor;
        til::generational<MiscellaneousSettings> misc;
        u16x2 targetSize;
        u16x2 cellCount;
    };

    struct FontDependents
    {
        wil::com_ptr<IDWriteTextFormat> textFormats[2][2];
        Buffer<DWRITE_FONT_AXIS_VALUE> textFormatAxes[2][2];
        wil::com_ptr<IDWriteTypography> typography;
        f32 dipPerPixel = 1.0f; // caches USER_DEFAULT_SCREEN_DPI / dpi
        f32 pixelPerDIP = 1.0f; // caches dpi / USER_DEFAULT_SCREEN_DPI
        f32x2 cellSizeDIP; // caches cellSize in DIP
    };

    struct Dependents
    {
        FontDependents font;
    };

    struct FontMapping
    {
        wil::com_ptr<IDWriteFontFace> fontFace;
        f32 fontEmSize = 0;
        u32 glyphsFrom = 0;
        u32 glyphsTo = 0;
    };

    struct ShapedRow
    {
        void clear() noexcept
        {
            mappings.clear();
            glyphIndices.clear();
            glyphAdvances.clear();
            glyphOffsets.clear();
            colors.clear();
            selectionFrom = 0;
            selectionTo = 0;
        }

        std::vector<FontMapping> mappings;
        std::vector<u16> glyphIndices;
        std::vector<f32> glyphAdvances; // same size as glyphIndices
        std::vector<DWRITE_GLYPH_OFFSET> glyphOffsets; // same size as glyphIndices
        std::vector<u32> colors;

        u16 selectionFrom = 0;
        u16 selectionTo = 0;
    };

    struct RenderingPayload
    {
        // Parameters which are constant across backends.
        wil::com_ptr<ID2D1Factory> d2dFactory;
        wil::com_ptr<IDWriteFactory2> dwriteFactory;
        wil::com_ptr<IDWriteFontFallback> systemFontFallback;
        wil::com_ptr<IDWriteTextAnalyzer1> textAnalyzer;
        wil::com_ptr<IDWriteRenderingParams1> renderingParams;
        f32 gamma = 0;
        f32 cleartypeEnhancedContrast = 0;
        f32 grayscaleEnhancedContrast = 0;
        std::function<void(HRESULT)> warningCallback;
        std::function<void(HANDLE)> swapChainChangedCallback;

        // Parameters which are constant for the existence of the backend.
        wil::com_ptr<IDXGIFactory3> dxgiFactory;

        // Parameters which change seldom.
        til::generational<Settings> s;
        Dependents d;

        // Parameters which change every frame.
        std::vector<ShapedRow> rows;
        std::vector<u32> backgroundBitmap;
        u16r cursorRect;
        til::rect dirtyRect;
        i16 scrollOffset = 0;
    };

    struct IBackend
    {
        virtual ~IBackend() = default;
        virtual void Render(const RenderingPayload& payload) = 0;
        virtual void WaitUntilCanRender() noexcept = 0;
    };
}
