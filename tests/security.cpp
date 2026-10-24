#include "LuaSafety.hpp"
#include "Material.hpp"
#include "BuiltInPresets.hpp"
#include "RenderSafety.hpp"
#include "ResizeAngle.hpp"
#include "PointerAngle.hpp"

#include <bit>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <random>

extern "C" {
#include <lualib.h>
}

namespace {
int liveOwners = 0;
struct Owner {
    Owner() { ++liveOwners; }
    ~Owner() { --liveOwners; }
};
struct Allocator {
    bool fail = false;
    lua_Alloc original = nullptr;
    void* originalContext = nullptr;
};

void* allocate(void* context, void* pointer, size_t oldSize, size_t size) {
    const auto& allocator = *static_cast<Allocator*>(context);
    if (size && allocator.fail) return nullptr;
    return allocator.original(allocator.originalContext, pointer, oldSize, size);
}

int readField(lua_State* L) {
    Owner owner;
    LuaSafety::getField(L, 1, "value");
    return 1;
}
int nativeFailure(lua_State*) {
    Owner owner;
    throw std::runtime_error("native failure");
}
int outOfMemory(lua_State* L) {
    Owner owner;
    void* context = nullptr;
    lua_getallocf(L, &context);
    static_cast<Allocator*>(context)->fail = true;
    LuaSafety::newTable(L);
    return 1;
}
int copyStrings(lua_State* L) {
    Owner owner;
    LuaSafety::newTable(L);
    const int output = lua_gettop(L);
    lua_pushnil(L);
    while (LuaSafety::next(L, 1)) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            const auto key = LuaSafety::stringView(L, -2);
            if (key.find('\0') == std::string_view::npos) {
                lua_pushvalue(L, -1);
                LuaSafety::setField(L, output, key.data());
            }
        }
        lua_pop(L, 1);
    }
    return 1;
}

void luaErrors() {
    Allocator allocator;
    lua_State* L = luaL_newstate();
    assert(L);
    allocator.original = lua_getallocf(L, &allocator.originalContext);
    lua_setallocf(L, allocate, &allocator);
    luaL_openlibs(L);
    lua_pushcfunction(L, LuaSafety::boundary<readField>); lua_setglobal(L, "read_field");
    lua_pushcfunction(L, LuaSafety::boundary<nativeFailure>); lua_setglobal(L, "native_failure");
    lua_pushcfunction(L, LuaSafety::boundary<copyStrings>); lua_setglobal(L, "copy_strings");
    const char* checks = R"(
        for i = 1, 100 do
            local ok, err = pcall(read_field, setmetatable({}, {__index = function() error('metamethod failure') end}))
            assert(not ok and err:find('metamethod failure'))
            ok, err = pcall(native_failure)
            assert(not ok and err:find('native failure'))
            local input = {[1] = 0.2, [17] = 0.4, good = 0.5, ['bad\0suffix'] = 1}
            local result = copy_strings(input)
            assert(result.good == 0.5 and result[1] == nil and result.bad == nil)
            assert(input[1] == 0.2 and input[17] == 0.4)
        end
    )";
    assert(luaL_dostring(L, checks) == LUA_OK);
    assert(liveOwners == 0);
    lua_pushcfunction(L, LuaSafety::boundary<outOfMemory>);
    const int status = lua_pcall(L, 0, 1, 0);
    allocator.fail = false;
    assert(status != LUA_OK && liveOwners == 0);
    lua_close(L);
    std::cout << "PASS: Lua iteration, metamethod errors, native exceptions and allocation failure unwind owners\n";
}

void geometry() {
    using namespace RenderSafety;
    const auto regular = sampleGeometry(-10, 15, 640, 480, 60);
    assert(regular && regular->width == 760 && regular->x0 == -70 && regular->x1 == 690);
    assert(!sampleGeometry(0, 0, 2147483647.0, 10, 100));
    assert(!sampleGeometry(-2147483648.0, 0, 1, 1, 1));
    assert(!sampleGeometry(2147483647.0, 0, 1, 1, 0));
    assert(!sampleGeometry(0, 0, 1, 1, std::numeric_limits<int>::max()));
    assert(!sampleGeometry(0, 0, 1, 1, -1));
    assert(!sampleGeometry(0, 0, -1, 1, 0));
    assert(mapBlitCoordinate(65536, 0, 65536, 0, 65536) == 65536);
    assert(mapBlitCoordinate(32768, 0, 65536, 100, 65636) == 32868);
    SMaterial material;
    assert(materialPadding(material, std::numeric_limits<float>::infinity()) == 0);
    assert(materialPadding(material, std::numeric_limits<float>::max()) == 0);
    assert(materialPadding(material, std::numeric_limits<float>::quiet_NaN()) == 0);
    std::mt19937_64 random(0x51afe);
    for (int i = 0; i < 100000; ++i) {
        const double x = std::bit_cast<double>(random()), y = std::bit_cast<double>(random());
        const double w = std::bit_cast<double>(random()), h = std::bit_cast<double>(random());
        const int padding = static_cast<int>(random() % 2147483648ULL);
        const auto box = sampleGeometry(x, y, w, h, padding);
        if (box) {
            assert(box->width > 0 && box->height > 0);
            assert(int64_t(box->x1) - box->x0 == box->width);
            assert(int64_t(box->y1) - box->y0 == box->height);
        }
        (void)materialPadding(material, std::bit_cast<float>(static_cast<uint32_t>(random())));
    }
    std::cout << "PASS: geometry overflow regressions and 100000 adversarial numeric inputs\n";
}

void materialFamilies() {
    auto presets = BuiltInPresets::getAll();
    presets["custom-liquid"].inherits = "frosted-liquid-medium";
    presets["custom-child"].inherits = "custom-liquid";
    assert(resolveMaterialFamily("custom-child", presets) == EMaterialFamily::LIQUID);
    assert(resolveMaterialFamily("frosted-glass-clear-edge", presets) == EMaterialFamily::LEGACY);
    presets["a"].inherits = "b"; presets["b"].inherits = "a";
    assert(resolveMaterialFamily("a", presets) == EMaterialFamily::LEGACY);
    assert(LIQUID_DEFAULT.refraction.outerHeight == 0 && LIQUID_DEFAULT.refraction.innerAmount == -60);
    assert(LIQUID_DEFAULT.blur.distances == (std::array<double, 5>{-83, -1, 0, 0, 0}));
    assert(LIQUID_DARK.shadow.opacity == 0.05 && LIQUID_DEFAULT.shadow.opacity == 0.15);
    assert(!LIQUID_DARK.highlight.colorHeadroom && !LIQUID_DEFAULT.face.ycc.dodgeFill);
    auto material = liquidControlDefaults(true);
    assert(materialBlurSigma(material) == 6 && materialOutputExtent(material) == 105);
    assert(materialPadding(material, 1) == 209);
    assert(materialPadding(material, std::numeric_limits<float>::infinity()) == 0);
    material.effect_scale = std::numeric_limits<float>::quiet_NaN();
    sanitizeMaterial(material);
    assert(material.effect_scale == 1 && material.family == EMaterialFamily::LIQUID);
    material.blur_sigma = 60;
    material.outer_refraction_amount = 120;
    material.outer_refraction_inv_height = 0.001f;
    material.shadow_offset = -30;
    material.blur_dist3 = 0.1f;
    material.refraction_threshold0 = 160;
    material.refraction_threshold1 = 0;
    sanitizeMaterial(material);
    const auto effective = effectiveLiquidMaterial(material);
    assert(materialNeedsNeighborScene(material));
    assert(effective.refraction.outerHeight > 999 && effective.refraction.outerAmount == 120);
    assert(effective.refraction.outerDistances[0] < effective.refraction.outerDistances[1]);
    assert(std::is_sorted(effective.blur.distances.begin(), effective.blur.distances.end()));
    assert(materialOutputExtent(material) == 127 && materialPadding(material, 1) == 519);
    material.refraction_opacity = 0;
    assert(effectiveLiquidMaterial(material).refraction.outerHeight == 0 && effectiveLiquidMaterial(material).refraction.innerAmount == 0);
    assert(!materialNeedsNeighborScene(material));
    for (bool dark : {false, true}) {
        auto local = liquidControlDefaults(dark);
        assert(!materialNeedsNeighborScene(local));
        local.inner_refraction_amount = 45;
        assert(materialNeedsNeighborScene(local));
        local.inner_refraction_amount = -45;
        local.outer_refraction_amount = 45;
        assert(materialNeedsNeighborScene(local));
        local.family = EMaterialFamily::LEGACY;
        local.inner_refraction_amount = local.outer_refraction_amount = 0;
        assert(materialNeedsNeighborScene(local));
    }
    std::cout << "PASS: typed material variants, family inheritance, disabled heights and sample/output extents\n";
}
}

static void resizeAngles() {
    CResizeAngle sweep;
    const CResizeAngle::Size begin{100, 100}, goal{300, 200};
    assert(sweep.update(45, true, begin, goal, begin) == 0);
    const auto first = sweep.update(45, true, begin, goal, {150, 125});
    assert(first > 0 && first < 45);
    assert(sweep.update(45, true, begin, goal, {200, 150}) == 45);
    assert(std::abs(sweep.update(45, true, begin, goal, {250, 175}) - first) < 1e-5);
    assert(sweep.update(45, true, begin, goal, goal) == 0);
    // Spring overshoot stays bounded, and a return before the goal follows it.
    assert(sweep.update(45, true, begin, goal, {320, 210}) == 0);
    assert(sweep.update(45, true, begin, goal, {280, 190}) > 0);
    assert(sweep.update(45, false, begin, goal, goal) == 0);
    assert(sweep.update(-45, true, begin, goal, {200, 150}) == -45);
    const CResizeAngle::Size interrupted{200, 150}, redirected{80, 300};
    assert(sweep.update(-45, true, interrupted, redirected, interrupted) == -45);
    const auto retargeted = sweep.update(-45, true, interrupted, redirected, {170, 187.5});
    assert(retargeted <= 0 && retargeted >= -45);
    // Changing amplitude/sign during a resize must not jump at the edit.
    assert(sweep.update(30, true, interrupted, redirected, {170, 187.5}) == retargeted);
    assert(sweep.update(30, true, interrupted, redirected, redirected) == 0);
    assert(sweep.update(30, true, goal, goal, goal) == 0);
    assert(sweep.update(30, true, begin, goal, {NAN, 150}) == 0);
    auto material = liquidControlDefaults(true);
    material.display_angle_degrees = -500;
    sanitizeMaterial(material);
    assert(material.display_angle_degrees == -180);
    material.display_angle_degrees = NAN;
    sanitizeMaterial(material);
    assert(material.display_angle_degrees == 0);
    std::cout << "PASS: resize highlight sweep, signed angles, spring bounds, retarget continuity and finite defaults\n";
}

static void pointerAngles() {
    assert(PointerAngle::lookup(0, -100) == 0);
    assert(PointerAngle::lookup(100, 0) == 90);
    assert(PointerAngle::lookup(0, 100) == 180);
    assert(PointerAngle::lookup(-100, 0) == -90);
    assert(!PointerAngle::lookup(0, 0) && !PointerAngle::lookup(4, 4));
    assert(!PointerAngle::lookup(NAN, 10) && !PointerAngle::lookup(10, INFINITY));
    double worst = 0;
    for (int x = -150; x <= 150; ++x) for (int y = -100; y <= 100; ++y) {
        const auto angle = PointerAngle::lookup(x, y);
        if (!angle) continue;
        const double exact = std::atan2(double(x), double(-y)) * 180 / std::numbers::pi;
        worst = std::max(worst, std::abs(std::remainder(*angle - exact, 360.0)));
    }
    assert(worst < 0.0002);
    assert(PointerAngle::nearest(179, -179) == 181);
    assert(PointerAngle::nearest(-179, 179) == -181);
    assert(PointerAngle::nearest(359, 1) == 361);
    assert(PointerAngle::wrap(720) == 0 && PointerAngle::wrap(NAN) == 0);
    assert(PointerAngle::nearest(INFINITY, 45) == 45);
    std::cout << "PASS: pointer lookup cardinal directions, center stability, finite inputs and shortest angle transitions (error " << worst << " degrees)\n";
}

int main() {
    geometry();
    materialFamilies();
    luaErrors();
    resizeAngles();
    pointerAngles();
}
