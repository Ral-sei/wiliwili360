//
// Created by fang on 2022/8/22.
//

#pragma once

#include <qrcodegen.hpp>
#include <cstdint>
#include <climits>
#include <vector>

#include "view/svg_image.hpp"

using qrcodegen::QrCode;

// QR codes are grids of filled squares.  They used to be rasterised through
// lunasvg as one giant SVG path (one "M..h1v1h-1z" sub-path per module), but
// plutovg's smooth rasteriser hangs or faults on that shape on the Xbox 360
// (settings page froze; KD showed gray_convert_glyph_inner walking garbage --
// see docs/XBOX360_PORTING_PLAN.md "设置页崩溃").  Drawing the modules straight
// into a bitmap is both faster and immune to that rasteriser.
class QRImage : public SVGImage {
public:
    QRImage() {
        this->registerStringXMLAttribute("QRContent",
                                         [this](const std::string& value) { this->setImageFromQRContent(value); });

        this->registerColorXMLAttribute("QRBackground", [this](NVGcolor value) { this->setQRBackgroundColor(value); });

        this->registerColorXMLAttribute("QRColor", [this](NVGcolor value) { this->setQRMainColor(value); });

        this->registerFloatXMLAttribute("QRBorder", [this](float value) { this->setQRBorder((int)value); });
    }

    void setImageFromQRContent(const std::string& value) {
        this->qr = QrCode::encodeText(value.c_str(), QrCode::Ecc::LOW);
        this->updateQR();
    }

    void setQRMainColor(NVGcolor c) {
        this->mainColor = c;
        this->updateQR();
    }

    void setQRBackgroundColor(NVGcolor c) {
        this->backgroundColor = c;
        this->updateQR();
    }

    void setQRBorder(int border) {
        this->QRBorder = border;
        this->updateQR();
    }

    void updateQR() {
        const int size   = qr.getSize();
        const int border = this->QRBorder;
        // Xbox 360 builds disable C++ exceptions; reject invalid values without throwing.
        if (size <= 0 || border < 0 || border > INT_MAX / 2 || border * 2 > INT_MAX - size) return;

        // Draw every module as a scale x scale block so the texture stays
        // crisp when the view scales it up (37 modules -> 296 px at 8x).
        const int scale   = 8;
        const int modules = size + border * 2;
        // The guard above only bounds size + 2*border; the 8x upscale and the
        // RGBA allocation still have to fit in int / size_t.  The RGBA buffer
        // is px*px*4 bytes, so on a 32-bit build px must stay below 32768.
        if (modules > INT_MAX / scale) return;
        const int px = modules * scale;
        if (px <= 0 || (size_t)px > SIZE_MAX / 4u / (size_t)px) return;

        const unsigned char br = toByte(this->backgroundColor.r), bg = toByte(this->backgroundColor.g),
                            bb = toByte(this->backgroundColor.b), ba = toByte(this->backgroundColor.a);
        const unsigned char mr = toByte(this->mainColor.r), mg = toByte(this->mainColor.g),
                            mb = toByte(this->mainColor.b), ma = toByte(this->mainColor.a);

        std::vector<unsigned char> rgba((size_t)px * (size_t)px * 4u);
        for (int y = 0; y < px; y++) {
            unsigned char* row = rgba.data() + (size_t)y * (size_t)px * 4u;
            for (int x = 0; x < px; x++) {
                const int mx = x / scale - border;
                const int my = y / scale - border;
                const bool on = mx >= 0 && my >= 0 && mx < size && my < size && qr.getModule(mx, my);
                row[x * 4 + 0] = on ? mr : br;
                row[x * 4 + 1] = on ? mg : bg;
                row[x * 4 + 2] = on ? mb : bb;
                row[x * 4 + 3] = on ? ma : ba;
            }
        }

        NVGcontext* vg = brls::Application::getNVGContext();
        int tex        = nvgCreateImageRGBA(vg, px, px, 0, rgba.data());
        if (tex <= 0) return;
        this->innerSetImage(tex);
    }

    static View* create() { return new QRImage(); }

private:
    static unsigned char toByte(float v) {
        if (v <= 0.f) return 0;
        if (v >= 1.f) return 255;
        return (unsigned char)(v * 255.0f + 0.5f);
    }

    QrCode qr      = QrCode::encodeText("", QrCode::Ecc::LOW);
    NVGcolor mainColor       = nvgRGB(0, 0, 0);
    NVGcolor backgroundColor = nvgRGB(0xFF, 0xFF, 0xFF);
    int QRBorder             = 2;
};
