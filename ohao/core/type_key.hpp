#pragma once
// A C++ type's identity at run time, without RTTI: the address of a variable each type has one of. The reflection
// registry finds a type's TypeInfo by it, and the World's event log names a channel's payload type by it, so ECS
// systems hold a type's identity without including reflect/.

namespace ohao {

using TypeKey = const void*;

template <class T>
TypeKey typeKey() noexcept {
    static const char key = 0;
    return &key;
}

} // namespace ohao
