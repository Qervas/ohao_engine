// The navigation module's registrations (navigation/module.hpp): its enum and components.
#include "navigation/components.hpp"
#include "navigation/module.hpp"

#include "ecs/scene_types.hpp"

namespace ohao::ecs {

void registerNavigationTypes(reflect::TypeRegistry& r) {
    r.enumeration<NavPathStatus>("NavPathStatus")
        .doc("Where an agent's path stands.")
        .value("None", NavPathStatus::None, "Not planned yet")
        .value("Found", NavPathStatus::Found, "A path to the goal")
        .value("Partial", NavPathStatus::Partial, "The goal is not reachable: a path to the reachable point nearest it")
        .value("NoPath", NavPathStatus::NoPath, "The agent or the goal is off the navmesh")
        .value("Arrived", NavPathStatus::Arrived, "Within its arrive distance of the goal");
    r.type<NavAgent>("NavAgent").component().host(makeComponentOps<NavAgent>())
        .doc("A character (a PlayerController with input false) the navigation system walks to its goal: a path on the "
             "navmesh, its corners followed, its velocity steered around the other agents, written as its MoveIntent "
             "each step. Inactive: the system leaves it alone.")
        .field("goal", &NavAgent::goal).doc("Where it walks, world (the nearest navmesh point to it)").unit("m")
        .field("active", &NavAgent::active).doc("Walk to the goal; false: the system leaves it and its MoveIntent alone")
        .field("speed", &NavAgent::speed).doc("Its forward move, of its walk speed").unitless().min(0.0).max(1.0)
        .field("arrive", &NavAgent::arrive).doc("Within this of the goal (horizontally) it stands").unit("m").min(0.05).max(10.0)
        .field("replanSeconds", &NavAgent::replanSeconds)
            .doc("A path is planned again this often (staggered by entity id), and whenever the navmesh changes")
            .unit("s").min(0.05).max(60.0)
        .field("avoid", &NavAgent::avoid).doc("Steer around the other agents (sampled velocities)");
    r.type<NavPath>("NavPath").component().host(makeComponentOps<NavPath>())
        .doc("The path an agent follows, as the navigation system planned it: world-space corners (never navmesh "
             "polygons), the corner it walks to, the step and navmesh version it was planned on. Written by the "
             "navigation system.")
        .field("corners", &NavPath::corners).doc("World corners from where it stood to the goal").unit("m").maxItems(32)
        .field("next", &NavPath::next).doc("The corner it walks to").unitless()
        .field("status", &NavPath::status).doc("Where the path stands")
        .field("plannedStep", &NavPath::plannedStep).doc("The World's step it was planned in")
        .field("version", &NavPath::version).doc("The navmesh version it was planned on (nav.info's version)")
        .field("goal", &NavPath::goal).doc("The goal it was planned to").unit("m");
    r.type<NavObstacle>("NavObstacle").component().host(makeComponentOps<NavObstacle>())
        .doc("While active, the entity blocks the navmesh as a box turned about +Y (a door, a breakable wall); its own "
             "collider is not baked into the navmesh. Applied in the step that changes it. A level node takes it as a "
             "hint: \"NavObstacle\": {} in its extras.")
        .field("halfExtents", &NavObstacle::halfExtents)
            .doc("Half sizes in the entity's frame (its scale applies); zero: the bounds of its mesh").unit("m").min(0.0)
        .field("active", &NavObstacle::active).doc("Blocks the navmesh");
    r.type<NavLink>("NavLink").component().host(makeComponentOps<NavLink>())
        .doc("A link the navmesh cannot walk, from the entity's position to its end: a drop, a vault. Baked into the "
             "navmesh. A level node takes it as a hint: \"NavLink\": {\"end\": [x, y, z]} in its extras.")
        .field("end", &NavLink::end).doc("The far end, in the entity's frame").unit("m")
        .field("bidirectional", &NavLink::bidirectional).doc("Both ways; false: from the entity to the end only")
        .field("radius", &NavLink::radius).doc("How near its ends a path may join it").unit("m").min(0.05).max(5.0);
}

} // namespace ohao::ecs
