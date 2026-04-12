#pragma once

#include <string>
#include <unordered_map>
#include <memory>

namespace ohao {

struct Skeleton;
struct AnimationClip;

struct AnimationState {
    std::string name;
    std::shared_ptr<AnimationClip> clip;
    bool looping{true};
    float speed{1.0f};
};

struct AnimationTransition {
    std::string fromState;
    std::string toState;
    float blendDuration{0.2f}; // seconds to crossfade
};

class AnimationController {
public:
    // State management
    void addState(const std::string& name, std::shared_ptr<AnimationClip> clip,
                  bool looping = true, float speed = 1.0f);
    void addTransition(const std::string& from, const std::string& to, float blendDuration = 0.2f);
    void setDefaultState(const std::string& name);

    // Playback control
    void play(const std::string& stateName);
    void update(float deltaTime, Skeleton& skeleton);

    // Query
    const std::string& getCurrentState() const { return m_currentStateName; }
    float getCurrentTime() const { return m_currentTime; }
    bool isTransitioning() const { return m_blending; }

private:
    std::unordered_map<std::string, AnimationState> m_states;
    std::unordered_map<std::string, std::unordered_map<std::string, AnimationTransition>> m_transitions;

    std::string m_currentStateName;
    std::string m_nextStateName;
    float m_currentTime{0.0f};
    float m_nextTime{0.0f};

    // Blending
    bool m_blending{false};
    float m_blendFactor{0.0f};
    float m_blendDuration{0.2f};
};

} // namespace ohao
