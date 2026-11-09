#include "ecs/physics_scene_impl.hpp"

namespace ohao::ecs {

auto PhysicsScene::Impl::yawOf(const glm::quat& q) -> float {
    const glm::vec3 f = q * glm::vec3(0.0f, 0.0f, -1.0f);
    if (std::fabs(f.x) < 1e-6f && std::fabs(f.z) < 1e-6f) return 0.0f;
    return glm::degrees(std::atan2(-f.x, -f.z));
}

// Up positive, from the rotation's -Z.
auto PhysicsScene::Impl::pitchOf(const glm::quat& q) -> float {
    const glm::vec3 f = q * glm::vec3(0.0f, 0.0f, -1.0f);
    return glm::degrees(std::asin(std::clamp(f.y, -1.0f, 1.0f)));
}

// An entity's world matrix from the Transforms up its chain: right after a level expands,
// before the transform system has given its nodes a WorldTransform.
auto PhysicsScene::Impl::worldOf(entt::entity e) const -> glm::mat4 {
    const auto* t = reg.try_get<Transform>(e);
    glm::mat4 m = t ? t->matrix() : glm::mat4(1.0f);
    for (const auto* p = reg.try_get<Parent>(e); p && reg.valid(p->entity); p = reg.try_get<Parent>(p->entity))
        if (const auto* pt = reg.try_get<Transform>(p->entity)) m = pt->matrix() * m;
    return m;
}

// The player's head: its first child (creation order) holding a Camera, else one named Head.
auto PhysicsScene::Impl::headOf(entt::entity e) const -> entt::entity {
    const auto* ch = reg.try_get<Children>(e);
    if (!ch) return entt::null;
    entt::entity camera = entt::null, named = entt::null;
    uint64_t cameraSeq = UINT64_MAX, namedSeq = UINT64_MAX;
    for (const entt::entity c : ch->entities) {
        if (!reg.valid(c) || !reg.all_of<Transform>(c)) continue;
        const uint64_t s = seqOf(c);
        if (reg.all_of<Camera>(c)) {
            if (s < cameraSeq) cameraSeq = s, camera = c;
        } else if (const auto* n = reg.try_get<Name>(c); n && n->value == "Head" && s < namedSeq) {
            namedSeq = s, named = c;
        }
    }
    return camera != entt::null ? camera : named;
}

void PhysicsScene::Impl::recordMove(entt::entity e, glm::vec3 fromPos, glm::quat fromRot, glm::vec3 toPos, glm::quat toRot) {
    Mover& m = movers[e];
    m.fromPos = fromPos;
    m.fromRot = fromRot;
    m.toPos = toPos;
    m.toRot = toRot;
    m.moved = true;
}

// The spawn point of a character: the k-th character with its PlayerController.spawn tag (in
// creation order) takes the k-th SpawnPoint with that tag (in creation order); null if it has
// no tag or no point is left for it.
auto PhysicsScene::Impl::spawnFor(entt::entity e) const -> entt::entity {
    const std::string& tag = reg.get<PlayerController>(e).spawn;
    if (tag.empty()) return entt::null;
    std::vector<std::pair<uint64_t, entt::entity>> points, characters;
    for (auto [p, sp] : reg.view<const SpawnPoint>().each())
        if (sp.tag == tag && reg.all_of<Transform>(p)) points.emplace_back(seqOf(p), p);
    for (auto [c, pc] : reg.view<const PlayerController>().each())
        if (pc.spawn == tag) characters.emplace_back(seqOf(c), c);
    std::sort(points.begin(), points.end());
    std::sort(characters.begin(), characters.end());
    for (std::size_t k = 0; k < characters.size(); ++k)
        if (characters[k].second == e) return k < points.size() ? points[k].second : entt::entity{entt::null};
    return entt::null;
}

// A character for a new PlayerController, at the player spawn (or where the entity is, with
// W_PLAYER_NO_SPAWN); for a changed one, where the player stands, facing as it faced; for one
// a restore put back (`restored`), as the restored entity and its head stand.
void PhysicsScene::Impl::createPlayer(entt::entity e, bool restored) {
    const auto old = players.find(e);
    const bool existed = old != players.end();
    float oldPitch = existed ? old->second.pitchDeg : 0.0f;
    // Its yaw, unless its rotation was edited too.
    const glm::quat rotNow = reg.get<Transform>(e).rotation;   // a copy: writePlayer rewrites it
    const float oldYaw = !existed ? 0.0f
                         : sameRotation(rotNow, old->second.writtenRot, 1e-4f) ? old->second.yawDeg
                                                                               : yawOf(rotNow);
    removePlayer(e, false);
    const PlayerController& pc = reg.get<PlayerController>(e);
    PlayerRecord back;
    const bool same = !existed && restored && unpark(e, pc, back);
    if (!same) dropParked(e);
    glm::vec3 pos(0.0f);
    float yaw = 0.0f;
    const entt::entity spawn = existed || restored ? entt::null : spawnFor(e);
    if (existed) {
        pos = reg.get<Transform>(e).position;
        yaw = oldYaw;
    } else if (restored) {
        pos = reg.get<Transform>(e).position;   // a character is a root entity: its world pose
        yaw = yawOf(rotNow);
        if (const entt::entity head = headOf(e); head != entt::null)
            oldPitch = std::clamp(pitchOf(reg.get<Transform>(head).rotation), -89.0f, 89.0f);
    } else if (spawn != entt::null) {
        const glm::mat4 w = worldOf(spawn);
        pos = glm::vec3(w[3]);
        yaw = yawOf(decompose(w).rotation);
    } else {
        const Placement p = decompose(worldOf(e));
        pos = p.position;
        yaw = yawOf(p.rotation);
        if (!pc.spawn.empty())
            warn(e, "W_PLAYER_NO_SPAWN",
                 "no SpawnPoint tagged \"" + pc.spawn + "\" is left for it; the character starts where it is");
    }
    pb::CharacterCreationInfo ci;
    ci.position = pos;
    ci.rotation = yawQuat(yaw);
    ci.capsuleRadius = pc.radius;
    ci.capsuleHeight = std::max(pc.height, 2.0f * pc.radius + 0.01f);
    ci.maxSlopeAngleDeg = pc.maxSlopeDeg;
    ci.mass = pc.mass;
    ci.maxStrength = pc.pushForce;
    ci.layer = pb::CollisionLayer::CHARACTER;
    ci.positionAtFeet = true;
    ci.innerBody = true;
    pb::IPhysicsBackend& be = ensureBackend();
    PlayerRecord p;
    if (same) {
        // The character it had (a restore puts the snapshot's simulation back over it), placed
        // where the restored entity stands.
        p.character = back.character;
        p.inner = back.inner;
        be.setCharacterRotation(p.character, ci.rotation);
        be.setCharacterPosition(p.character, pos);
        be.setCharacterLinearVelocity(p.character, glm::vec3(0.0f));
    } else {
        p.character = be.createCharacter(ci);
        if (p.character == pb::INVALID_CHARACTER) {
            warn(e, "W_PLAYER_NO_CHARACTER", "the physics backend could not make the player's character");
            return;
        }
        p.inner = be.getCharacterInnerBody(p.character);
    }
    if (p.inner != pb::INVALID_BODY) entityOf[p.inner] = e;
    p.yawDeg = yaw;
    p.pitchDeg = oldPitch;
    p.written = pos;
    p.writtenRot = ci.rotation;
    p.airTop = pos.y;
    standWhereItIs(p);   // the backend found its ground at creation
    movers.erase(e);     // placed, not moved: nothing to draw between
    const entt::entity headNow = restored ? headOf(e) : entt::entity{entt::null};
    const glm::quat headRotNow = headNow != entt::null ? reg.get<Transform>(headNow).rotation : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    writePlayer(e, p, pos, glm::vec3(0.0f), p.grounded, false);
    if (restored) {
        // A restore's entity keeps the rotation it was restored with, bit for bit: the yaw and
        // pitch read back from it give the same rotation but not always the same quaternion (a
        // yaw of -180 reads back as +180), and world.hash after a restart is the round start's.
        if (sameRotation(rotNow, p.writtenRot, 1e-4f) && rotNow != p.writtenRot) {
            const glm::quat keep = rotNow;
            p.writtenRot = keep;
            writePose(e, [&](Transform& t) { t.rotation = keep; });
        }
        if (headNow != entt::null && headNow == p.head && sameRotation(headRotNow, p.headRot, 1e-4f) &&
            headRotNow != p.headRot) {
            p.headRot = headRotNow;
            writePose(headNow, [&](Transform& t) { t.rotation = headRotNow; });
        }
    }
    players[e] = p;
}

// A placed player (made or teleported) on the ground the backend found where it is.
void PhysicsScene::Impl::standWhereItIs(PlayerRecord& p) {
    const pb::CharacterState st = backend->getCharacterState(p.character);
    p.grounded = st.groundState == pb::GroundState::ON_GROUND;
    p.ground = groundEntity(st);
}

auto PhysicsScene::Impl::groundEntity(const pb::CharacterState& st) const -> entt::entity {
    if (st.groundState != pb::GroundState::ON_GROUND || st.groundBody == pb::INVALID_BODY) return entt::null;
    const auto it = entityOf.find(st.groundBody);
    return it == entityOf.end() ? entt::entity{entt::null} : it->second;
}

auto PhysicsScene::Impl::teleport(entt::entity e, glm::vec3 pos, std::optional<float> yawDeg) -> bool {
    const auto it = players.find(e);
    if (it == players.end() || !backend) return false;
    PlayerRecord& p = it->second;
    if (yawDeg) p.yawDeg = std::remainder(*yawDeg, 360.0f);
    backend->setCharacterRotation(p.character, yawQuat(p.yawDeg));
    backend->setCharacterPosition(p.character, pos);   // its contacts found there
    backend->setCharacterLinearVelocity(p.character, glm::vec3(0.0f));
    p.horizontal = glm::vec3(0.0f);
    p.vy = 0.0f;
    p.stride = 0.0f;
    p.airTop = pos.y;
    p.placed = true;
    standWhereItIs(p);
    movers.erase(e);
    if (p.head != entt::null) movers.erase(p.head);
    writePlayer(e, p, pos, glm::vec3(0.0f), p.grounded, false);
    return true;
}

// The player's Transform (feet, yaw), its head and PlayerState. `track`: a move of the step,
// drawn between where it was and where it is (else a placement).
void PhysicsScene::Impl::writePlayer(entt::entity e, PlayerRecord& p, glm::vec3 pos, glm::vec3 velocity, bool onGround, bool track) {
    const glm::quat rot = yawQuat(p.yawDeg);
    const glm::vec3 fromPos = p.written;
    const glm::quat fromRot = p.writtenRot;
    if (track) recordMove(e, fromPos, fromRot, pos, rot);
    p.written = pos;
    p.writtenRot = rot;
    writePose(e, [&](Transform& t) {
        t.position = pos;
        t.rotation = rot;
    });
    writeHead(e, p, fromPos, fromRot, track);
    reg.emplace_or_replace<PlayerState>(e, PlayerState{velocity, onGround, p.yawDeg, p.pitchDeg, p.character});
    writeMotion(e, BodyMotion{velocity, glm::vec3(0.0f), false});
}

// Recoil: the view turned after the step moved the player. A turn inside a step keeps the step's
// move (drawn from where the step started to the turned pose); outside one it is a placement.
auto PhysicsScene::Impl::turnPlayer(entt::entity e, float dYaw, float dPitch) -> bool {
    const auto it = players.find(e);
    if (it == players.end() || !reg.valid(e)) return false;
    PlayerRecord& p = it->second;
    p.yawDeg = std::remainder(p.yawDeg + dYaw, 360.0f);
    p.pitchDeg = std::clamp(p.pitchDeg + dPitch, -89.0f, 89.0f);
    if (backend) backend->setCharacterRotation(p.character, yawQuat(p.yawDeg));
    const auto mv = movers.find(e);
    const bool moving = mv != movers.end() && mv->second.moved;
    std::optional<Mover> body, head;
    if (moving) {
        body = mv->second;
        if (const auto h = movers.find(p.head); p.head != entt::null && h != movers.end()) head = h->second;
    }
    const PlayerState* st = reg.try_get<PlayerState>(e);
    writePlayer(e, p, p.written, st ? st->velocity : glm::vec3(0.0f), st && st->onGround, moving);
    const auto keepFrom = [&](entt::entity who, const std::optional<Mover>& was) {
        if (!was) return;
        if (const auto m2 = movers.find(who); m2 != movers.end()) {
            m2->second.fromPos = was->fromPos;
            m2->second.fromRot = was->fromRot;
        }
    };
    keepFrom(e, body);
    keepFrom(p.head, head);
    return true;
}

// The head at eyeHeight above the feet, pitched; written only when it changes.
void PhysicsScene::Impl::writeHead(entt::entity e, PlayerRecord& p, glm::vec3 fromPos, glm::quat fromRot, bool track) {
    const entt::entity head = headOf(e);
    if (head == entt::null) {
        p.head = entt::null;
        return;
    }
    const glm::vec3 local(0.0f, reg.get<PlayerController>(e).eyeHeight, 0.0f);
    const glm::quat localRot = pitchQuat(p.pitchDeg);
    if (track && head == p.head) {
        const glm::vec3 scale = reg.get<Transform>(e).scale;
        recordMove(head, fromPos + fromRot * (scale * p.headPos), fromRot * p.headRot, p.written + p.writtenRot * (scale * local),
                   p.writtenRot * localRot);
    } else {
        movers.erase(head);
    }
    p.head = head;
    p.headPos = local;
    p.headRot = localRot;
    const Transform& ht = reg.get<Transform>(head);
    if (ht.position != local || ht.rotation != localRot)
        writePose(head, [&](Transform& t) {
            t.position = local;
            t.rotation = localRot;
        });
}

// Characters for PlayerControllers added or changed since: the `player_spawn` sync system
// (before `transform`, so a play scene's first frame is drawn from the spawn) and each step.
void PhysicsScene::Impl::spawnPlayers() {
    const bool restored = std::exchange(restoredSinceSpawn, false);
    for (auto it = parked.begin(); it != parked.end();) {   // characters of destroyed entities go
        if (reg.valid(it->first)) {
            ++it;
            continue;
        }
        if (backend) backend->destroyCharacter(it->second.record.character);
        it = parked.erase(it);
    }
    if (playersTouched.empty()) return;
    std::vector<entt::entity> list;
    list.swap(playersTouched);
    for (const entt::entity e : list)
        if (reg.valid(e) && reg.all_of<PlayerController>(e)) createPlayer(e, restored);
}

// A player's footstep or landing on the ground body `ground` (spec "Footsteps").
auto PhysicsScene::Impl::playerEvent(PhysicsEvent::Kind kind, entt::entity e, const pb::CharacterState& at, float speed) const -> PhysicsEvent {
    PhysicsEvent ev;
    ev.kind = kind;
    ev.aId = idOf(e);
    entt::entity ground = entt::null;
    if (const auto it = entityOf.find(at.groundBody); at.groundBody != pb::INVALID_BODY && it != entityOf.end()) {
        ground = it->second;
        ev.bId = idOf(ground);
    }
    const auto* c = ground != entt::null && reg.valid(ground) ? reg.try_get<Collider>(ground) : nullptr;
    ev.surface = c && !c->surface.empty() ? c->surface : std::string("default");
    ev.point = at.position;
    ev.normal = at.groundNormal;
    ev.speed = speed;
    return ev;
}

// Every player, once per step, before the bodies step (spec: the character moves first); its
// footsteps and landing go into `out`.
void PhysicsScene::Impl::updatePlayers(const InputMap& input, float dt, std::vector<PhysicsEvent>& out) {
    spawnPlayers();
    if (players.empty() || !(dt > 0.0f) || !backend) return;
    const glm::vec3 gravity = backend->getGravity();
    const glm::vec2 mouse = input.mouseDelta();
    for (auto& [e, p] : players) {
        const PlayerController& pc = reg.get<PlayerController>(e);
        const Transform& t = reg.get<Transform>(e);
        // Moved by anything but this system (an edit, a restore): it goes there, at rest (a
        // placement: the step draws from there).
        const bool posed = !posedExternally.empty() && posedExternally.contains(e);
        if (posed || differs(t.position, p.written, 1e-4f)) {
            backend->setCharacterPosition(p.character, t.position);
            backend->setCharacterLinearVelocity(p.character, glm::vec3(0.0f));
            p.horizontal = glm::vec3(0.0f);
            p.vy = 0.0f;
            p.written = t.position;
            p.airTop = t.position.y;
            p.stride = 0.0f;
            standWhereItIs(p);
        }
        // Its motion written by a restore (BodyMotion): what it carried at the snapshot.
        if (!playerMotion.empty() && playerMotion.contains(e))
            if (const auto* m = reg.try_get<BodyMotion>(e)) {
                p.horizontal = glm::vec3(m->linear.x, 0.0f, m->linear.z);
                p.vy = m->linear.y;
                backend->setCharacterLinearVelocity(p.character, m->linear);
            }
        if (posed || !sameRotation(t.rotation, p.writtenRot, 1e-4f)) {
            p.yawDeg = yawOf(t.rotation);
            p.writtenRot = yawQuat(p.yawDeg);
        }
        // A head turned by anything but this system sets the pitch.
        if (p.head != entt::null && reg.valid(p.head))
            if (const auto* ht = reg.try_get<Transform>(p.head);
                ht && ((!posedExternally.empty() && posedExternally.contains(p.head)) ||
                       !sameRotation(ht->rotation, p.headRot, 1e-4f))) {
                p.pitchDeg = std::clamp(pitchOf(ht->rotation), -89.0f, 89.0f);
                p.headRot = pitchQuat(p.pitchDeg);
            }

        // What moves it: the world's input (a player), else its MoveIntent (an AI or an agent;
        // without one it stands, facing as it faces).
        const MoveIntent* intent = pc.input ? nullptr : reg.try_get<MoveIntent>(e);
        glm::vec2 wish(0.0f);
        bool sprinting = false, jumpPressed = false;
        // A dead player (Health 0; combat, sub-project 7) neither walks, turns nor jumps: it
        // stands (or falls) where it died until the round restarts.
        const auto* health = reg.try_get<Health>(e);
        const bool dead = health && !(health->current > 0.0f);
        if (pc.input && !dead) {
            const LookResult look =
                applyLook(p.yawDeg, p.pitchDeg, mouse + glm::vec2(input.axis(pc.lookX), input.axis(pc.lookY)), pc);
            p.yawDeg = look.yawDeg;
            p.pitchDeg = look.pitchDeg;
            wish = glm::vec2(input.axis(pc.moveRight), input.axis(pc.moveForward));
            sprinting = input.held(pc.sprint);
            jumpPressed = input.pressed(pc.jump);
        } else if (intent) {
            p.yawDeg = intent->yawDeg;
            p.pitchDeg = intent->pitchDeg;
            wish = glm::clamp(intent->move, glm::vec2(-1.0f), glm::vec2(1.0f));
            sprinting = intent->sprint;
            jumpPressed = intent->jump;
        }
        p.yawDeg = std::remainder(p.yawDeg, 360.0f);
        p.pitchDeg = std::clamp(p.pitchDeg, -89.0f, 89.0f);
        const glm::quat rot = yawQuat(p.yawDeg);
        backend->setCharacterRotation(p.character, rot);

        // The wanted horizontal velocity, in the yaw frame.
        if (glm::dot(wish, wish) > 1.0f) wish = glm::normalize(wish);
        const float speed = sprinting ? pc.sprintSpeed : pc.walkSpeed;
        const glm::vec3 target = (rot * glm::vec3(0.0f, 0.0f, -1.0f) * wish.y + rot * glm::vec3(1.0f, 0.0f, 0.0f) * wish.x) * speed;

        // On the ground (and not rising from a jump): walk and maybe jump; else fall.
        const pb::CharacterState before = backend->getCharacterState(p.character);
        const bool grounded = before.groundState == pb::GroundState::ON_GROUND && p.vy < 0.1f;
        const float accel = (grounded ? 1.0f : pc.airControl) * pc.acceleration * dt;
        const glm::vec3 dv = target - p.horizontal;
        const float dvLen = glm::length(dv);
        p.horizontal = dvLen <= accel || dvLen == 0.0f ? target : p.horizontal + dv * (accel / dvLen);
        bool jumped = false;
        if (grounded) {
            p.vy = 0.0f;
            if (jumpPressed) {
                p.vy = pc.jumpSpeed;
                jumped = true;
            }
        }
        // The move takes the step's average vertical velocity (half a step of gravity), so a
        // jump follows the exact parabola: apex jumpSpeed^2 / 2g.
        const bool falling = !grounded || jumped;
        const float vMove = falling ? p.vy + 0.5f * gravity.y * dt : 0.0f;
        backend->moveCharacter(p.character, dt, glm::vec3(p.horizontal.x, vMove, p.horizontal.z), gravity,
                               pc.stepHeight, grounded && !jumped ? 0.5f : 0.0f);
        const pb::CharacterState after = backend->getCharacterState(p.character);
        // What the move left: sideways what a wall took away, vertically the step's end.
        p.horizontal = glm::vec3(after.linearVelocity.x, 0.0f, after.linearVelocity.z);
        if (falling) p.vy = after.linearVelocity.y + 0.5f * gravity.y * dt;
        const bool onGround = after.groundState == pb::GroundState::ON_GROUND && p.vy < 0.1f;
        if (onGround) p.vy = 0.0f;

        // Footsteps: one every stride walked on the ground; a land on touching it after a fall.
        if (onGround) {
            if (!p.grounded) {
                if (p.airTop - after.position.y > kLandDrop)
                    out.push_back(playerEvent(PhysicsEvent::Kind::Land, e, after, std::max(0.0f, -vMove)));
                p.stride = 0.0f;   // a new stride starts where it lands
            }
            const glm::vec3 moved = after.position - before.position;
            p.stride += glm::length(glm::vec2(moved.x, moved.z));
            const float stride = sprinting ? kSprintStride : kWalkStride;
            if (p.stride >= stride) {
                p.stride -= stride;
                out.push_back(playerEvent(PhysicsEvent::Kind::Footstep, e, after, glm::length(p.horizontal)));
                ++p.footsteps;
            }
            p.airTop = after.position.y;
        } else {
            p.airTop = std::max(p.airTop, after.position.y);
        }
        p.grounded = onGround;
        p.placed = false;
        p.ground = onGround ? groundEntity(after) : entt::entity{entt::null};
        writePlayer(e, p, after.position, glm::vec3(p.horizontal.x, p.vy, p.horizontal.z), onGround, true);
    }
    playerMotion.clear();
}

void PhysicsScene::Impl::step(const InputMap& input, float dt) {
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    const auto t0 = Clock::now();
    ++steps;
    lastSubsteps = 0;
    timing = {};
    std::vector<PhysicsEvent> fresh;
    if (bodies.empty() && touched.empty() && players.empty() && playersTouched.empty() &&
        reg.view<PhysicsMoved>().empty()) {   // a world without colliders or players: no work at all
        posedExternally.clear();
        return;
    }
    propagateTransforms(reg);
    stepDt = dt;
    sync();
    stepDt = 0.0f;
    for (auto& [e, mv] : movers) mv.moved = false;
    const auto tp = Clock::now();
    updatePlayers(input, dt, fresh);
    posedExternally.clear();   // the bodies took theirs in the sync, the players in their update
    auto t1 = Clock::now(), t2 = t1, t3 = t1;
    timing.playerMs = ms(tp, t1);
    if (backend && dt > 0.0f) {
        constexpr float kMaxDt = 1.0f / 60.0f;
        int n = static_cast<int>(std::ceil(dt / kMaxDt - 1e-4f));
        if (n > PhysicsScene::kMaxSubsteps) {
            if (!warnedLongStep)
                OHAO_LOG(Warn, physics, "physics: a step of {} s is integrated as {} updates of {} s", dt,
                         PhysicsScene::kMaxSubsteps, dt / float(PhysicsScene::kMaxSubsteps));
            warnedLongStep = true;
            n = PhysicsScene::kMaxSubsteps;
        }
        n = std::max(n, 1);
        t1 = Clock::now();
        for (int i = 0; i < n; ++i) backend->step(dt / float(n));
        lastSubsteps = n;
        // Kinematic bodies moved this step stop at their targets until their entities move again.
        for (const pb::BodyHandle h : kinematicMoved) {
            backend->setLinearVelocity(h, glm::vec3(0.0f));
            backend->setAngularVelocity(h, glm::vec3(0.0f));
        }
        kinematicMoved.clear();
        if (!jointOf.empty()) publishBrokenJoints(fresh);
        t2 = Clock::now();
        drainContacts(fresh);
        t3 = Clock::now();
        writeBack();
    }
    // What this step did not move is drawn where it is.
    for (auto it = movers.begin(); it != movers.end();) it = it->second.moved ? std::next(it) : movers.erase(it);
    const auto t4 = Clock::now();
    publish(fresh);
    const auto t5 = Clock::now();
    timing.joltMs = ms(t1, t2);
    timing.eventsMs = ms(t2, t3) + ms(t4, t5);
    timing.writeMs = ms(t3, t4);
    timing.totalMs = ms(t0, t5);
}

// RenderPose for what the last step moved, between its last two step poses at alpha, and for
// their subtrees (a descendant that moves by itself has its own entry). Alpha 1 (or outside
// [0, 1)) draws the step's poses: every RenderPose goes. The step's state is not touched.
void PhysicsScene::Impl::interpolate(float alpha, glm::vec2 pendingLook) {
    ankerl::unordered_dense::set<entt::entity> was;
    was.swap(drawn);
    const bool between = alpha >= 0.0f && alpha < 1.0f;
    if (between) {
        std::vector<entt::entity> stack;
        for (const auto& [e, mv] : movers) {
            if (!reg.valid(e) || !reg.all_of<WorldTransform>(e)) continue;
            if (mv.fromPos == mv.toPos && mv.fromRot == mv.toRot) continue;
            const glm::mat4 at = glm::translate(glm::mat4(1.0f), glm::mix(mv.fromPos, mv.toPos, alpha)) *
                                 glm::mat4_cast(glm::slerp(mv.fromRot, mv.toRot, alpha));
            const glm::mat4 to = glm::translate(glm::mat4(1.0f), mv.toPos) * glm::mat4_cast(mv.toRot);
            const glm::mat4 delta = at * glm::inverse(to);
            stack.assign(1, e);
            while (!stack.empty()) {
                const entt::entity x = stack.back();
                stack.pop_back();
                if (!reg.valid(x) || (x != e && movers.contains(x))) continue;
                if (const auto* wt = reg.try_get<WorldTransform>(x)) {
                    const auto* last = reg.try_get<RenderPose>(x);
                    const glm::mat4 prev = last && was.contains(x) ? last->world : wt->prevWorld;
                    reg.emplace_or_replace<RenderPose>(x, RenderPose{delta * wt->world, prev});
                    reg.emplace_or_replace<RenderDirty>(x);
                    drawn.insert(x);
                }
                if (const auto* ch = reg.try_get<Children>(x)) stack.insert(stack.end(), ch->entities.begin(), ch->entities.end());
            }
        }
    }
    lookLead(between, alpha, pendingLook, was);
    for (const entt::entity x : was) {
        if (drawn.contains(x) || !reg.valid(x)) continue;
        reg.remove<RenderPose>(x);
        reg.emplace_or_replace<RenderDirty>(x);
    }
}

void PhysicsScene::Impl::lookLead(bool between, float alpha, glm::vec2 pending, const ankerl::unordered_dense::set<entt::entity>& was) {
    leadsLook = false;
    const bool lead = between || pending != glm::vec2(0.0f);
    std::vector<std::pair<entt::entity, glm::mat4>> stack;
    for (auto& [e, p] : players) {
        if (!reg.valid(e)) continue;
        const auto* pc = reg.try_get<PlayerController>(e);
        const auto* t = reg.try_get<Transform>(e);
        if (!pc || !pc->input || !t) continue;
        if (const auto* h = reg.try_get<Health>(e); h && !(h->current > 0.0f)) continue;
        leadsLook = true;
        if (!lead) continue;
        const LookResult look = applyLook(p.yawDeg, p.pitchDeg, pending, *pc);
        glm::vec3 pos = t->position;
        if (between)
            if (const auto mv = movers.find(e); mv != movers.end() && mv->second.fromPos != mv->second.toPos)
                pos = glm::mix(mv->second.fromPos, mv->second.toPos, alpha);
        // As the transform system composes it (a player is a root: identity times its local).
        const glm::mat4 body = glm::mat4(1.0f) * Transform{pos, yawQuat(look.yawDeg), t->scale}.matrix();
        setDrawn(e, body, was);
        stack.clear();
        if (const auto* ch = reg.try_get<Children>(e))
            for (const entt::entity c : ch->entities) stack.emplace_back(c, body);
        while (!stack.empty()) {
            const auto [x, parent] = stack.back();
            stack.pop_back();
            const auto* xt = reg.try_get<Transform>(x);
            if (!reg.valid(x) || !xt) continue;
            const glm::mat4 local = x == p.head ? Transform{xt->position, pitchQuat(look.pitchDeg), xt->scale}.matrix()
                                                : xt->matrix();
            const glm::mat4 world = parent * local;
            setDrawn(x, world, was);
            if (const auto* ch = reg.try_get<Children>(x))
                for (const entt::entity c : ch->entities) stack.emplace_back(c, world);
        }
    }
}

// A drawn pose for x this frame; its previous one is what the last frame drew of it.
void PhysicsScene::Impl::setDrawn(entt::entity x, const glm::mat4& world, const ankerl::unordered_dense::set<entt::entity>& was) {
    const auto* wt = reg.try_get<WorldTransform>(x);
    if (!wt) return;
    const auto* last = reg.try_get<RenderPose>(x);
    const glm::mat4 prev = drawn.contains(x) && last ? last->prevWorld          // set by the movers this frame
                           : last && was.contains(x) ? last->world                // drawn last frame
                                                     : wt->prevWorld;
    reg.emplace_or_replace<RenderPose>(x, RenderPose{world, prev});
    reg.emplace_or_replace<RenderDirty>(x);
    drawn.insert(x);
}

} // namespace ohao::ecs
