# rs2d Engine API

2D rigid-body physics engine for Ragdoll Sandbox. C++17, no external
dependencies, no platform code, no game logic.

Public header: `include/rs2d/physics.h`. CMake target: `rs2d_physics`.

## Concepts

- **Units**: arbitrary. The game uses 1 unit = 1 point, +y up.
- **Ids**: `uint32_t`; `id = slot index + 1`. `0` is always invalid. Ids are
  recycled via a free list; a destroyed body's id may be reused by a later
  body. Never store an id across a `destroyBody` call.
- **Determinism**: fixed `dt` gives bit-identical results across runs (no RNG
  anywhere; pair ordering is sorted). Verified by `test_determinism`.
- **Allocation**: after warmup, `step()` performs no heap allocation.
  Broadphase scratch, contact list, and pair buffers are reused member
  vectors. Creating/destroying bodies and joints may allocate.
- **Solver**: semi-implicit Euler, sequential impulses with Coulomb friction
  (clamped by accumulated normal impulse), Baumgarte position correction.
  Restitution applies as a one-time velocity bias when the approach speed
  exceeds 60 units/s (avoids jitter on resting stacks).

## World

```cpp
rs2d::World world(rs2d::Vec2(0.0f, -1600.0f));  // gravity
world.step(1.0f / 60.0f);                       // dt, velIters=8, posIters=3
world.step(dt, velIters, posIters);              // configurable iterations
world.setGravity(rs2d::Vec2(0, -800));
rs2d::Vec2 g = world.getGravity();
```

`dt` is clamped to a maximum of 1/20 s for stability. Use a fixed `dt`
for deterministic simulation.

## Bodies

```cpp
rs2d::BodyDef def;
def.shape = rs2d::ShapeType::Box;      // Circle, Box, or Polygon
def.position = rs2d::Vec2(100, 200);
def.angle = 0.0f;
def.halfExtents = rs2d::Vec2(18, 22);  // Box
def.radius = 14.0f;                    // Circle
// Polygon: def.polygon[8] (local-space vertices, convex), def.polygonCount
def.density = 0.002f;      // mass per unit area
def.friction = 0.4f;
def.restitution = 0.05f;
def.linearDamping = 0.01f;
def.angularDamping = 0.05f;
def.isStatic = false;      // static bodies have zero inverse mass
def.userData = nullptr;    // opaque pointer for game logic
uint32_t id = world.createBody(def);   // 0 on failure
world.destroyBody(id);                 // also destroys attached joints
```

Mass properties (mass, centroid, inertia) are derived from density and shape
area. `setStatic(id, true/false)` freezes/unfreezes a body while preserving
its mass; frozen bodies have their velocity zeroed.

### Body accessors

```cpp
world.getPosition(id);          // Vec2
world.getAngle(id);             // float, radians
world.getVelocity(id);          // Vec2
world.getAngularVelocity(id);   // float, rad/s
world.setVelocity(id, v);
world.setAngularVelocity(id, w);
world.setTransform(id, pos, angle);   // teleports, zeroes velocity
world.applyImpulse(id, impulse);      // at center of mass
world.applyImpulseAt(id, impulse, worldPoint);
world.isStatic(id);
world.getUserData(id); world.setUserData(id, ptr);
world.getMass(id);
world.getAABB(id);            // rs2d::AABB { Vec2 mn, mx } (world space)
world.bodyCount();
```

All accessors on an invalid/destroyed id return a safe default (zero vector,
0, or nullptr); `isStatic` returns true for invalid ids.

## Joints

Destroying a body destroys its joints. `destroyJoint(id)` removes one joint.

### Revolute (pin) with angle limits and motor

```cpp
rs2d::RevoluteDef rd;
rd.a = bodyA; rd.b = bodyB;
rd.anchor = rs2d::Vec2(x, y);   // world-space; bodies rotate about this point
rd.lower = -0.5f; rd.upper = 0.5f;  // radians, relative to pose at creation
rd.enableLimit = true;
rd.enableMotor = true;
rd.motorSpeed = 3.0f;           // target relative angular velocity (rad/s)
rd.maxMotorTorque = 1e6f;       // torque limit
uint32_t jid = world.createRevolute(rd);
float rel = world.getJointAngle(jid);  // current relative angle (radians)
world.destroyJoint(jid);
```

The point-to-point constraint is solved as a 2x2 effective-mass system;
limits use an inequality constraint with position correction. The motor
drives the relative angular velocity toward `motorSpeed`, clamped by
`maxMotorTorque * dt` of impulse per step.

### Distance (spring rod or rigid rod)

```cpp
rs2d::DistanceDef dd;
dd.a = bodyA; dd.b = bodyB;
dd.anchorA = ...; dd.anchorB = ...;  // world-space; rest length = |anchorB - anchorA|
dd.stiffness = 600.0f; dd.damping = 6.0f;  // soft spring mode
dd.rigid = false;                          // true: hard rod (constraint solve)
uint32_t jid2 = world.createDistance(dd);
```

Soft mode applies an explicit spring force each step. Rigid mode solves a
velocity + position constraint that keeps the anchor distance exactly at the
rest length (no spring oscillation).

### Weld (rigid attachment)

```cpp
rs2d::WeldDef wd;
wd.a = bodyA; wd.b = bodyB;
wd.anchor = rs2d::Vec2(x, y);  // world-space reference point
uint32_t jid3 = world.createWeld(wd);
```

Locks the two bodies' relative position AND relative angle (as of creation).
Useful for gluing debris or building compound bodies.

## Queries

```cpp
rs2d::RaycastHit hit;
if (world.raycast(p0, p1, &hit)) {
  // hit.body (uint32_t, 0 = none), hit.point, hit.normal (world-space),
  // hit.fraction (0..1 along p0->p1)
}

uint32_t ids[64]; int count = 0;
world.queryAABB(mn, mx, ids, &count, 64);  // bodies overlapping the AABB

uint32_t grabbed = world.pickBody(point);  // first non-static body containing point (0 = none)
```

Raycast tests circles analytically and polygons per-edge, with an AABB
slab reject per body; returns the closest hit.

## Contact events

```cpp
class MyListener : public rs2d::ContactListener {
  void beginContact(uint32_t a, uint32_t b) override { /* ... */ }
  void endContact(uint32_t a, uint32_t b) override { /* ... */ }
  // Per-contact impulse after the velocity solve (impact damage, sounds):
  void postSolve(uint32_t a, uint32_t b, rs2d::Vec2 point,
                 rs2d::Vec2 normal, float impulse) override { /* ... */ }
};
world.setContactListener(&listener);  // nullptr to detach
```

Callbacks fire during `step()`; do not create/destroy bodies or joints
inside them. `postSolve` reports the total accumulated normal impulse for
the contact manifold — scale by mass or threshold it for damage.

## Solver notes

- Broadphase: uniform-grid spatial hash (cell 96) with sort+unique pair
  dedup. Handles 500 dynamic bodies at ~3.6 ms/step on a Linux host
  (Release); comfortably within 60 Hz budgets.
- Narrowphase: SAT. Polygon-polygon uses reference-face clipping
  (Sutherland–Hodgman, max 2 contact points); circle-polygon uses
  closest-point with explicit inside/outside handling; circle-circle
  analytic.
- Contacts: sequential impulses, Coulomb friction
  (`mu = sqrt(muA * muB)`), restitution velocity bias (threshold 60 u/s),
  positional Baumgarte (0.55, slop 0.25, per-contact clamp).
- Joints are solved interleaved with contacts each velocity iteration.

## Deviations from the original sketch

The initial task sketch proposed a pointer-based API (`Body*`,
`void* createRevolute(...)`, `RevoluteDef{Body *a, *b; ...}`). During
parallel development, the game-logic agent defined and built against the
id-based API in `include/rs2d/physics.h` (`uint32_t` handles, free-list
recycling). To keep one engine that the game actually uses, this Engine
implements that id-based API. The semantics match the sketch: circle/box/
convex-polygon bodies, density/friction/restitution, static/dynamic,
sequential-impulse solver with configurable iterations, spatial-hash
broadphase, revolute joints with angle limits, distance joints, weld
joints, raycast (body/point/normal/t), AABB query, configurable gravity,
zero allocations in `step()`, deterministic for fixed `dt`. Extensions
beyond the sketch: revolute motor, rigid distance-rod mode, weld joints,
contact listener (begin/end/impact impulse), `pickBody`, `getJointAngle`,
`setStatic` toggling.
