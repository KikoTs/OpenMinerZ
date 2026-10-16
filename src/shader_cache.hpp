#pragma once

// Compiled HLSL, kept for the life of the process.
//
// Entering a world builds a new WorldPreviewRenderer and with it every
// pipeline, so D3DCompile ran again over the same sources each time: about
// three seconds on the frame thread, one frozen frame behind a loading bar
// that could not move (2.8 to 4.5 s measured over twelve entries on a
// 13900K). Bytecode depends only on the text, the entry point, the target and
// the flags, never on the device, so the second and later builds take it from
// here. Failed compilations are not kept: they are reported, and retried, as
// before.
#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace openminerz
{

// Same contract as D3DCompile. A call with macros or an include handler is
// passed straight through (neither is part of the key; no caller uses them).
[[nodiscard]] inline HRESULT compile_hlsl_cached(const void *source, std::size_t length,
                                                 const char *name,
                                                 const D3D_SHADER_MACRO *defines,
                                                 ID3DInclude *include, const char *entry,
                                                 const char *target, UINT flags, UINT effect_flags,
                                                 ID3DBlob **code, ID3DBlob **errors)
{
    if (source == nullptr || entry == nullptr || target == nullptr || code == nullptr ||
        defines != nullptr || include != nullptr)
        return D3DCompile(source, length, name, defines, include, entry, target, flags,
                          effect_flags, code, errors);
    static std::mutex mutex;
    static std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3DBlob>> cache;
    std::string key;
    key.reserve(length + 64U);
    key.append(entry).push_back('\0');
    key.append(target).push_back('\0');
    key.append(std::to_string(flags)).push_back(',');
    key.append(std::to_string(effect_flags)).push_back('\0');
    key.append(static_cast<const char *>(source), length);
    {
        const std::lock_guard lock{mutex};
        if (const auto found = cache.find(key); found != cache.end())
        {
            if (errors != nullptr)
                *errors = nullptr;
            *code = found->second.Get();
            (*code)->AddRef();
            return S_OK;
        }
    }
    const HRESULT result = D3DCompile(source, length, name, nullptr, nullptr, entry, target, flags,
                                      effect_flags, code, errors);
    if (SUCCEEDED(result) && *code != nullptr)
    {
        const std::lock_guard lock{mutex};
        cache.emplace(std::move(key), *code);
    }
    return result;
}

} // namespace openminerz

#endif
