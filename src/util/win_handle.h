#pragma once
#include <windows.h>

// 無効値は関数で渡す (INVALID_HANDLE_VALUE は定数式でない)。
template <typename T, T (*Invalid)() noexcept, auto Close>
class UniqueResource {
public:
    UniqueResource() noexcept = default;
    explicit UniqueResource(T h) noexcept : handle_(h)
    {}
    ~UniqueResource()
    {
        reset();
    }

    UniqueResource(const UniqueResource&) = delete;
    UniqueResource& operator=(const UniqueResource&) = delete;
    UniqueResource(UniqueResource&& other) noexcept : handle_(other.release())
    {}
    UniqueResource& operator=(UniqueResource&& other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    explicit operator bool() const noexcept
    {
        return handle_ != Invalid();
    }
    T get() const noexcept
    {
        return handle_;
    }

    T release() noexcept
    {
        T h = handle_;
        handle_ = Invalid();
        return h;
    }

    // 出力引数で受け取る API 用。
    T* put() noexcept
    {
        reset();
        return &handle_;
    }

    void reset(T h = Invalid()) noexcept
    {
        if (handle_ != Invalid()) {
            Close(handle_);
        }
        handle_ = h;
    }

private:
    T handle_ = Invalid();
};

inline HANDLE InvalidHandleValue() noexcept
{
    return INVALID_HANDLE_VALUE;
}

template <typename T>
constexpr T NullHandle() noexcept
{
    return nullptr;
}

using UniqueHandle = UniqueResource<HANDLE, InvalidHandleValue, CloseHandle>;
using UniqueEventHandle = UniqueResource<HANDLE, NullHandle<HANDLE>, CloseHandle>;
using UniqueFindHandle = UniqueResource<HANDLE, InvalidHandleValue, FindClose>;
using UniqueCoTaskMemStr = UniqueResource<wchar_t*, NullHandle<wchar_t*>, CoTaskMemFree>;
// SetClipboardData / CreateStreamOnHGlobal への所有権移譲時は release() を使う。
using UniqueGlobalMem = UniqueResource<HGLOBAL, NullHandle<HGLOBAL>, GlobalFree>;
