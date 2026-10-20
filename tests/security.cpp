#include "LuaSafety.hpp"
#include "Material.hpp"
#include "RenderSafety.hpp"

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
}

int main() {
    geometry();
    luaErrors();
}
