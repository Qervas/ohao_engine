#pragma once

#include "scene/component/component.hpp"
#include "skeleton.hpp"
#include "animation_controller.hpp"
#include <memory>
#include <vector>
#include <glm/glm.hpp>

namespace ohao {

struct AnimationClip;

class AnimationComponent : public Component {
public:
    using Ptr = std::shared_ptr<AnimationComponent>;

    AnimationComponent() = default;
    ~AnimationComponent() override = default;

    void initialize() override;
    void update(float deltaTime) override;

    const char* getTypeName() const override { return "AnimationComponent"; }

    // Skeleton
    void setSkeleton(std::shared_ptr<Skeleton> skel) { m_skeleton = std::move(skel); }
    Skeleton* getSkeleton() { return m_skeleton.get(); }
    const Skeleton* getSkeleton() const { return m_skeleton.get(); }

    // Animation clips
    void addAnimation(const std::string& name, std::shared_ptr<AnimationClip> clip,
                      bool looping = true, float speed = 1.0f);

    // Playback
    void play(const std::string& animationName);
    void stop();

    // State query
    bool isPlaying() const { return m_playing; }
    bool hasAnimations() const { return m_skeleton != nullptr; }

    // Get joint matrices for GPU upload (after update)
    const std::vector<glm::mat4>& getJointMatrices() const;

    // Controller access for advanced state machine setup
    AnimationController& getController() { return m_controller; }

private:
    std::shared_ptr<Skeleton> m_skeleton;
    AnimationController m_controller;
    bool m_playing{false};
    float m_animTime{0.0f};

    // Empty matrix vector returned when no skeleton exists
    static const std::vector<glm::mat4> s_emptyMatrices;
};

} // namespace ohao
