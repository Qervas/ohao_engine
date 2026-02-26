class_name OhaoPhysicsHelpers
## Factory methods for common physics body configurations.
##
## Usage:
##   var body = OhaoPhysicsHelpers.player_body()
##   add_child(body)


## Kinematic capsule for player controller.
static func player_body(mass: float = 80.0) -> OhaoPhysicsBody:
	var body := OhaoPhysicsBody.new()
	body.set_body_type(OhaoConst.BODY_KINEMATIC)
	body.set_shape_type(OhaoConst.SHAPE_CAPSULE)
	body.set_mass(mass)
	body.set_friction(0.3)
	body.set_restitution(0.0)
	return body


## Dynamic capsule for AI enemies.
static func enemy_body(mass: float = 60.0) -> OhaoPhysicsBody:
	var body := OhaoPhysicsBody.new()
	body.set_body_type(OhaoConst.BODY_DYNAMIC)
	body.set_shape_type(OhaoConst.SHAPE_CAPSULE)
	body.set_mass(mass)
	body.set_friction(0.4)
	body.set_restitution(0.1)
	return body


## Static box for walls, floors, platforms.
static func static_body() -> OhaoPhysicsBody:
	var body := OhaoPhysicsBody.new()
	body.set_body_type(OhaoConst.BODY_STATIC)
	body.set_shape_type(OhaoConst.SHAPE_BOX)
	body.set_mass(0.0)
	body.set_friction(0.6)
	body.set_restitution(0.0)
	return body


## Dynamic sphere with configurable bounciness.
static func bouncy_ball(mass: float = 1.0, restitution: float = 0.8) -> OhaoPhysicsBody:
	var body := OhaoPhysicsBody.new()
	body.set_body_type(OhaoConst.BODY_DYNAMIC)
	body.set_shape_type(OhaoConst.SHAPE_SPHERE)
	body.set_mass(mass)
	body.set_friction(0.3)
	body.set_restitution(restitution)
	return body


## Dynamic box for physics props (crates, barrels, etc).
static func prop_body(mass: float = 5.0) -> OhaoPhysicsBody:
	var body := OhaoPhysicsBody.new()
	body.set_body_type(OhaoConst.BODY_DYNAMIC)
	body.set_shape_type(OhaoConst.SHAPE_BOX)
	body.set_mass(mass)
	body.set_friction(0.5)
	body.set_restitution(0.2)
	return body
