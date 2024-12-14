#include "core/scene/scene_node.hpp"
#include <queue>
#include <algorithm>

namespace ohao {

SceneNode::SceneNode(const std::string& name) : name(name) {
    transform.setOwner(this);
}

void SceneNode::addChild(Ptr child) {
    if (!child || child.get() == this) return;

    // Remove from old parent if exists
    if (auto oldParent = child->getParent()) {
        oldParent->removeChild(child);
    }

    // Add to children
    children.push_back(child);
    child->setParent(shared_from_this());
    child->onAddedToScene();
}

void SceneNode::removeChild(Ptr child) {
    if (!child) return;

    auto it = std::find(children.begin(), children.end(), child);
    if (it != children.end()) {
        (*it)->onRemovedFromScene();
        (*it)->parent.reset();
        children.erase(it);
    }
}

void SceneNode::setParent(Ptr newParent) {
    parent = newParent;
}

void SceneNode::detachFromParent() {
    if (auto p = parent.lock()) {
        p->removeChild(shared_from_this());
    }
}

SceneNode* SceneNode::findChild(const std::string& searchName) {
    if (name == searchName) return this;

    for (const auto& child : children) {
        if (SceneNode* result = child->findChild(searchName)) {
            return result;
        }
    }

    return nullptr;
}

std::vector<SceneNode*> SceneNode::findChildren(const std::string& searchName) {
    std::vector<SceneNode*> results;

    std::queue<SceneNode*> queue;
    queue.push(this);

    while (!queue.empty()) {
        SceneNode* current = queue.front();
        queue.pop();

        if (current->getName() == searchName) {
            results.push_back(current);
        }

        for (const auto& child : current->getChildren()) {
            queue.push(child.get());
        }
    }

    return results;
}

bool SceneNode::isAncestorOf(const SceneNode* node) const {
    if (!node) return false;

    const SceneNode* parent = node->getParent();
    while (parent) {
        if (parent == this) return true;
        parent = parent->getParent();
    }

    return false;
}

bool SceneNode::isDescendantOf(const SceneNode* node) const {
    return node && node->isAncestorOf(this);
}

void SceneNode::update(float deltaTime) {
    if (!enabled) return;

    // Update children
    for (auto& child : children) {
        child->update(deltaTime);
    }
}

void SceneNode::onAddedToScene() {
    // Override in derived classes
}

void SceneNode::onRemovedFromScene() {
    // Override in derived classes
}

} // namespace ohao
