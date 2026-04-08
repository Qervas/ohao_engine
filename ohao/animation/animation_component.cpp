#include "animation_component.hpp"
#include "animation_clip.hpp"

namespace ohao {

const std::vector<glm::mat4> AnimationComponent::s_emptyMatrices;

void AnimationComponent::initialize() {
    if (m_skeleton) {
        m_skeleton->computeJointMatrices();
    }
}

void AnimationComponent::update(float deltaTime) {
    if (!m_playing || !m_skeleton) return;
    m_controller.update(deltaTime, *m_skeleton);
}

void AnimationComponent::addAnimation(const std::string& name, std::shared_ptr<AnimationClip> clip,
                                       bool looping, float speed) {
    m_controller.addState(name, std::move(clip), looping, speed);
}

void AnimationComponent::play(const std::string& animationName) {
    m_controller.play(animationName);
    m_playing = true;
}

void AnimationComponent::stop() {
    m_playing = false;
}

const std::vector<glm::mat4>& AnimationComponent::getJointMatrices() const {
    if (m_skeleton) {
        return m_skeleton->jointMatrices;
    }
    return s_emptyMatrices;
}

} // namespace ohao
