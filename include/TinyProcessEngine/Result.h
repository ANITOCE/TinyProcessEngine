#ifndef _RESULT_H_
#define _RESULT_H_

#include <string>
#include <string_view>
#include <variant>
#include <cassert>
#include <cstdint>

namespace tpe {

// ============================================================
// PlatformError — 平台操作错误信息
// ============================================================
struct PlatformError {
    std::string  operation;   // 失败的操作名称，如 "ReadProcessMemory"
    unsigned long pid;        // 目标进程 ID（跨平台兼容 DWORD/unsigned int）
    int          native_code; // Windows: GetLastError(), Linux: errno
    std::string  message;     // 人类可读描述

    // Factory — implementation in Platform.cpp (platform-specific native code retrieval)
    static PlatformError from_last_error(std::string op, unsigned long pid_val);
    /// 重载(缺陷⑦;FR-020/C-P4):基础消息后追加 "; <advice>"(advice 非空时)。
    static PlatformError from_last_error(std::string op, unsigned long pid_val,
                                         std::string_view advice);
};

// ============================================================
// Result<T, E> — 成功或失败的结果类型
// ============================================================
template <typename T, typename E>
class Result {
public:
    // 成功工厂
    static Result success(T value) {
        Result r;
        r.m_storage = std::move(value);
        return r;
    }

    // 错误工厂
    static Result error(E err) {
        Result r;
        r.m_storage = std::move(err);
        return r;
    }

    bool has_value() const noexcept { return std::holds_alternative<T>(m_storage); }
    explicit operator bool() const noexcept { return has_value(); }

    T& value() {
        assert(has_value() && "Result::value() called on error state");
        return std::get<T>(m_storage);
    }
    const T& value() const {
        assert(has_value() && "Result::value() called on error state");
        return std::get<T>(m_storage);
    }

    E& error() {
        assert(!has_value() && "Result::error() called on success state");
        return std::get<E>(m_storage);
    }
    const E& error() const {
        assert(!has_value() && "Result::error() called on success state");
        return std::get<E>(m_storage);
    }

private:
    std::variant<T, E> m_storage;
};

// ============================================================
// Result<void, E> 偏特化 — 无返回值操作 (write 等)
// ============================================================
template <typename E>
class Result<void, E> {
public:
    static Result success() {
        Result r;
        r.m_has_value = true;
        return r;
    }

    static Result error(E err) {
        Result r;
        r.m_has_value = false;
        r.m_error = std::move(err);
        return r;
    }

    bool has_value() const noexcept { return m_has_value; }
    explicit operator bool() const noexcept { return has_value(); }

    E& error() {
        assert(!has_value() && "Result::error() called on success state");
        return m_error;
    }
    const E& error() const {
        assert(!has_value() && "Result::error() called on success state");
        return m_error;
    }

private:
    bool m_has_value = false;
    E m_error;
};

} // namespace tpe

#endif // _RESULT_H_
