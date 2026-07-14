#pragma once

/**
 * Lightweight Result<T, E> — C++20 stand-in for std::expected (C++23).
 *
 *   Result<int> r = Result<int>::ok(42);
 *   if (!r) { log(r.error()); return; }
 *   use(r.value());
 */

#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ohao {

template<typename T, typename E = std::string>
class Result {
public:
    using value_type = T;
    using error_type = E;

    [[nodiscard]] static Result ok(T value) {
        Result r;
        r.m_storage.template emplace<T>(std::move(value));
        return r;
    }

    [[nodiscard]] static Result err(E error) {
        Result r;
        r.m_storage.template emplace<E>(std::move(error));
        return r;
    }

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<T>(m_storage);
    }

    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& value() & { return std::get<T>(m_storage); }
    [[nodiscard]] const T& value() const& { return std::get<T>(m_storage); }
    [[nodiscard]] T&& value() && { return std::get<T>(std::move(m_storage)); }

    [[nodiscard]] E& error() & { return std::get<E>(m_storage); }
    [[nodiscard]] const E& error() const& { return std::get<E>(m_storage); }

    [[nodiscard]] T value_or(T fallback) const& {
        return has_value() ? std::get<T>(m_storage) : std::move(fallback);
    }

private:
    Result() : m_storage(std::in_place_type<E>, E{}) {}
    std::variant<T, E> m_storage;
};

template<typename E = std::string>
class VoidResult {
public:
    [[nodiscard]] static VoidResult ok() {
        VoidResult r;
        r.m_ok = true;
        return r;
    }

    [[nodiscard]] static VoidResult err(E error) {
        VoidResult r;
        r.m_ok = false;
        r.m_error = std::move(error);
        return r;
    }

    [[nodiscard]] bool has_value() const noexcept { return m_ok; }
    [[nodiscard]] explicit operator bool() const noexcept { return m_ok; }

    [[nodiscard]] const E& error() const& { return m_error; }
    [[nodiscard]] E& error() & { return m_error; }

private:
    VoidResult() = default;
    bool m_ok{false};
    E m_error{};
};

template<typename T>
[[nodiscard]] Result<T> err_string(std::string_view msg) {
    return Result<T>::err(std::string(msg));
}

[[nodiscard]] inline VoidResult<> err_void(std::string_view msg) {
    return VoidResult<>::err(std::string(msg));
}

} // namespace ohao
