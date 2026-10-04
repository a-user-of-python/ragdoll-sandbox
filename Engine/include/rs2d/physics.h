// rs2d — minimal 2D rigid-body physics engine (reference implementation).
// C++17, no external dependencies, no platform code.
// Units are arbitrary (the game uses 1 unit = 1 point, +y up).
// Deterministic for fixed dt. No allocation inside step().
#pragma once

#include <cstdint>

namespace rs2d {

struct Vec2 {
  float x = 0.0f, y = 0.0f;
  Vec2() = default;
  Vec2(float x_, float y_) : x(x_), y(y_) {}
  Vec2 operator+(const Vec2& o) const { return Vec2(x + o.x, y + o.y); }
  Vec2 operator-(const Vec2& o) const { return Vec2(x - o.x, y - o.y); }
  Vec2 operator*(float s) const { return Vec2(x * s, y * s); }
  Vec2 operator/(float s) const { return Vec2(x / s, y / s); }
  Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
  Vec2& operator-=(const Vec2& o) { x -= o.x; y -= o.y; return *this; }
  Vec2& operator*=(float s) { x *= s; y *= s; return *this; }
  Vec2 operator-() const { return Vec2(-x, -y); }
};

inline float dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
inline float cross(const Vec2& a, const Vec2& b) { return a.x * b.y - a.y * b.x; }
inline Vec2 cross(float s, const Vec2& v) { return Vec2(-s * v.y, s * v.x); }
inline Vec2 cross(const Vec2& v, float s) { return Vec2(s * v.y, -s * v.x); }
inline float lengthSq(const Vec2& v) { return dot(v, v); }
float length(const Vec2& v);
// sqrtf without <cmath> include cost is fine; we include cmath in the .cpp.
Vec2 rotate(const Vec2& v, float angle);  // rotate by angle (radians)

enum class ShapeType : uint8_t { Circle = 0, Box = 1, Polygon = 2 };

struct BodyDef {
  ShapeType shape = ShapeType::Box;
  Vec2 position;
  float angle = 0.0f;
  Vec2 halfExtents{8.0f, 8.0f};   // Box
  float radius = 8.0f;            // Circle
  Vec2 polygon[8];                // Polygon: local-space CCW vertices
  int polygonCount = 0;
  float density = 0.002f;         // mass per unit area
  float friction = 0.4f;
  float restitution = 0.05f;
  float linearDamping = 0.01f;
  float angularDamping = 0.05f;
  bool isStatic = false;
  void* userData = nullptr;
};

struct RevoluteDef {
  uint32_t a = 0;          // body id
  uint32_t b = 0;          // body id
  Vec2 anchor;             // world-space anchor point
  float lower = 0.0f;      // angle limit lower (radians, relative)
  float upper = 0.0f;      // angle limit upper (radians, relative)
  bool enableLimit = false;
  bool enableMotor = false;
  float motorSpeed = 0.0f;     // target relative angular velocity (rad/s, b relative to a)
  float maxMotorTorque = 0.0f; // torque limit for the motor
};

struct DistanceDef {
  uint32_t a = 0;
  uint32_t b = 0;
  Vec2 anchorA;            // world-space
  Vec2 anchorB;            // world-space
  float stiffness = 600.0f;  // spring k (force per unit)
  float damping = 6.0f;      // spring c
  bool rigid = false;        // true: hard distance constraint (rod), ignores spring k/c
};

struct WeldDef {
  uint32_t a = 0;
  uint32_t b = 0;
  Vec2 anchor;             // world-space reference point (bodies keep relative transform)
};

// Contact events for gameplay (impact damage, sound triggers, ...).
// All callbacks fire during step(); do not create/destroy bodies inside them.
class ContactListener {
 public:
  virtual ~ContactListener() = default;
  virtual void beginContact(uint32_t, uint32_t) {}
  virtual void endContact(uint32_t, uint32_t) {}
  // Per contact manifold, after the velocity solve. impulse is the total
  // accumulated normal impulse this step; point is the manifold centroid.
  virtual void postSolve(uint32_t, uint32_t, Vec2, Vec2, float) {}
};

struct RaycastHit {
  uint32_t body = 0;       // 0 = no hit
  Vec2 point;
  Vec2 normal;             // world-space, points back along ray
  float fraction = 1.0f;   // 0..1 along p0->p1
};

struct AABB {
  Vec2 mn, mx;
};

class World {
 public:
  // Opaque internals (defined in physics.cpp).
  struct Body;
  struct Joint;
  struct Contact;

  explicit World(Vec2 gravity);
  ~World();

  World(const World&) = delete;
  World& operator=(const World&) = delete;

  // Bodies. Returns id (>0); 0 on failure.
  uint32_t createBody(const BodyDef& def);
  void destroyBody(uint32_t id);

  // Joints. Returns id (>0); 0 on failure.
  uint32_t createRevolute(const RevoluteDef& def);
  uint32_t createDistance(const DistanceDef& def);
  uint32_t createWeld(const WeldDef& def);
  void destroyJoint(uint32_t id);
  // Relative angle (b - a, minus the reference angle at creation) for
  // revolute/weld joints. 0 if the joint id is invalid.
  float getJointAngle(uint32_t id) const;

  void setContactListener(ContactListener* listener);

  void step(float dt, int velIters = 8, int posIters = 3);

  // Closest hit along segment p0->p1. Returns false if none.
  bool raycast(Vec2 p0, Vec2 p1, RaycastHit* out) const;
  // Bodies whose AABB overlaps [mn,mx]. *count set to number written.
  void queryAABB(Vec2 mn, Vec2 mx, uint32_t* out, int* count, int max) const;
  // N4: paginated variant — skips the first `offset` matches so callers can
  // loop until count < max and never silently drop bodies past the cap.
  void queryAABB(Vec2 mn, Vec2 mx, uint32_t* out, int* count, int max,
                 int offset) const;
  // First non-static body containing point p (for grab tool).
  uint32_t pickBody(Vec2 p) const;

  // Accessors
  int bodyCount() const;
  Vec2 getPosition(uint32_t id) const;
  float getAngle(uint32_t id) const;
  Vec2 getVelocity(uint32_t id) const;
  float getAngularVelocity(uint32_t id) const;
  void setVelocity(uint32_t id, Vec2 v);
  void setAngularVelocity(uint32_t id, float w);
  void setTransform(uint32_t id, Vec2 p, float angle);
  void applyImpulse(uint32_t id, Vec2 impulse);                 // at center of mass
  void applyImpulseAt(uint32_t id, Vec2 impulse, Vec2 point);   // world point
  void setStatic(uint32_t id, bool s);
  bool isStatic(uint32_t id) const;
  void* getUserData(uint32_t id) const;
  void setUserData(uint32_t id, void* d);
  float getMass(uint32_t id) const;
  AABB getAABB(uint32_t id) const;
  Vec2 getGravity() const;
  void setGravity(Vec2 g);

 private:
  Body* getBody(uint32_t id);
  const Body* getBody(uint32_t id) const;
  void integrate(float dt);
  void solveJointsVelocity(float dt);
  void solveJointsPosition();
  void collide();
  void solveContactsVelocity(float dt);
  void solveContactsPosition();
  void computeAABB(Body& b);

  Vec2 gravity_;
  // Dense slots with free-list; id = index+1.
  struct BodySlot { Body* ptr = nullptr; };
  // Implemented with std::vector in the .cpp via PIMPL to keep header clean.
  struct Impl;
  Impl* impl_;
};

}  // namespace rs2d
