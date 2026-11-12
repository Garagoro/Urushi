#pragma once
#include <mutex>
#include <unordered_set>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

// Pointer provenance for the Lua <-> native boundary.
//
// Lua only ever sees native objects (cars, Unity GameObjects/Transforms) as light userdata that C++ itself handed out.
// Every native function that takes such a pointer must obtain it through PtrRegistry::Arg(), which rejects anything
// C++ did not hand out in this Lua session (forged integers, pointers read out of raw memory, nil-vs-garbage mixups).
// Without this a script could aim a "set position" / "destroy" call at an arbitrary address = memory write primitive.
namespace PtrRegistry {

namespace detail {
inline std::mutex& Mtx() { static std::mutex m; return m; }
inline std::unordered_set<const void*>& Set() { static std::unordered_set<const void*> s; return s; }
constexpr size_t kMaxTracked = 400000;
}

inline void Track(const void* p) {
    if (!p) return;
    std::lock_guard<std::mutex> lk(detail::Mtx());
    if (detail::Set().size() >= detail::kMaxTracked) detail::Set().clear();   // scripts simply re-query objects
    detail::Set().insert(p);
}

inline bool Has(const void* p) {
    std::lock_guard<std::mutex> lk(detail::Mtx());
    return detail::Set().count(p) != 0;
}

inline void Forget(const void* p) {
    std::lock_guard<std::mutex> lk(detail::Mtx());
    detail::Set().erase(p);
}

inline void Clear() {
    std::lock_guard<std::mutex> lk(detail::Mtx());
    detail::Set().clear();
}

// Push a native pointer to Lua and remember that we handed it out.
inline void Push(lua_State* L, void* p) {
    Track(p);
    lua_pushlightuserdata(L, p);
}

// Read a native pointer argument. nil/none -> nullptr. Anything not handed out by Push() raises a Lua error.
inline void* Arg(lua_State* L, int idx) {
    if (lua_isnoneornil(L, idx)) return nullptr;
    if (!lua_islightuserdata(L, idx)) luaL_error(L, "bad argument #%d: native object expected", idx < 0 ? lua_gettop(L) + idx + 1 : idx);
    void* p = lua_touserdata(L, idx);
    if (p && !Has(p)) luaL_error(L, "bad argument #%d: unknown native pointer", idx < 0 ? lua_gettop(L) + idx + 1 : idx);
    return p;
}

} // namespace PtrRegistry
