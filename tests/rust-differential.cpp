#include "Material.hpp"
#include "RenderSafety.hpp"
#include "reference/material.hpp"
#include "reference/render-safety.hpp"

#include <bit>
#include <cassert>
#include <iostream>
#include <random>

namespace {
void equal(const SMaterial& a, const SMaterial& b) {
    assert(a.family == b.family && a.darkTheme == b.darkTheme);
#define GLASS_PARAM(name, ...) assert(std::bit_cast<uint32_t>(a.name) == std::bit_cast<uint32_t>(b.name));
#include "MaterialParams.inc"
#undef GLASS_PARAM
}

void compare(SMaterial material) {
    auto reference = material;
    reference_sanitizeMaterial(reference);
    sanitizeMaterial(material);
    equal(material, reference);
    assert(effectiveLiquidMaterial(material) == reference_effectiveLiquidMaterial(reference));
    assert(materialBlurSigma(material) == reference_materialBlurSigma(reference));
    assert(materialOutputExtent(material) == reference_materialOutputExtent(reference));
    for (float scale : {0.0f, 0.1f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f, 1e6f, -1.0f,
                        std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        assert(materialPadding(material, scale) == reference_materialPadding(reference, scale));
}
}

int main() {
    std::mt19937_64 random(0xc001c0de);
    for (bool dark : {false, true}) {
        const auto defaults = liquidControlDefaults(dark);
        equal(defaults, reference_liquidControlDefaults(dark));
        assert(effectiveLiquidMaterial(defaults) == liquidMaterial(dark));
        // Single controls, extrema and special floats expose accidental f32
        // round-trips of unrelated structured values and float operation order.
#define GLASS_PARAM(name, value, lo, hi) \
        for (float scalar : {value, lo, hi, -0.0f, std::numeric_limits<float>::infinity(), \
                             -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) { \
            auto m = defaults; m.name = scalar; compare(m); \
        }
#include "MaterialParams.inc"
#undef GLASS_PARAM
        for (bool liquid : {false, true}) {
            for (int i = 0; i < 10000; ++i) {
                auto material = defaults;
                material.family = liquid ? EMaterialFamily::LIQUID : EMaterialFamily::LEGACY;
#define GLASS_PARAM(name, value, lo, hi) \
                material.name = i % 2 ? std::bit_cast<float>(static_cast<uint32_t>(random())) \
                    : std::uniform_real_distribution<float>(lo, hi)(random);
#include "MaterialParams.inc"
#undef GLASS_PARAM
                compare(material);
            }
        }
    }
    for (int i = 0; i < 100000; ++i) {
        auto coordinate = [&] { return i % 2 ? std::bit_cast<double>(random())
            : std::uniform_real_distribution<double>(-1e6, 1e6)(random); };
        const double x = coordinate(), y = coordinate(), w = coordinate(), h = coordinate();
        const int padding = random() % 10000;
        assert(RenderSafety::integerCoordinate(x) == ReferenceRenderSafety::integerCoordinate(x));
        assert(RenderSafety::dimension(w) == ReferenceRenderSafety::dimension(w));
        const auto expected = ReferenceRenderSafety::sampleGeometry(x, y, w, h, padding);
        const auto actual = RenderSafety::sampleGeometry(x, y, w, h, padding);
        assert(expected.has_value() == actual.has_value());
        if (expected) {
            assert(expected->width == actual->width && expected->height == actual->height);
            assert(expected->x0 == actual->x0 && expected->y0 == actual->y0);
            assert(expected->x1 == actual->x1 && expected->y1 == actual->y1 && expected->owner == actual->owner);
        }
        const int value = random() % 100001, from0 = -int(random() % 100001), from1 = 100001 + random() % 100001;
        const int to0 = random() % 100001, to1 = random() % 100001;
        assert(RenderSafety::mapBlitCoordinate(value, from0, from1, to0, to1) ==
               ReferenceRenderSafety::mapBlitCoordinate(value, from0, from1, to0, to1));
    }
    assert(!RenderSafety::mapBlitCoordinate(1, 0, 0, 0, 1));
    assert(!RenderSafety::mapBlitCoordinate(INT32_MAX, 0, 1, 0, INT32_MAX));
    assert(RenderSafety::mapBlitCoordinate(INT32_MAX, INT32_MIN, INT32_MAX, INT32_MIN, INT32_MAX) == INT32_MAX);
    assert(!RenderSafety::bufferBytes(INT32_MAX, INT32_MAX, 1, 1, 8));
    assert(RenderSafety::bufferBytes(7, 5, 4, 4, 16) == 64);
    std::cout << "PASS: Rust/C++ differential material equations (40000 combinations), scalar precision, and 100000 geometry cases\n";
}
