#pragma once
#include <windows.h>

// ポリシーベースの汎用 RAII リソースラッパー。
// Traits は type, invalid(), close(type) を定義する。
template <typename Traits>
class UniqueResource {
    using handle_t = typename Traits::type;

public:
    UniqueResource() noexcept = default;
    explicit UniqueResource(handle_t h) noexcept : handle_(h)
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
        return handle_ != Traits::invalid();
    }
    handle_t get() const noexcept
    {
        return handle_;
    }

    handle_t release() noexcept
    {
        handle_t h = handle_;
        handle_ = Traits::invalid();
        return h;
    }

    void reset(handle_t h = Traits::invalid()) noexcept
    {
        if (handle_ != Traits::invalid()) {
            Traits::close(handle_);
        }
        handle_ = h;
    }

private:
    handle_t handle_ = Traits::invalid();
};

// CloseHandle で解放、無効値 INVALID_HANDLE_VALUE（CreateFileW 等）
struct HandleTraits {
    using type = HANDLE;
    static type invalid() noexcept
    {
        return INVALID_HANDLE_VALUE;
    }
    static void close(type h) noexcept
    {
        CloseHandle(h);
    }
};
using UniqueHandle = UniqueResource<HandleTraits>;

// CloseHandle で解放、無効値 nullptr（CreateEventW 等）
struct EventHandleTraits {
    using type = HANDLE;
    static type invalid() noexcept
    {
        return nullptr;
    }
    static void close(type h) noexcept
    {
        CloseHandle(h);
    }
};
using UniqueEventHandle = UniqueResource<EventHandleTraits>;

// FindClose で解放（FindFirstFileW）
struct FindHandleTraits {
    using type = HANDLE;
    static type invalid() noexcept
    {
        return INVALID_HANDLE_VALUE;
    }
    static void close(type h) noexcept
    {
        FindClose(h);
    }
};
using UniqueFindHandle = UniqueResource<FindHandleTraits>;

// GlobalFree で解放（GlobalAlloc）。
// SetClipboardData / CreateStreamOnHGlobal への所有権移譲時は release() を使う。
struct GlobalMemTraits {
    using type = HGLOBAL;
    static type invalid() noexcept
    {
        return nullptr;
    }
    static void close(type h) noexcept
    {
        GlobalFree(h);
    }
};
using UniqueGlobalMem = UniqueResource<GlobalMemTraits>;

// クリップボード書き込み API は clipboard_util.h に分離した。
