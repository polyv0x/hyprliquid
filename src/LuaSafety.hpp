#pragma once

#include <cstdio>
#include <stdexcept>
#include <string_view>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace LuaSafety {

struct Error {};

inline void reserve(lua_State* L, int count) {
    if (!lua_checkstack(L, count))
        throw std::runtime_error("hyprliquid: Lua stack exhausted");
}

inline void call(lua_State* L, int args, int results) {
    if (lua_pcall(L, args, results, 0) != LUA_OK)
        throw Error{}; // Leave the original Lua error on the stack.
}

// Every allocating/metamethod-capable Lua operation runs in a C-only frame.
// Its longjmp stops at pcall; C++ owners unwind normally via Error instead.
inline void newTable(lua_State* L) {
    reserve(L, 1);
    lua_pushcfunction(L, +[](lua_State* inner) { lua_newtable(inner); return 1; });
    call(L, 0, 1);
}

inline void getGlobal(lua_State* L, const char* key) {
    reserve(L, 2);
    lua_pushcfunction(L, +[](lua_State* inner) {
        lua_getglobal(inner, *static_cast<const char**>(lua_touserdata(inner, 1)));
        return 1;
    });
    lua_pushlightuserdata(L, &key);
    call(L, 1, 1);
}

inline void getField(lua_State* L, int index, const char* key) {
    index = lua_absindex(L, index);
    reserve(L, 3);
    lua_pushcfunction(L, +[](lua_State* inner) {
        lua_getfield(inner, 1, *static_cast<const char**>(lua_touserdata(inner, 2)));
        return 1;
    });
    lua_pushvalue(L, index);
    lua_pushlightuserdata(L, &key);
    call(L, 2, 1);
}

inline void setField(lua_State* L, int index, const char* key) {
    index = lua_absindex(L, index);
    const int value = lua_gettop(L);
    reserve(L, 4);
    lua_pushcfunction(L, +[](lua_State* inner) {
        lua_setfield(inner, 1, *static_cast<const char**>(lua_touserdata(inner, 2)));
        return 0;
    });
    lua_pushvalue(L, index);
    lua_pushlightuserdata(L, &key);
    lua_pushvalue(L, value);
    call(L, 3, 0);
    lua_pop(L, 1);
}

inline bool next(lua_State* L, int index) {
    index = lua_absindex(L, index);
    const int key = lua_gettop(L);
    reserve(L, 3);
    lua_pushcfunction(L, +[](lua_State* inner) { return lua_next(inner, 1) ? 2 : 0; });
    lua_pushvalue(L, index);
    lua_pushvalue(L, key);
    lua_remove(L, key);
    call(L, 2, LUA_MULTRET);
    return lua_gettop(L) > key - 1;
}

// Call only after checking LUA_TSTRING: lua_isstring also accepts numbers,
// whose conversion would replace an iteration key and invalidate lua_next.
inline std::string_view stringView(lua_State* L, int index) {
    size_t length = 0;
    const char* value = lua_tolstring(L, index, &length);
    return {value, length};
}

template <int (*Function)(lua_State*)>
int boundary(lua_State* L) {
    bool nativeError = false;
    char message[1024]{};
    try {
        return Function(L);
    } catch (const Error&) {
        // The protected operation left its error object on top.
    } catch (const std::exception& error) {
        nativeError = true;
        std::snprintf(message, sizeof(message), "%s", error.what());
    } catch (...) {
        nativeError = true;
        std::snprintf(message, sizeof(message), "%s", "hyprliquid: native callback failed");
    }
    // No nontrivial automatic object (including an exception) is alive here.
    return nativeError ? luaL_error(L, "%s", message) : lua_error(L);
}

} // namespace LuaSafety
