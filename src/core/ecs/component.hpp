#pragma once
#include "ecs_types.hpp"

namespace ohao {

class Component {
public:
    virtual ~Component() = default;
    virtual void onAttach() {}
    virtual void onDetach() {}
    virtual void onUpdate(float dt) {}

    Entity* getOwner() const { return owner; }
    void setOwner(Entity* entity) { owner = entity; }

protected:
    Entity* owner{nullptr};
};

} // namespace ohao
