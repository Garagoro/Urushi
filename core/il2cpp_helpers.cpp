#include "il2cpp_helpers.h"
#include "pattern_scanner.h"
#include "game_build.h"
#include "logger.h"

#include <cstring>
#include <string>

namespace Il2CppHelpers {

namespace {

constexpr BranchRva kFbTypeInfoDefinitionTable = { 0x87EAEA0, 0x9C2F5C0 };

uintptr_t s_gaBase = 0;
uintptr_t s_rvaTypeInfoDefinitionTable = 0;

void EnsureInitialized() {
    if (!s_gaBase) {
        HMODULE hGA = GetModuleHandleW(L"GameAssembly.dll");
        if (!hGA) return;
        s_gaBase = reinterpret_cast<uintptr_t>(hGA);
    }
    if (!s_rvaTypeInfoDefinitionTable) {
        HMODULE hGA = reinterpret_cast<HMODULE>(s_gaBase);
        uintptr_t va = PatternScanner::FindRipOrFallback(
            hGA, "48 63 C8 48 8B 05 ? ? ? ? 48 8B 04 C8 C3", 7, 3,
            GameBuild::Pick(kFbTypeInfoDefinitionTable), 3, "TypeInfoDefinitionTable");
        if (va >= s_gaBase) {
            s_rvaTypeInfoDefinitionTable = va - s_gaBase;
        }
    }
}

void* ResolveStaticFieldsFromTypeInfo(uintptr_t klassPtr, const char* expectedName, const char* expectedNs) {
    if (!IsCanonPtr(klassPtr, 0xC0)) return nullptr;
    uintptr_t namePtr = 0;
    uintptr_t nsPtr = 0;
    uintptr_t staticFields = 0;
    if (!SafeRead(klassPtr + 0x10, &namePtr) || !SafeRead(klassPtr + 0x18, &nsPtr)) return nullptr;
    char nameBuf[96] = {};
    char nsBuf[96] = {};
    if (!SafeReadCString(namePtr, nameBuf, sizeof(nameBuf))) return nullptr;
    if (!SafeReadCString(nsPtr, nsBuf, sizeof(nsBuf))) return nullptr;
    if (strcmp(nameBuf, expectedName) != 0 || strcmp(nsBuf, expectedNs) != 0) return nullptr;
    if (!SafeRead(klassPtr + 0xB8, &staticFields)) return nullptr;
    return IsCanonPtr(staticFields, 0x20) ? reinterpret_cast<void*>(staticFields) : nullptr;
}

} // namespace

bool SafeReadCString(uintptr_t addr, char* buf, size_t maxLen) {
    if (!buf || maxLen < 2 || !IsCanonPtr(addr, 1)) return false;
    __try {
        const char* p = reinterpret_cast<const char*>(addr);
        size_t i = 0;
        for (; i + 1 < maxLen; ++i) {
            char c = p[i];
            buf[i] = c;
            if (c == '\0') return true;
        }
        buf[maxLen - 1] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        buf[0] = '\0';
        return false;
    }
}

uintptr_t GetGameAssemblyBase() {
    EnsureInitialized();
    return s_gaBase;
}

uintptr_t GetTypeInfoDefinitionTableRva() {
    EnsureInitialized();
    return s_rvaTypeInfoDefinitionTable;
}

uintptr_t LookupTypeClass(const char* expectedName, const char* expectedNs, uintptr_t fallbackGlobalRva) {
    EnsureInitialized();
    if (!s_gaBase || !expectedName || !expectedNs) return 0;

    if (s_rvaTypeInfoDefinitionTable) {
        uintptr_t tablePtr = 0;
        if (SafeRead(s_gaBase + s_rvaTypeInfoDefinitionTable, &tablePtr) && IsCanonPtr(tablePtr, sizeof(uintptr_t))) {
            for (uint32_t i = 1; i < 65536; ++i) {
                uintptr_t k = 0;
                if (!SafeRead(tablePtr + static_cast<uintptr_t>(i) * sizeof(uintptr_t), &k) || !IsCanonPtr(k, 0xC0)) continue;
                uintptr_t namePtr = 0, nsPtr = 0;
                if (!SafeRead(k + 0x10, &namePtr) || !SafeRead(k + 0x18, &nsPtr)) continue;
                char nameBuf[96] = {}, nsBuf[96] = {};
                if (SafeReadCString(namePtr, nameBuf, sizeof(nameBuf)) && SafeReadCString(nsPtr, nsBuf, sizeof(nsBuf))) {
                    if (strcmp(nameBuf, expectedName) == 0 && strcmp(nsBuf, expectedNs) == 0) {
                        return k;
                    }
                }
            }
        }
    }

    if (fallbackGlobalRva != 0) {
        uintptr_t klassPtr = 0;
        if (SafeRead(s_gaBase + fallbackGlobalRva, &klassPtr) && IsCanonPtr(klassPtr, 0xC0)) {
            return klassPtr;
        }
    }
    return 0;
}

void* LookupTypeStaticFields(uint32_t hintTypeIdx, const char* expectedName, const char* expectedNs, uintptr_t fallbackGlobalRva) {
    EnsureInitialized();
    if (!s_gaBase || !expectedName || !expectedNs) return nullptr;

    if (s_rvaTypeInfoDefinitionTable) {
        uintptr_t tablePtr = 0;
        if (SafeRead(s_gaBase + s_rvaTypeInfoDefinitionTable, &tablePtr) && IsCanonPtr(tablePtr, sizeof(uintptr_t))) {
            uintptr_t klassPtr = 0;
            if (hintTypeIdx > 0 && SafeRead(tablePtr + static_cast<uintptr_t>(hintTypeIdx) * sizeof(uintptr_t), &klassPtr)) {
                if (void* sf = ResolveStaticFieldsFromTypeInfo(klassPtr, expectedName, expectedNs)) {
                    return sf;
                }
            }
            for (uint32_t i = 1; i < 65536; ++i) {
                if (i == hintTypeIdx) continue;
                uintptr_t k = 0;
                if (!SafeRead(tablePtr + static_cast<uintptr_t>(i) * sizeof(uintptr_t), &k) || !k) continue;
                if (void* sf = ResolveStaticFieldsFromTypeInfo(k, expectedName, expectedNs)) {
                    Logger::Info(std::string("Il2CppHelpers: dynamically found ") + expectedName + " at type index " + std::to_string(i));
                    return sf;
                }
            }
        }
    }

    if (fallbackGlobalRva != 0) {
        uintptr_t klassPtr = 0;
        if (SafeRead(s_gaBase + fallbackGlobalRva, &klassPtr)) {
            return ResolveStaticFieldsFromTypeInfo(klassPtr, expectedName, expectedNs);
        }
    }
    return nullptr;
}

void* LookupClassMethod(uintptr_t klass, const char* methodName) {
    if (!klass || !methodName) return nullptr;
    uintptr_t methodsPtr = 0;
    uint16_t methodCount = 0;
    if (!SafeRead(klass + 0x98, &methodsPtr)) return nullptr;
    if (!SafeRead(klass + 0x120, &methodCount) || methodCount == 0 || methodCount > 512) {
        if (!SafeRead(klass + 0x118, &methodCount)) return nullptr;
    }
    if (!methodsPtr || methodCount == 0 || methodCount > 512) return nullptr;
    for (uint16_t i = 0; i < methodCount; ++i) {
        uintptr_t mi = 0;
        if (!SafeRead(methodsPtr + static_cast<size_t>(i) * sizeof(uintptr_t), &mi) || !mi) continue;
        uintptr_t namePtr = 0;
        uintptr_t fnPtr = 0;
        if (!SafeRead(mi + 0x18, &namePtr) || !SafeRead(mi + 0x00, &fnPtr)) continue;
        char nameBuf[64] = {};
        if (SafeReadCString(namePtr, nameBuf, sizeof(nameBuf)) && strcmp(nameBuf, methodName) == 0) {
            return reinterpret_cast<void*>(fnPtr);
        }
    }
    return nullptr;
}

bool LookupInterfaceSlot(void* obj, void* ifaceTypeInfo, uint32_t slot,
                         void** outFn, void** outMethodInfo,
                         t_LookupVirtualInvoke fallbackFn) {
    if (!obj || !ifaceTypeInfo || !outFn || !outMethodInfo) return false;
    *outFn = nullptr;
    *outMethodInfo = nullptr;
    __try {
        uintptr_t objAddr = reinterpret_cast<uintptr_t>(obj);
        if (!IsCanonPtr(objAddr, sizeof(uintptr_t))) return false;
        uintptr_t klass = *reinterpret_cast<const uintptr_t*>(objAddr);
        if (!IsCanonPtr(klass, 0x140)) return false;

        uint16_t ifaceCount = *reinterpret_cast<const uint16_t*>(klass + 0x12E);
        uintptr_t ifaceOffsets = *reinterpret_cast<const uintptr_t*>(klass + 0xB0);
        if (ifaceCount > 0 && ifaceCount < 256 && IsCanonPtr(ifaceOffsets, static_cast<size_t>(ifaceCount) * 16)) {
            for (uint16_t i = 0; i < ifaceCount; ++i) {
                uintptr_t entryIface = *reinterpret_cast<const uintptr_t*>(ifaceOffsets + static_cast<size_t>(i) * 16);
                if (entryIface == reinterpret_cast<uintptr_t>(ifaceTypeInfo)) {
                    uint32_t startSlot = *reinterpret_cast<const uint32_t*>(ifaceOffsets + static_cast<size_t>(i) * 16 + 8);
                    uintptr_t vtableSlot = klass + 0x138 + static_cast<size_t>(startSlot + slot) * 16;
                    if (!IsCanonPtr(vtableSlot, 16)) break;
                    void* fn = *reinterpret_cast<void* const*>(vtableSlot);
                    void* mi = *reinterpret_cast<void* const*>(vtableSlot + 8);
                    if (IsCanonPtr(reinterpret_cast<uintptr_t>(fn), 8)) {
                        *outFn = fn;
                        *outMethodInfo = mi;
                        return true;
                    }
                    break;
                }
            }
        }
        if (fallbackFn) {
            void** slotPair = fallbackFn(obj, ifaceTypeInfo, slot);
            if (IsCanonPtr(reinterpret_cast<uintptr_t>(slotPair), 16)) {
                void* fn = slotPair[0];
                void* mi = slotPair[1];
                if (IsCanonPtr(reinterpret_cast<uintptr_t>(fn), 8)) {
                    *outFn = fn;
                    *outMethodInfo = mi;
                    return true;
                }
            }
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outFn = nullptr;
        *outMethodInfo = nullptr;
        return false;
    }
}

} // namespace Il2CppHelpers
