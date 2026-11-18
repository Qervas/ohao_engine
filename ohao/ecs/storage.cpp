// The ecs module's component storages, instantiated once (ecs/storage.hpp: every other unit calls these).
#include "ecs/components.hpp"

template class entt::basic_sparse_set<entt::entity>;
template class entt::basic_storage<entt::entity>;
template class entt::basic_sigh_mixin<entt::basic_storage<entt::entity>, entt::basic_registry<entt::entity>>;
template class entt::basic_registry<entt::entity>;

OHAO_STORAGE_INSTANCE(ohao::ecs::Children);
OHAO_STORAGE_INSTANCE(ohao::ecs::CreationOrder);
OHAO_STORAGE_INSTANCE(ohao::ecs::EntityId);
OHAO_STORAGE_INSTANCE(ohao::ecs::FlyController);
OHAO_STORAGE_INSTANCE(ohao::ecs::InputActions);
OHAO_STORAGE_INSTANCE(ohao::ecs::MovedLastFrame);
OHAO_STORAGE_INSTANCE(ohao::ecs::Name);
OHAO_STORAGE_INSTANCE(ohao::ecs::Parent);
OHAO_STORAGE_INSTANCE(ohao::ecs::PreservedComponents);
OHAO_STORAGE_INSTANCE(ohao::ecs::RenderDirty);
OHAO_STORAGE_INSTANCE(ohao::ecs::RenderPose);
OHAO_STORAGE_INSTANCE(ohao::ecs::SpawnPoint);
OHAO_STORAGE_INSTANCE(ohao::ecs::Spin);
OHAO_STORAGE_INSTANCE(ohao::ecs::Transform);
OHAO_STORAGE_INSTANCE(ohao::ecs::TransformDirty);
OHAO_STORAGE_INSTANCE(ohao::ecs::WorldTransform);
