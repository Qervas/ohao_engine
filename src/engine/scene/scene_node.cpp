#include "engine/scene/scene_node.hpp"
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
        // Find child in old parent's children
        auto& oldParentChildren = oldParent->children;
        auto it = std::find_if(oldParentChildren.begin(), oldParentChildren.end(),
            [child](const Ptr& ptr) { return ptr.get() == child.get(); });

        if (it != oldParentChildren.end()) {
            oldParentChildren.erase(it);
        }
    }

    // Add to children
    children.push_back(child);
    child->parent = weak_from_this();
    child->onAddedToScene();

    // Update transform relationships
    child->getTransform().setOwner(child.get());
    child->markTransformDirty();

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
    auto parentPtr = parent.lock();
    if (!parentPtr) {
        // If no valid parent, just clear the weak pointer
        parent.reset();
        return;
    }

    // Find and remove this node from parent's children
    auto& parentChildren = parentPtr->children;
    auto it = std::find_if(parentChildren.begin(), parentChildren.end(),
        [this](const Ptr& child) { return child.get() == this; });

    if (it != parentChildren.end()) {
        parentChildren.erase(it);
    }

    // Clear parent pointer
    parent.reset();
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

void SceneNode::setTransform(const Transform& newTransform) {
    transform = newTransform;
    transform.setOwner(this);  // Ensure owner is set
    markTransformDirty();
}

void SceneNode::markTransformDirty() {
    transformDirty = true;
    if (auto p = getParent()) {
        p->markTransformDirty();
    }
}

void SceneNode::onAddedToScene() {
    // Override in derived classes
}

void SceneNode::onRemovedFromScene() {
    // Override in derived classes
}

glm::mat4 SceneNode::getWorldTransform() const {
    return transform.getWorldMatrix();
}

} // namespace ohao
