#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <cstddef>

namespace Il2CppHelpers {

inline bool IsCanonPtr(uintptr_t p, size_t sz = 1) {
    return p >= 0x10000ULL && p <= 0x00007FFFFFFEFFFFULL && sz > 0 && (p + sz) > p;
}

template <typename T>
inline bool SafeRead(uintptr_t addr, T* out) {
    if (!out || !IsCanonPtr(addr, sizeof(T))) return false;
    __try {
        *out = *reinterpret_cast<const T*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
inline bool SafeWrite(uintptr_t addr, const T& val) {
    if (!IsCanonPtr(addr, sizeof(T))) return false;
    __try {
        *reinterpret_cast<T*>(addr) = val;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadCString(uintptr_t addr, char* buf, size_t maxLen);

// Resolves GameAssembly.dll base & s_TypeInfoDefinitionTable RVA (cached on success).
uintptr_t GetGameAssemblyBase();
uintptr_t GetTypeInfoDefinitionTableRva();

// Looks up an Il2CppClass* by (name, namespace) in s_TypeInfoDefinitionTable (optional fallback global RVA).
uintptr_t LookupTypeClass(const char* expectedName, const char* expectedNs, uintptr_t fallbackGlobalRva = 0);

// Resolves Il2CppClass::static_fields with a hint type index first, falling back to full scan + optional global RVA.
void* LookupTypeStaticFields(uint32_t hintTypeIdx, const char* expectedName, const char* expectedNs, uintptr_t fallbackGlobalRva = 0);

// Resolves MethodInfo::methodPointer for `methodName` on `klass` (Unity 6 +0x120 methodCount, fallback +0x118).
void* LookupClassMethod(uintptr_t klass, const char* methodName);

// Looks up an interface method slot on a managed object instance, with optional il2cpp_class_get_virtual_invoke_data fallback.
using t_LookupVirtualInvoke = void** (__fastcall*)(void* obj, void* ifaceTypeInfo, uint32_t slot);
bool LookupInterfaceSlot(void* obj, void* ifaceTypeInfo, uint32_t slot,
                         void** outFn, void** outMethodInfo,
                         t_LookupVirtualInvoke fallbackFn = nullptr);

} // namespace Il2CppHelpers
