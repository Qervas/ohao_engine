#include "animation_controller.hpp"
#include "animation_clip.hpp"
#include "skeleton.hpp"
#include <iostream>

namespace ohao {

void AnimationController::addState(const std::string& name, std::shared_ptr<AnimationClip> clip,
                                    bool looping, float speed) {
    AnimationState state;
    state.name = name;
    state.clip = std::move(clip);
    state.looping = looping;
    state.speed = speed;
    m_states[name] = std::move(state);
}

void AnimationController::addTransition(const std::string& from, const std::string& to,
                                         float blendDuration) {
    AnimationTransition transition;
    transition.fromState = from;
    transition.toState = to;
    transition.blendDuration = blendDuration;
    m_transitions[from][to] = transition;
}

void AnimationController::setDefaultState(const std::string& name) {
    if (m_states.find(name) != m_states.end()) {
        m_currentStateName = name;
        m_currentTime = 0.0f;
    }
}

void AnimationController::play(const std::string& stateName) {
    if (stateName == m_currentStateName && !m_blending) {
        return; // Already playing
    }

    if (m_states.find(stateName) == m_states.end()) {
        std::cerr << "AnimationController: State '" << stateName << "' not found" << std::endl;
        return;
    }

    // Check if there's a transition defined
    float blendDuration = 0.2f; // default
    auto transIt = m_transitions.find(m_currentStateName);
    if (transIt != m_transitions.end()) {
        auto targetIt = transIt->second.find(stateName);
        if (targetIt != transIt->second.end()) {
            blendDuration = targetIt->second.blendDuration;
        }
    }

    m_nextStateName = stateName;
    m_nextTime = 0.0f;
    m_blending = true;
    m_blendFactor = 0.0f;
    m_blendDuration = blendDuration;
}

void AnimationController::update(float deltaTime, Skeleton& skeleton) {
    if (m_currentStateName.empty()) return;

    auto currentIt = m_states.find(m_currentStateName);
    if (currentIt == m_states.end() || !currentIt->second.clip) return;

    const auto& currentState = currentIt->second;

    // Advance current animation time
    m_currentTime += deltaTime * currentState.speed;
    if (currentState.looping && currentState.clip->duration > 0.0f) {
        m_currentTime = std::fmod(m_currentTime, currentState.clip->duration);
    }

    if (m_blending && !m_nextStateName.empty()) {
        auto nextIt = m_states.find(m_nextStateName);
        if (nextIt != m_states.end() && nextIt->second.clip) {
            const auto& nextState = nextIt->second;
            m_nextTime += deltaTime * nextState.speed;

            m_blendFactor += deltaTime / m_blendDuration;
            if (m_blendFactor >= 1.0f) {
                // Transition complete
                m_blendFactor = 1.0f;
                m_currentStateName = m_nextStateName;
                m_currentTime = m_nextTime;
                m_nextStateName.clear();
                m_blending = false;

                // Sample only the new state
                nextState.clip->sample(m_currentTime, skeleton);
            } else {
                // Sample both and blend via skeleton
                // First, save joint transforms from current animation
                Skeleton tempSkeleton = skeleton;
                currentState.clip->sample(m_currentTime, skeleton);

                // Save current result
                std::vector<glm::mat4> currentTransforms;
                currentTransforms.reserve(skeleton.joints.size());
                for (const auto& joint : skeleton.joints) {
                    currentTransforms.push_back(joint.localTransform);
                }

                // Sample next animation
                skeleton = tempSkeleton;
                nextState.clip->sample(m_nextTime, skeleton);

                // Blend joint local transforms
                for (size_t i = 0; i < skeleton.joints.size(); ++i) {
                    // Simple matrix lerp (good enough for small blend windows)
                    skeleton.joints[i].localTransform =
                        currentTransforms[i] * (1.0f - m_blendFactor) +
                        skeleton.joints[i].localTransform * m_blendFactor;
                }
            }
        }
    } else {
        // No blending - just sample current
        currentState.clip->sample(m_currentTime, skeleton);
    }

    // Compute final joint matrices for GPU
    skeleton.computeJointMatrices();
}

} // namespace ohao
