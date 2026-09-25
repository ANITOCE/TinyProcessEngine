#pragma once

#ifdef _WIN32

#include <Windows.h>

namespace tpe::platform {

/**
 * RAII wrapper for Windows HANDLE.
 * Automatically calls CloseHandle on destruction.
 * Move-only semantics — copying is disabled.
 */
class ScopedHandle {
public:
    // Default: invalid handle
    ScopedHandle() noexcept : m_handle(INVALID_HANDLE_VALUE) {}

    // Take ownership of an existing HANDLE
    explicit ScopedHandle(HANDLE h) noexcept : m_handle(h) {}

    // Move constructor
    ScopedHandle(ScopedHandle&& other) noexcept : m_handle(other.m_handle) {
        other.m_handle = INVALID_HANDLE_VALUE;
    }

    // Move assignment
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            close();
            m_handle = other.m_handle;
            other.m_handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    // No copying
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    // Destroy: auto-close
    ~ScopedHandle() { close(); }

    // Access the raw HANDLE (for Win32 API calls)
    HANDLE get() const noexcept { return m_handle; }

    // Check if the handle is valid
    bool valid() const noexcept {
        return m_handle != INVALID_HANDLE_VALUE && m_handle != NULL;
    }
    explicit operator bool() const noexcept { return valid(); }

    // Early-close
    void close() noexcept {
        if (valid()) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE m_handle;
};

} // namespace tpe::platform

#endif // _WIN32
