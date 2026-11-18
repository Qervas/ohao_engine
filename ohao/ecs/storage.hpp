#pragma once
// A component's EnTT storage is compiled once, in the module that owns the component, not in every unit that names it.
// Each module's components header declares its components' storages external (OHAO_EXTERN_STORAGE, at the global
// scope, after the types), and the module's storage.cpp instantiates them (OHAO_STORAGE_INSTANCE): every other unit
// then inlines what it inlines and calls the rest, and no unit emits a storage's virtual functions again. A component
// without the declaration still works: its storage is instantiated where it is used, as EnTT does by default.
#include <entt/entity/registry.hpp>

#define OHAO_EXTERN_STORAGE(T) extern template class entt::basic_storage<T>
#define OHAO_STORAGE_INSTANCE(T) template class entt::basic_storage<T>

// The registry itself and its entity storage, instantiated once by the core (ecs/storage.cpp).
extern template class entt::basic_sparse_set<entt::entity>;
extern template class entt::basic_storage<entt::entity>;
extern template class entt::basic_sigh_mixin<entt::basic_storage<entt::entity>, entt::basic_registry<entt::entity>>;
extern template class entt::basic_registry<entt::entity>;
