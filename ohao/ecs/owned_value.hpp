#pragma once
#include "reflect/type_info.hpp"

#include <new>

namespace ohao::ecs {

// One value of a reflected type in its own heap block: decoded before it is moved into
// the registry. The address is stable, so pending entity references may point into it.
class OwnedValue {
public:
    explicit OwnedValue(const reflect::TypeInfo* t)
        : m_type(t), m_ptr(::operator new(t->size, std::align_val_t(t->align))) {
        t->construct(m_ptr);
    }
    // A copy of an existing value of the same type.
    OwnedValue(const reflect::TypeInfo* t, const void* copyFrom) : OwnedValue(t) { t->copy(m_ptr, copyFrom); }
    OwnedValue(OwnedValue&& o) noexcept : m_type(o.m_type), m_ptr(o.m_ptr) { o.m_ptr = nullptr; }
    OwnedValue& operator=(OwnedValue&&) = delete;
    OwnedValue(const OwnedValue&) = delete;
    ~OwnedValue() {
        if (!m_ptr) return;
        m_type->destroy(m_ptr);
        ::operator delete(m_ptr, std::align_val_t(m_type->align));
    }
    [[nodiscard]] const reflect::TypeInfo* type() const noexcept { return m_type; }
    [[nodiscard]] void* get() const noexcept { return m_ptr; }

private:
    const reflect::TypeInfo* m_type;
    void* m_ptr;
};

} // namespace ohao::ecs
