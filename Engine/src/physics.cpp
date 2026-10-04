// rs2d — 2D rigid-body physics engine, reference implementation.
// Sequential-impulse solver, SAT collision, revolute/distance joints.
// Units: arbitrary (game uses points, +y up). No allocation in step().
#include "rs2d/physics.h"

#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>

namespace rs2d {

float length(const Vec2& v) { return std::sqrt(dot(v, v)); }

Vec2 rotate(const Vec2& v, float angle) {
  float c = std::cos(angle), s = std::sin(angle);
  return Vec2(c * v.x - s * v.y, s * v.x + c * v.y);
}

// N1: normalize a relative joint angle to [-PI, PI]. Body angles accumulate
// unboundedly in integrate(), so without this the limit check compares a
// multi-revolution angle against radian limits after forced large rotations
// (explosion spin, teleport), applying correction as if massively violated.
static float wrapAngle(float a) {
  const float PI = 3.14159265358979323846f;
  const float TAU = 2.0f * PI;
  a = std::fmod(a + PI, TAU);
  if (a < 0.0f) a += TAU;
  return a - PI;
}

// ---- Body / Joint / Contact (World:: member types, defined here) ---------

struct World::Body {
  uint32_t id = 0;
  bool alive = false;
  ShapeType shape = ShapeType::Box;
  Vec2 pos, vel;
  float angle = 0.0f, angVel = 0.0f;
  float invMass = 0.0f, invI = 0.0f;
  float mass = 0.0f, inertia = 0.0f;
  float friction = 0.4f, restitution = 0.05f;
  float linearDamping = 0.01f, angularDamping = 0.05f;
  bool isStatic = false;
  // shape data
  float radius = 0.0f;            // circle
  Vec2 halfExtents;               // box
  Vec2 verts[8];                  // polygon local-space CCW
  Vec2 normals[8];                // polygon local edge normals
  int vertCount = 0;
  void* userData = nullptr;
  AABB aabb;
};

struct World::Joint {
  uint32_t id = 0;
  bool alive = false;
  bool isRevolute = true;
  bool isWeld = false;      // weld: point-to-point + fixed relative angle
  uint32_t a = 0, b = 0;  // body ids
  // revolute / weld
  Vec2 localA, localB;
  float refAngle = 0.0f;
  float lower = 0.0f, upper = 0.0f;
  bool enableLimit = false;
  Vec2 pImpulse;      // accumulated point impulse
  float limitImpulse = 0.0f;
  float angImpulse = 0.0f;  // weld angular impulse accumulator
  // revolute motor
  bool enableMotor = false;
  float motorSpeed = 0.0f;
  float maxMotorTorque = 0.0f;
  float motorImpulse = 0.0f;
  // distance (spring or rigid rod)
  float restLength = 0.0f;
  float stiffness = 600.0f, damping = 6.0f;
  bool rigid = false;
  float distImpulse = 0.0f;  // rigid distance impulse accumulator
};

struct World::Contact {
  uint32_t a = 0, b = 0;
  Vec2 normal;             // from a to b
  float penetration = 0.0f;
  Vec2 points[2];
  int pointCount = 0;
  float normalImpulse[2] = {0.0f, 0.0f};
  float tangentImpulse[2] = {0.0f, 0.0f};
  float restBias[2] = {0.0f, 0.0f};  // restitution velocity bias, set once per step
};

namespace {

using Body = World::Body;
using Joint = World::Joint;
using Contact = World::Contact;

void computeMass(Body& b, const BodyDef& def) {
  float density = def.density;
  if (b.isStatic || density <= 0.0f) {
    b.invMass = 0.0f; b.invI = 0.0f; b.mass = 0.0f;
    return;
  }
  float m = 0.0f, I = 0.0f;
  if (b.shape == ShapeType::Circle) {
    m = density * 3.14159265f * b.radius * b.radius;
    I = 0.5f * m * b.radius * b.radius;
  } else if (b.shape == ShapeType::Box) {
    float w = b.halfExtents.x * 2.0f, h = b.halfExtents.y * 2.0f;
    m = density * w * h;
    I = m / 12.0f * (w * w + h * h);
  } else {
    // Polygon: shoelace area + standard polygon inertia about centroid.
    float area2 = 0.0f;  // 2*area
    float cx = 0.0f, cy = 0.0f;
    float inertiaNum = 0.0f;
    int n = b.vertCount;
    for (int i = 0; i < n; ++i) {
      const Vec2& p0 = b.verts[i];
      const Vec2& p1 = b.verts[(i + 1) % n];
      float cr = cross(p0, p1);
      area2 += cr;
      cx += (p0.x + p1.x) * cr;
      cy += (p0.y + p1.y) * cr;
      inertiaNum += cr * (dot(p0, p0) + dot(p0, p1) + dot(p1, p1));
    }
    float area = 0.5f * area2;
    m = density * (area > 0.0f ? area : 0.0f);
    if (area > 1e-9f) {
      cx /= (3.0f * area2); cy /= (3.0f * area2);
      // inertia about origin then shift to centroid
      float iOrigin = density * inertiaNum / 12.0f;
      I = iOrigin - m * (cx * cx + cy * cy);
      if (I < 0.0f) I = 0.0f;
    }
  }
  b.mass = m;
  b.inertia = I;
  b.invMass = m > 1e-9f ? 1.0f / m : 0.0f;
  b.invI = I > 1e-9f ? 1.0f / I : 0.0f;
}

void buildBoxVerts(Body& b) {
  float hx = b.halfExtents.x, hy = b.halfExtents.y;
  b.verts[0] = Vec2(-hx, -hy);
  b.verts[1] = Vec2(hx, -hy);
  b.verts[2] = Vec2(hx, hy);
  b.verts[3] = Vec2(-hx, hy);
  b.vertCount = 4;
  for (int i = 0; i < 4; ++i) {
    Vec2 e = b.verts[(i + 1) % 4] - b.verts[i];
    Vec2 n = Vec2(e.y, -e.x);
    float l = length(n);
    b.normals[i] = l > 1e-9f ? n / l : Vec2(1, 0);
  }
}

// World-space vertices of a polygon-ish body (box or polygon).
int worldVerts(const Body& b, Vec2 out[8]) {
  if (b.shape == ShapeType::Circle) return 0;
  for (int i = 0; i < b.vertCount; ++i) out[i] = rotate(b.verts[i], b.angle) + b.pos;
  return b.vertCount;
}

Vec2 worldNormal(const Body& b, int i) {
  return rotate(b.normals[i % b.vertCount], b.angle);
}

// Support point of polygon body in direction d (world).
Vec2 support(const Body& b, const Vec2& d) {
  Vec2 wv[8];
  int n = worldVerts(b, wv);
  float best = dot(wv[0], d);
  Vec2 pt = wv[0];
  for (int i = 1; i < n; ++i) {
    float v = dot(wv[i], d);
    if (v > best) { best = v; pt = wv[i]; }
  }
  return pt;
}

struct FaceQuery {
  float separation = -1e9f;
  int index = 0;  // face index on body A
};

// Max separation of B from a face of A.
FaceQuery maxSeparation(const Body& A, const Body& B) {
  FaceQuery q;
  Vec2 wvA[8], wvB[8];
  int nA = worldVerts(A, wvA);
  int nB = worldVerts(B, wvB);
  (void)wvB; (void)nB;
  for (int i = 0; i < nA; ++i) {
    Vec2 n = worldNormal(A, i);
    Vec2 p = support(B, Vec2(-n.x, -n.y));
    float s = dot(n, p - wvA[i]);
    if (s > q.separation) { q.separation = s; q.index = i; }
  }
  return q;
}

// Clip segment (p0,p1) to line: keep points with dot(n, p) <= offset.
int clipSegment(Vec2 p0, Vec2 p1, Vec2 n, float offset, Vec2 out[2]) {
  int count = 0;
  float d0 = dot(n, p0) - offset;
  float d1 = dot(n, p1) - offset;
  if (d0 <= 0.0f) out[count++] = p0;
  if (d1 <= 0.0f) out[count++] = p1;
  if (d0 * d1 < 0.0f) {
    float t = d0 / (d0 - d1);
    out[count++] = p0 + (p1 - p0) * t;
  }
  return count;
}

bool collidePolygons(const Body& A, const Body& B, Contact& c) {
  FaceQuery qa = maxSeparation(A, B);
  if (qa.separation > 0.0f) return false;
  FaceQuery qb = maxSeparation(B, A);
  if (qb.separation > 0.0f) return false;

  const Body *ref, *inc;
  int refIndex;
  bool flip;
  if (qb.separation > qa.separation + 0.1f) {
    ref = &B; inc = &A; refIndex = qb.index; flip = true;
  } else {
    ref = &A; inc = &B; refIndex = qa.index; flip = false;
  }

  Vec2 wvR[8], wvI[8];
  int nR = worldVerts(*ref, wvR);
  int nI = worldVerts(*inc, wvI);
  (void)nI;

  Vec2 refN = worldNormal(*ref, refIndex);
  Vec2 v0 = wvR[refIndex];
  Vec2 v1 = wvR[(refIndex + 1) % nR];
  // Incident edge: most anti-parallel to refN.
  float best = 1e9f;
  int incIndex = 0;
  for (int i = 0; i < inc->vertCount; ++i) {
    float v = dot(worldNormal(*inc, i), refN);
    if (v < best) { best = v; incIndex = i; }
  }
  Vec2 i0 = wvI[incIndex];
  Vec2 i1 = wvI[(incIndex + 1) % inc->vertCount];

  // Clip incident edge to reference side planes.
  Vec2 refEdge = v1 - v0;
  float refLen = length(refEdge);
  Vec2 refT = refLen > 1e-9f ? refEdge / refLen : Vec2(1, 0);
  Vec2 clipped[2];
  int nClip = clipSegment(i0, i1, refT, dot(refT, v1), clipped);
  if (nClip < 2) return false;
  Vec2 clipped2[2];
  nClip = clipSegment(clipped[0], clipped[1], Vec2(-refT.x, -refT.y), dot(refT, -v0), clipped2);
  if (nClip < 2) return false;

  c.pointCount = 0;
  float refOffset = dot(refN, v0);
  for (int i = 0; i < 2; ++i) {
    float sep = dot(refN, clipped2[i]) - refOffset;
    if (sep <= 0.0f) {
      c.points[c.pointCount++] = clipped2[i];
    }
  }
  if (c.pointCount == 0) return false;
  c.normal = flip ? Vec2(-refN.x, -refN.y) : refN;
  // penetration = max over points of (refOffset - dot(refN, p))
  c.penetration = 0.0f;
  for (int i = 0; i < c.pointCount; ++i) {
    float pen = refOffset - dot(refN, clipped2[i]);
    if (pen > c.penetration) c.penetration = pen;
  }
  return true;
}

bool collideCirclePolygon(const Body& circ, const Body& poly, Contact& c, bool circIsA) {
  // Returns contact with normal pointing from A to B.
  // nrm0 points from poly to circ.
  Vec2 wv[8];
  int n = worldVerts(poly, wv);
  Vec2 center = circ.pos;

  // FIRST: is the center inside the polygon? Must test before the
  // closest-point path: a center inside but within `radius` of the boundary
  // yields a (center-closest) vector pointing INTO the polygon, i.e. a
  // flipped normal that injects energy instead of separating.
  bool inside = true;
  float minSep = 1e30f;
  Vec2 minN{1.0f, 0.0f};
  for (int i = 0; i < n; ++i) {
    Vec2 nn = worldNormal(poly, i);
    float s = dot(nn, center - wv[i]);
    if (s > 0.0f) { inside = false; break; }
    if (-s < minSep) { minSep = -s; minN = nn; }
  }
  if (inside) {
    // Push circ out along poly's outward face normal.
    // Contact normal is A->B: if circ is A, normal = -minN; else +minN.
    Vec2 nrm = circIsA ? Vec2(-minN.x, -minN.y) : minN;
    c.normal = nrm;
    c.penetration = circ.radius + minSep;
    c.points[0] = center + nrm * (circ.radius * 0.5f);
    c.pointCount = 1;
    return true;
  }

  // Outside: closest boundary point.
  float bestD2 = 1e30f;
  Vec2 closest = wv[0];
  for (int i = 0; i < n; ++i) {
    Vec2 a = wv[i], b2 = wv[(i + 1) % n];
    Vec2 ab = b2 - a;
    float t = dot(center - a, ab) / (dot(ab, ab) + 1e-9f);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    Vec2 p = a + ab * t;
    float d2 = lengthSq(center - p);
    if (d2 < bestD2) { bestD2 = d2; closest = p; }
  }
  Vec2 d = center - closest;
  float dist = length(d);
  if (dist > circ.radius) return false;
  Vec2 nrm0 = dist > 1e-6f ? d / dist : minN;  // poly -> circ (outward)
  c.normal = circIsA ? Vec2(-nrm0.x, -nrm0.y) : nrm0;  // A -> B
  c.penetration = circ.radius - dist;
  c.points[0] = closest;
  c.pointCount = 1;
  return true;
}

bool collideCircles(const Body& A, const Body& B, Contact& c) {
  Vec2 d = B.pos - A.pos;
  float dist = length(d);
  float rr = A.radius + B.radius;
  if (dist >= rr) return false;
  c.normal = dist > 1e-6f ? d / dist : Vec2(1, 0);
  c.penetration = rr - dist;
  c.points[0] = A.pos + c.normal * (A.radius - c.penetration * 0.5f);
  c.pointCount = 1;
  return true;
}

bool collideBodies(const Body& A, const Body& B, Contact& c) {
  c.a = A.id; c.b = B.id;
  bool aC = A.shape == ShapeType::Circle;
  bool bC = B.shape == ShapeType::Circle;
  if (aC && bC) return collideCircles(A, B, c);
  if (aC && !bC) return collideCirclePolygon(A, B, c, true);   // circ is A
  if (!aC && bC) return collideCirclePolygon(B, A, c, false);  // circ is B
  return collidePolygons(A, B, c);
}

}  // namespace

// ---- World impl -------------------------------------------------------------

struct World::Impl {
  std::vector<Body> bodies;       // index = id-1
  std::vector<uint32_t> freeBodies;
  std::vector<Joint> joints;
  std::vector<uint32_t> freeJoints;
  std::vector<Contact> contacts;
  uint32_t nextBodyId = 1;
  uint32_t nextJointId = 1;
  ContactListener* listener = nullptr;
  std::vector<uint64_t> prevPairs;  // sorted contact pair keys from last step
  // Reused broadphase scratch (no allocation inside step() after warmup).
  struct CellEntry { uint32_t id; int cx, cy; };
  std::vector<CellEntry> entries;
  std::vector<uint64_t> tested;
};

World::World(Vec2 gravity) : gravity_(gravity), impl_(new Impl()) {}
World::~World() { delete impl_; }

World::Body* World::getBody(uint32_t id) {
  if (id == 0 || id > impl_->bodies.size()) return nullptr;
  Body& b = impl_->bodies[id - 1];
  return b.alive ? &b : nullptr;
}
const World::Body* World::getBody(uint32_t id) const {
  if (id == 0 || id > impl_->bodies.size()) return nullptr;
  const Body& b = impl_->bodies[id - 1];
  return b.alive ? &b : nullptr;
}

uint32_t World::createBody(const BodyDef& def) {
  Body b;
  b.shape = def.shape;
  b.pos = def.position;
  b.angle = def.angle;
  b.friction = def.friction;
  b.restitution = def.restitution;
  b.linearDamping = def.linearDamping;
  b.angularDamping = def.angularDamping;
  b.isStatic = def.isStatic;
  b.userData = def.userData;
  if (def.shape == ShapeType::Circle) {
    b.radius = def.radius > 0.5f ? def.radius : 0.5f;
  } else if (def.shape == ShapeType::Box) {
    // N7: clamp like circles — zero extents make an immovable ghost
    // (zero area -> zero mass), negative extents invert the geometry.
    b.halfExtents = Vec2(def.halfExtents.x > 0.5f ? def.halfExtents.x : 0.5f,
                         def.halfExtents.y > 0.5f ? def.halfExtents.y : 0.5f);
    buildBoxVerts(b);
  } else {
    int n = def.polygonCount < 8 ? def.polygonCount : 8;
    if (n < 3) return 0;
    for (int i = 0; i < n; ++i) b.verts[i] = def.polygon[i];
    b.vertCount = n;
    for (int i = 0; i < n; ++i) {
      Vec2 e = b.verts[(i + 1) % n] - b.verts[i];
      Vec2 nn = Vec2(e.y, -e.x);
      float l = length(nn);
      b.normals[i] = l > 1e-9f ? nn / l : Vec2(1, 0);
    }
  }
  computeMass(b, def);
  computeAABB(b);

  uint32_t id;
  if (!impl_->freeBodies.empty()) {
    id = impl_->freeBodies.back(); impl_->freeBodies.pop_back();
    impl_->bodies[id - 1] = b;
  } else {
    id = impl_->nextBodyId++;
    impl_->bodies.push_back(b);
  }
  Body& slot = impl_->bodies[id - 1];
  slot.id = id;
  slot.alive = true;
  return id;
}

void World::destroyBody(uint32_t id) {
  Body* b = getBody(id);
  if (!b) return;
  // Destroy attached joints.
  for (auto& j : impl_->joints) {
    if (j.alive && (j.a == id || j.b == id)) {
      j.alive = false;
      impl_->freeJoints.push_back(j.id);
    }
  }
  b->alive = false;
  impl_->freeBodies.push_back(id);
}

uint32_t World::createRevolute(const RevoluteDef& def) {
  Body* a = getBody(def.a);
  Body* b = getBody(def.b);
  if (!a || !b || def.a == def.b) return 0;
  Joint j;
  j.isRevolute = true;
  j.a = def.a; j.b = def.b;
  // local anchors
  Vec2 pa = def.anchor - a->pos;
  Vec2 pb = def.anchor - b->pos;
  // inverse rotate
  float ca = std::cos(-a->angle), sa = std::sin(-a->angle);
  float cb = std::cos(-b->angle), sb = std::sin(-b->angle);
  j.localA = Vec2(ca * pa.x - sa * pa.y, sa * pa.x + ca * pa.y);
  j.localB = Vec2(cb * pb.x - sb * pb.y, sb * pb.x + cb * pb.y);
  j.refAngle = b->angle - a->angle;
  j.lower = def.lower; j.upper = def.upper;
  j.enableLimit = def.enableLimit;
  j.enableMotor = def.enableMotor;
  j.motorSpeed = def.motorSpeed;
  j.maxMotorTorque = def.maxMotorTorque;

  uint32_t id;
  if (!impl_->freeJoints.empty()) {
    id = impl_->freeJoints.back(); impl_->freeJoints.pop_back();
    impl_->joints[id - 1] = j;
  } else {
    id = impl_->nextJointId++;
    impl_->joints.push_back(j);
  }
  Joint& s = impl_->joints[id - 1];
  s.id = id; s.alive = true;
  return id;
}

uint32_t World::createWeld(const WeldDef& def) {
  Body* a = getBody(def.a);
  Body* b = getBody(def.b);
  if (!a || !b || def.a == def.b) return 0;
  Joint j;
  j.isRevolute = false;   // not the spring path
  j.isWeld = true;
  j.a = def.a; j.b = def.b;
  float ca = std::cos(-a->angle), sa = std::sin(-a->angle);
  float cb = std::cos(-b->angle), sb = std::sin(-b->angle);
  Vec2 pa = def.anchor - a->pos, pb = def.anchor - b->pos;
  j.localA = Vec2(ca * pa.x - sa * pa.y, sa * pa.x + ca * pa.y);
  j.localB = Vec2(cb * pb.x - sb * pb.y, sb * pb.x + cb * pb.y);
  j.refAngle = b->angle - a->angle;
  uint32_t id;
  if (!impl_->freeJoints.empty()) {
    id = impl_->freeJoints.back(); impl_->freeJoints.pop_back();
    impl_->joints[id - 1] = j;
  } else {
    id = impl_->nextJointId++;
    impl_->joints.push_back(j);
  }
  Joint& s = impl_->joints[id - 1];
  s.id = id; s.alive = true;
  return id;
}

float World::getJointAngle(uint32_t id) const {
  if (id == 0 || id > impl_->joints.size()) return 0.0f;
  const Joint& j = impl_->joints[id - 1];
  if (!j.alive || (!j.isRevolute && !j.isWeld)) return 0.0f;
  const Body* A = getBody(j.a);
  const Body* B = getBody(j.b);
  if (!A || !B) return 0.0f;
  return wrapAngle((B->angle - A->angle) - j.refAngle);  // N1: normalized
}

void World::setContactListener(ContactListener* listener) {
  impl_->listener = listener;
}

uint32_t World::createDistance(const DistanceDef& def) {
  Body* a = getBody(def.a);
  Body* b = getBody(def.b);
  if (!a || !b || def.a == def.b) return 0;
  Joint j;
  j.isRevolute = false;
  j.a = def.a; j.b = def.b;
  float ca = std::cos(-a->angle), sa = std::sin(-a->angle);
  float cb = std::cos(-b->angle), sb = std::sin(-b->angle);
  Vec2 pa = def.anchorA - a->pos, pb = def.anchorB - b->pos;
  j.localA = Vec2(ca * pa.x - sa * pa.y, sa * pa.x + ca * pa.y);
  j.localB = Vec2(cb * pb.x - sb * pb.y, sb * pb.x + cb * pb.y);
  j.restLength = length(def.anchorB - def.anchorA);
  j.stiffness = def.stiffness; j.damping = def.damping;
  j.rigid = def.rigid;
  uint32_t id;
  if (!impl_->freeJoints.empty()) {
    id = impl_->freeJoints.back(); impl_->freeJoints.pop_back();
    impl_->joints[id - 1] = j;
  } else {
    id = impl_->nextJointId++;
    impl_->joints.push_back(j);
  }
  Joint& s = impl_->joints[id - 1];
  s.id = id; s.alive = true;
  return id;
}

void World::destroyJoint(uint32_t id) {
  if (id == 0 || id > impl_->joints.size()) return;
  Joint& j = impl_->joints[id - 1];
  if (!j.alive) return;
  j.alive = false;
  impl_->freeJoints.push_back(id);
}

void World::computeAABB(Body& b) {
  if (b.shape == ShapeType::Circle) {
    b.aabb.mn = b.pos - Vec2(b.radius, b.radius);
    b.aabb.mx = b.pos + Vec2(b.radius, b.radius);
  } else {
    Vec2 wv[8];
    int n = worldVerts(b, wv);
    Vec2 mn = wv[0], mx = wv[0];
    for (int i = 1; i < n; ++i) {
      if (wv[i].x < mn.x) mn.x = wv[i].x;
      if (wv[i].y < mn.y) mn.y = wv[i].y;
      if (wv[i].x > mx.x) mx.x = wv[i].x;
      if (wv[i].y > mx.y) mx.y = wv[i].y;
    }
    b.aabb.mn = mn; b.aabb.mx = mx;
  }
}

void World::integrate(float dt) {
  for (auto& b : impl_->bodies) {
    if (!b.alive || b.isStatic) continue;
    // N3: a NaN/inf anywhere (bad mod input via applyImpulse, 0/0 in game
    // code) permanently zombies the body — NaN position spreads to the AABB
    // and poisons the broadphase. Sanitize: kill the velocity and snap to
    // the last finite state so one bad value can't corrupt the world.
    if (!std::isfinite(b.vel.x) || !std::isfinite(b.vel.y) ||
        !std::isfinite(b.angVel) || !std::isfinite(b.pos.x) ||
        !std::isfinite(b.pos.y) || !std::isfinite(b.angle)) {
      b.vel = Vec2(0.0f, 0.0f);
      b.angVel = 0.0f;
      if (!std::isfinite(b.pos.x)) b.pos.x = 0.0f;
      if (!std::isfinite(b.pos.y)) b.pos.y = 0.0f;
      if (!std::isfinite(b.angle)) b.angle = 0.0f;
      computeAABB(b);
      continue;
    }
    b.vel += gravity_ * dt;
    // damping
    b.vel *= 1.0f / (1.0f + b.linearDamping * dt);
    b.angVel *= 1.0f / (1.0f + b.angularDamping * dt);
    b.pos += b.vel * dt;
    b.angle += b.angVel * dt;
    computeAABB(b);
  }
}

void World::collide() {
  impl_->contacts.clear();
  auto& bodies = impl_->bodies;
  // Spatial hash broadphase with reused scratch (no allocation after warmup).
  const float cell = 96.0f;
  auto& entries = impl_->entries;
  entries.clear();
  entries.reserve(bodies.size() * 2);
  for (auto& b : bodies) {
    if (!b.alive) continue;
    int x0 = (int)std::floor(b.aabb.mn.x / cell);
    int x1 = (int)std::floor(b.aabb.mx.x / cell);
    int y0 = (int)std::floor(b.aabb.mn.y / cell);
    int y1 = (int)std::floor(b.aabb.mx.y / cell);
    for (int cx = x0; cx <= x1; ++cx)
      for (int cy = y0; cy <= y1; ++cy)
        entries.push_back({b.id, cx, cy});
  }
  // Sort by cell then check pairs within same cell with dedup set.
  auto& tested = impl_->tested;
  tested.clear();
  tested.reserve(4096);
  // sort entries by (cx,cy)
  std::sort(entries.begin(), entries.end(), [](const Impl::CellEntry& a, const Impl::CellEntry& b) {
    if (a.cx != b.cx) return a.cx < b.cx;
    return a.cy < b.cy;
  });
  size_t i = 0;
  while (i < entries.size()) {
    size_t j = i;
    while (j < entries.size() && entries[j].cx == entries[i].cx && entries[j].cy == entries[i].cy) ++j;
    for (size_t a = i; a < j; ++a) {
      for (size_t c = a + 1; c < j; ++c) {
        uint32_t idA = entries[a].id, idB = entries[c].id;
        if (idA == idB) continue;
        uint32_t lo = idA < idB ? idA : idB;
        uint32_t hi = idA < idB ? idB : idA;
        // Duplicates across shared cells are removed by sort+unique below.
        tested.push_back(((uint64_t)lo << 32) | hi);
      }
    }
    i = j;
  }
  std::sort(tested.begin(), tested.end());
  tested.erase(std::unique(tested.begin(), tested.end()), tested.end());
  for (uint64_t key : tested) {
    uint32_t idA = (uint32_t)(key >> 32);
    uint32_t idB = (uint32_t)(key & 0xffffffffu);
    Body* A = getBody(idA);
    Body* B = getBody(idB);
    if (!A || !B) continue;
    if (A->isStatic && B->isStatic) continue;
    // AABB overlap test
    if (A->aabb.mx.x < B->aabb.mn.x || A->aabb.mn.x > B->aabb.mx.x ||
        A->aabb.mx.y < B->aabb.mn.y || A->aabb.mn.y > B->aabb.mx.y)
      continue;
    Contact ct;
    if (collideBodies(*A, *B, ct)) impl_->contacts.push_back(ct);
  }

  // Contact begin/end events (sorted pair keys keep this deterministic).
  if (impl_->listener) {
    std::vector<uint64_t> cur;
    cur.reserve(impl_->contacts.size());
    for (auto& c : impl_->contacts) {
      uint32_t lo = c.a < c.b ? c.a : c.b;
      uint32_t hi = c.a < c.b ? c.b : c.a;
      cur.push_back(((uint64_t)lo << 32) | hi);
    }
    std::sort(cur.begin(), cur.end());
    cur.erase(std::unique(cur.begin(), cur.end()), cur.end());
    auto& prev = impl_->prevPairs;
    size_t pi = 0, ci = 0;
    while (pi < prev.size() || ci < cur.size()) {
      uint64_t pk = pi < prev.size() ? prev[pi] : UINT64_MAX;
      uint64_t ck = ci < cur.size() ? cur[ci] : UINT64_MAX;
      if (pk < ck) {
        impl_->listener->endContact((uint32_t)(pk >> 32), (uint32_t)(pk & 0xffffffffu));
        ++pi;
      } else if (ck < pk) {
        impl_->listener->beginContact((uint32_t)(ck >> 32), (uint32_t)(ck & 0xffffffffu));
        ++ci;
      } else {
        ++pi; ++ci;
      }
    }
    prev.swap(cur);
  } else {
    impl_->prevPairs.clear();
  }
}

void World::solveContactsVelocity(float dt) {
  // Note: no Baumgarte bias in the velocity solver (it injects energy when
  // applied across iterations on deep penetrations). Separation is handled
  // by the positional solver. dt unused.
  (void)dt;
  const float slop = 0.5f;
  (void)slop;
  for (auto& c : impl_->contacts) {
    Body* A = getBody(c.a);
    Body* B = getBody(c.b);
    if (!A || !B) continue;
    float mu = std::sqrt(A->friction * B->friction);
    for (int p = 0; p < c.pointCount; ++p) {
      Vec2 pt = c.points[p];
      Vec2 rA = pt - A->pos, rB = pt - B->pos;
      Vec2 dv = (B->vel + cross(B->angVel, rB)) - (A->vel + cross(A->angVel, rA));
      float vn = dot(dv, c.normal);
      // normal impulse
      float rnA = cross(rA, c.normal), rnB = cross(rB, c.normal);
      float kn = A->invMass + B->invMass + rnA * rnA * A->invI + rnB * rnB * B->invI;
      if (kn < 1e-9f) continue;
      // Normal impulse with one-time restitution bias (computed in step()).
      // The bias is fixed for the step; later iterations must not re-derive
      // restitution from the (now separating) velocity.
      float dPn = (-vn + c.restBias[p]) / kn;
      float pn0 = c.normalImpulse[p];
      c.normalImpulse[p] = pn0 + dPn;
      if (c.normalImpulse[p] < 0.0f) c.normalImpulse[p] = 0.0f;
      Vec2 Pn = c.normal * (c.normalImpulse[p] - pn0);
      A->vel -= Pn * A->invMass; A->angVel -= A->invI * cross(rA, Pn);
      B->vel += Pn * B->invMass; B->angVel += B->invI * cross(rB, Pn);
      // friction
      Vec2 tangent = Vec2(-c.normal.y, c.normal.x);
      dv = (B->vel + cross(B->angVel, rB)) - (A->vel + cross(A->angVel, rA));
      float vt = dot(dv, tangent);
      float rtA = cross(rA, tangent), rtB = cross(rB, tangent);
      float kt = A->invMass + B->invMass + rtA * rtA * A->invI + rtB * rtB * B->invI;
      if (kt < 1e-9f) continue;
      float dPt = -vt / kt;
      float pt0 = c.tangentImpulse[p];
      float maxF = mu * c.normalImpulse[p];
      c.tangentImpulse[p] = pt0 + dPt;
      if (c.tangentImpulse[p] > maxF) c.tangentImpulse[p] = maxF;
      if (c.tangentImpulse[p] < -maxF) c.tangentImpulse[p] = -maxF;
      Vec2 Pt = tangent * (c.tangentImpulse[p] - pt0);
      A->vel -= Pt * A->invMass; A->angVel -= A->invI * cross(rA, Pt);
      B->vel += Pt * B->invMass; B->angVel += B->invI * cross(rB, Pt);
    }
  }
}

void World::solveContactsPosition() {
  const float baumgarte = 0.55f;
  const float slop = 0.25f;
  const float maxCorr = 6.0f;  // clamp per-contact correction (prevents overshoot)
  for (auto& c : impl_->contacts) {
    Body* A = getBody(c.a);
    Body* B = getBody(c.b);
    if (!A || !B) continue;
    float pen = c.penetration - slop;
    if (pen <= 0.0f) continue;
    if (pen > maxCorr) pen = maxCorr;
    float totalInv = A->invMass + B->invMass;
    if (totalInv < 1e-9f) continue;
    Vec2 corr = c.normal * (baumgarte * pen / totalInv / (float)(c.pointCount > 0 ? c.pointCount : 1));
    A->pos -= corr * A->invMass;
    B->pos += corr * B->invMass;
  }
  for (auto& b : impl_->bodies) if (b.alive) computeAABB(b);
}

void World::solveJointsVelocity(float dt) {
  for (auto& j : impl_->joints) {
    if (!j.alive) continue;
    if (!j.isRevolute && !j.isWeld && !j.rigid) continue;  // soft springs handled in step()
    Body* A = getBody(j.a);
    Body* B = getBody(j.b);
    if (!A || !B) continue;
    Vec2 rA = rotate(j.localA, A->angle);
    Vec2 rB = rotate(j.localB, B->angle);

    if (j.isRevolute || j.isWeld) {
      // point-to-point 2x2
      Vec2 dv = (B->vel + cross(B->angVel, rB)) - (A->vel + cross(A->angVel, rA));
      float k11 = A->invMass + B->invMass + A->invI * cross(rA, Vec2(1,0)) * cross(rA, Vec2(1,0))
                + B->invI * cross(rB, Vec2(1,0)) * cross(rB, Vec2(1,0));
      float k12 = A->invI * cross(rA, Vec2(1,0)) * cross(rA, Vec2(0,1))
                + B->invI * cross(rB, Vec2(1,0)) * cross(rB, Vec2(0,1));
      float k22 = A->invMass + B->invMass + A->invI * cross(rA, Vec2(0,1)) * cross(rA, Vec2(0,1))
                + B->invI * cross(rB, Vec2(0,1)) * cross(rB, Vec2(0,1));
      float det = k11 * k22 - k12 * k12;
      if (std::fabs(det) > 1e-9f) {
        Vec2 rhs = Vec2(-dv.x, -dv.y);
        Vec2 lambda = Vec2((k22 * rhs.x - k12 * rhs.y) / det,
                           (k11 * rhs.y - k12 * rhs.x) / det);
        j.pImpulse += lambda;
        A->vel -= lambda * A->invMass;
        A->angVel -= A->invI * cross(rA, lambda);
        B->vel += lambda * B->invMass;
        B->angVel += B->invI * cross(rB, lambda);
      }
    }

    if (j.isWeld) {
      // Lock relative angle: kill relative angular velocity.
      float k = A->invI + B->invI;
      if (k > 1e-9f) {
        float dL = -((B->angVel - A->angVel)) / k;
        j.angImpulse += dL;
        A->angVel -= dL * A->invI;
        B->angVel += dL * B->invI;
      }
      continue;  // weld has no limits or motor
    }

    if (j.rigid && !j.isRevolute) {
      // Rigid rod: keep anchor distance == restLength.
      Vec2 pa = A->pos + rA, pb = B->pos + rB;
      Vec2 d = pb - pa;
      float dist = length(d);
      if (dist > 1e-6f) {
        Vec2 n = d / dist;
        Vec2 dv = (B->vel + cross(B->angVel, rB)) - (A->vel + cross(A->angVel, rA));
        float vn = dot(dv, n);
        float rnA = cross(rA, n), rnB = cross(rB, n);
        float kk = A->invMass + B->invMass + rnA * rnA * A->invI + rnB * rnB * B->invI;
        if (kk > 1e-9f) {
          float dImp = -vn / kk;
          float old = j.distImpulse;
          j.distImpulse += dImp;  // no clamp: rigid rod pulls both ways
          float applied = j.distImpulse - old;
          Vec2 P = n * applied;
          A->vel -= P * A->invMass;
          A->angVel -= A->invI * cross(rA, P);
          B->vel += P * B->invMass;
          B->angVel += B->invI * cross(rB, P);
        }
      }
      continue;
    }

    // revolute motor
    if (j.isRevolute && j.enableMotor) {
      float k = A->invI + B->invI;
      if (k > 1e-9f && j.maxMotorTorque > 0.0f) {
        float dW = j.motorSpeed - (B->angVel - A->angVel);
        float dImp = dW / k;
        float maxImp = j.maxMotorTorque * dt;
        float old = j.motorImpulse;
        j.motorImpulse += dImp;
        if (j.motorImpulse > maxImp) j.motorImpulse = maxImp;
        if (j.motorImpulse < -maxImp) j.motorImpulse = -maxImp;
        float applied = j.motorImpulse - old;
        A->angVel -= applied * A->invI;
        B->angVel += applied * B->invI;
      }
    }
    // angle limits
    if (j.isRevolute && j.enableLimit) {
      float jointAngle = wrapAngle((B->angle - A->angle) - j.refAngle);  // N1
      float limitC = 0.0f;
      float sign = 0.0f;
      if (jointAngle < j.lower) { limitC = jointAngle - j.lower; sign = 1.0f; }
      else if (jointAngle > j.upper) { limitC = jointAngle - j.upper; sign = -1.0f; }
      if (sign != 0.0f) {
        float k = A->invI + B->invI;
        if (k > 1e-9f) {
          float dL = sign * (-limitC) / k * 8.0f;  // stiff correction
          // N2: cap the per-step kick. Without this a large violation
          // (teleport, one bad frame) produces a ~156 rad/s fling in one
          // step. Cap each body's angular-velocity change at 30 rad/s.
          float mi = A->invI > B->invI ? A->invI : B->invI;
          if (mi < 1e-9f) mi = 1e-9f;
          float maxDL = 30.0f / mi;
          if (dL > maxDL) dL = maxDL;
          else if (dL < -maxDL) dL = -maxDL;
          float old = j.limitImpulse;
          j.limitImpulse += dL;
          // clamp: limit impulse opposes violation; keep simple accumulation clamp
          if (sign > 0.0f && j.limitImpulse < 0.0f) j.limitImpulse = 0.0f;
          if (sign < 0.0f && j.limitImpulse > 0.0f) j.limitImpulse = 0.0f;
          float applied = j.limitImpulse - old;
          A->angVel -= applied * A->invI * sign;
          B->angVel += applied * B->invI * sign;
        }
      } else {
        j.limitImpulse = 0.0f;
      }
    }
  }
}

void World::solveJointsPosition() {
  for (auto& j : impl_->joints) {
    if (!j.alive) continue;
    Body* A = getBody(j.a);
    Body* B = getBody(j.b);
    if (!A || !B) continue;
    Vec2 rA = rotate(j.localA, A->angle);
    Vec2 rB = rotate(j.localB, B->angle);
    // Rigid distance rod: position correction to restLength.
    if (j.rigid && !j.isRevolute && !j.isWeld) {
      Vec2 pa = A->pos + rA, pb = B->pos + rB;
      Vec2 d = pb - pa;
      float dist = length(d);
      if (dist > 1e-6f) {
        Vec2 n = d / dist;
        float err = dist - j.restLength;
        float totalInv = A->invMass + B->invMass;
        if (totalInv > 1e-9f) {
          Vec2 corr = n * (err * 0.5f / totalInv);
          A->pos += corr * A->invMass;
          B->pos -= corr * B->invMass;
        }
      }
      continue;
    }
    if (!j.isRevolute && !j.isWeld) continue;
    Vec2 pa = A->pos + rA, pb = B->pos + rB;
    Vec2 err = pb - pa;
    float totalInv = A->invMass + B->invMass;
    if (totalInv < 1e-9f) continue;
    Vec2 corr = err * (0.5f / totalInv);
    A->pos += corr * A->invMass;
    B->pos -= corr * B->invMass;
    // angular correction: limits for revolute, full lock for weld
    {
      float jointAngle = wrapAngle((B->angle - A->angle) - j.refAngle);  // N1
      float k = A->invI + B->invI;
      if (k > 1e-9f) {
        float corrA = 0.0f;
        if (j.isWeld) {
          corrA = -jointAngle * 0.5f;
        } else if (j.enableLimit) {
          if (jointAngle < j.lower) corrA = (j.lower - jointAngle) * 0.5f;
          else if (jointAngle > j.upper) corrA = (j.upper - jointAngle) * 0.5f;
        }
        A->angle -= corrA * (A->invI / k);
        B->angle += corrA * (B->invI / k);
      }
    }
  }
  for (auto& b : impl_->bodies) if (b.alive) computeAABB(b);
}

void World::step(float dt, int velIters, int posIters) {
  if (dt <= 0.0f) return;
  if (dt > 1.0f / 20.0f) dt = 1.0f / 20.0f;
  // distance joint springs (explicit force; rigid rods are solved as constraints)
  for (auto& j : impl_->joints) {
    if (!j.alive || j.isRevolute || j.isWeld || j.rigid) continue;
    Body* A = getBody(j.a);
    Body* B = getBody(j.b);
    if (!A || !B) continue;
    Vec2 pa = A->pos + rotate(j.localA, A->angle);
    Vec2 pb = B->pos + rotate(j.localB, B->angle);
    Vec2 d = pb - pa;
    float dist = length(d);
    if (dist < 1e-6f) continue;
    Vec2 n = d / dist;
    Vec2 rv = (B->vel + cross(B->angVel, rotate(j.localB, B->angle)))
            - (A->vel + cross(A->angVel, rotate(j.localA, A->angle)));
    float f = j.stiffness * (dist - j.restLength) + j.damping * dot(rv, n);
    Vec2 imp = n * (f * dt);
    A->vel += imp * A->invMass;
    B->vel -= imp * B->invMass;
  }
  integrate(dt);
  collide();
  // Restitution bias: computed ONCE per step from the pre-solve approach
  // velocity. (Applying (1+e)*vn every iteration lets later iterations see
  // the separating velocity and undo the bounce.)
  const float restThreshold = 60.0f;
  for (auto& c : impl_->contacts) {
    Body* A = getBody(c.a);
    Body* B = getBody(c.b);
    if (!A || !B) continue;
    float e = A->restitution > B->restitution ? A->restitution : B->restitution;
    for (int p = 0; p < c.pointCount; ++p) {
      Vec2 rA = c.points[p] - A->pos, rB = c.points[p] - B->pos;
      Vec2 dv = (B->vel + cross(B->angVel, rB)) - (A->vel + cross(A->angVel, rA));
      float vn = dot(dv, c.normal);
      c.restBias[p] = (vn < -restThreshold) ? -e * vn : 0.0f;
    }
  }
  // reset joint accumulators each step (no warm starting across steps)
  for (auto& j : impl_->joints) if (j.alive) {
    j.pImpulse = Vec2(0,0); j.limitImpulse = 0.0f; j.angImpulse = 0.0f;
    j.motorImpulse = 0.0f; j.distImpulse = 0.0f;
  }
  for (int i = 0; i < velIters; ++i) {
    solveJointsVelocity(dt);
    solveContactsVelocity(dt);
  }
  // impact reports for gameplay (damage, sound)
  if (impl_->listener) {
    for (auto& c : impl_->contacts) {
      float imp = 0.0f;
      Vec2 pt{0, 0};
      for (int p = 0; p < c.pointCount; ++p) {
        imp += c.normalImpulse[p];
        pt += c.points[p];
      }
      if (c.pointCount > 0) pt = pt / (float)c.pointCount;
      if (imp > 0.0f)
        impl_->listener->postSolve(c.a, c.b, pt, c.normal, imp);
    }
  }
  for (int i = 0; i < posIters; ++i) {
    solveJointsPosition();
    solveContactsPosition();
  }
}

// ---- Queries ---------------------------------------------------------------

static bool rayCircle(Vec2 p0, Vec2 d, float maxT, const Body& b, float& t, Vec2& n) {
  Vec2 m = p0 - b.pos;
  float bb = dot(m, d);
  float cc = dot(m, m) - b.radius * b.radius;
  if (cc <= 0.0f) { t = 0.0f; n = Vec2(-d.y, d.x); return true; }  // origin inside
  if (bb > 0.0f) return false;
  float disc = bb * bb - cc;
  if (disc < 0.0f) return false;
  t = -bb - std::sqrt(disc);
  if (t < 0.0f || t > maxT) return false;
  Vec2 hit = p0 + d * t;
  Vec2 nn = hit - b.pos;
  float l = length(nn);
  n = l > 1e-6f ? nn / l : Vec2(1, 0);
  return true;
}

static bool rayPolygon(Vec2 p0, Vec2 d, float maxT, const Body& b, float& t, Vec2& n) {
  Vec2 wv[8];
  int nv = worldVerts(b, wv);
  float bestT = maxT + 1.0f;
  Vec2 bestN{0, 0};
  bool hit = false;
  for (int i = 0; i < nv; ++i) {
    Vec2 a = wv[i], e = wv[(i + 1) % nv] - wv[i];
    Vec2 en = Vec2(e.y, -e.x);  // outward normal (CCW winding)
    float denom = dot(d, en);
    if (std::fabs(denom) < 1e-9f) continue;
    float tt = dot(a - p0, en) / denom;
    if (tt < 0.0f || tt > bestT) continue;
    Vec2 hp = p0 + d * tt;
    Vec2 ap = hp - a;
    float s = dot(ap, e) / (dot(e, e) + 1e-9f);
    if (s < 0.0f || s > 1.0f) continue;
    bestT = tt; bestN = en / (length(en) + 1e-9f); hit = true;
  }
  if (!hit || bestT > maxT) return false;
  t = bestT; n = bestN;
  return true;
}

bool World::raycast(Vec2 p0, Vec2 p1, RaycastHit* out) const {
  Vec2 d = p1 - p0;
  float maxT = length(d);
  if (maxT < 1e-9f) return false;
  Vec2 dn = d / maxT;
  bool any = false;
  RaycastHit best;
  best.fraction = 1.0f;
  for (auto& b : impl_->bodies) {
    if (!b.alive) continue;
    // AABB reject
    float tmin = 0.0f, tmax = maxT;
    bool ok = true;
    for (int ax = 0; ax < 2; ++ax) {
      float o = ax == 0 ? p0.x : p0.y;
      float dd = ax == 0 ? dn.x : dn.y;
      float mn = ax == 0 ? b.aabb.mn.x : b.aabb.mn.y;
      float mx = ax == 0 ? b.aabb.mx.x : b.aabb.mx.y;
      if (std::fabs(dd) < 1e-9f) {
        if (o < mn || o > mx) { ok = false; break; }
      } else {
        float t1 = (mn - o) / dd, t2 = (mx - o) / dd;
        if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) { ok = false; break; }
      }
    }
    if (!ok || tmin > maxT) continue;
    float t = 0.0f; Vec2 n{0, 0};
    bool h = (b.shape == ShapeType::Circle)
                 ? rayCircle(p0, dn, maxT, b, t, n)
                 : rayPolygon(p0, dn, maxT, b, t, n);
    if (h && t / maxT < best.fraction) {
      best.fraction = t / maxT;
      best.body = b.id;
      best.point = p0 + dn * t;
      best.normal = n;
      any = true;
    }
  }
  if (any && out) *out = best;
  return any;
}

void World::queryAABB(Vec2 mn, Vec2 mx, uint32_t* out, int* count, int max) const {
  queryAABB(mn, mx, out, count, max, 0);
}

// N4: offset lets callers page through all matches instead of silently
// dropping bodies past `max`.
void World::queryAABB(Vec2 mn, Vec2 mx, uint32_t* out, int* count, int max,
                      int offset) const {
  int n = 0;
  int skipped = 0;
  for (auto& b : impl_->bodies) {
    if (!b.alive) continue;
    if (b.aabb.mx.x < mn.x || b.aabb.mn.x > mx.x ||
        b.aabb.mx.y < mn.y || b.aabb.mn.y > mx.y)
      continue;
    if (skipped < offset) { ++skipped; continue; }
    if (n < max) {
      out[n++] = b.id;
    } else {
      break;  // page full; caller advances offset for the next page
    }
  }
  if (count) *count = n;
}

uint32_t World::pickBody(Vec2 p) const {
  for (auto& b : impl_->bodies) {
    if (!b.alive || b.isStatic) continue;
    if (p.x < b.aabb.mn.x || p.x > b.aabb.mx.x ||
        p.y < b.aabb.mn.y || p.y > b.aabb.mx.y)
      continue;
    if (b.shape == ShapeType::Circle) {
      if (lengthSq(p - b.pos) <= b.radius * b.radius) return b.id;
    } else {
      // point-in-convex-polygon
      Vec2 wv[8];
      int nv = worldVerts(b, wv);
      bool inside = true;
      for (int i = 0; i < nv; ++i) {
        if (dot(worldNormal(b, i), p - wv[i]) > 0.0f) { inside = false; break; }
      }
      if (inside) return b.id;
    }
  }
  return 0;
}

// ---- Accessors ---------------------------------------------------------------

int World::bodyCount() const {
  int n = 0;
  for (auto& b : impl_->bodies) if (b.alive) ++n;
  return n;
}
Vec2 World::getPosition(uint32_t id) const { const Body* b = getBody(id); return b ? b->pos : Vec2(); }
float World::getAngle(uint32_t id) const { const Body* b = getBody(id); return b ? b->angle : 0.0f; }
Vec2 World::getVelocity(uint32_t id) const { const Body* b = getBody(id); return b ? b->vel : Vec2(); }
float World::getAngularVelocity(uint32_t id) const { const Body* b = getBody(id); return b ? b->angVel : 0.0f; }
void World::setVelocity(uint32_t id, Vec2 v) { Body* b = getBody(id); if (b && !b->isStatic) b->vel = v; }
void World::setAngularVelocity(uint32_t id, float w) { Body* b = getBody(id); if (b && !b->isStatic) b->angVel = w; }
void World::setTransform(uint32_t id, Vec2 p, float angle) {
  Body* b = getBody(id);
  if (!b) return;
  b->pos = p; b->angle = angle;
  b->vel = Vec2(); b->angVel = 0.0f;
  computeAABB(*b);
}
void World::applyImpulse(uint32_t id, Vec2 impulse) {
  Body* b = getBody(id);
  if (b && !b->isStatic) b->vel += impulse * b->invMass;
}
void World::applyImpulseAt(uint32_t id, Vec2 impulse, Vec2 point) {
  Body* b = getBody(id);
  if (!b || b->isStatic) return;
  b->vel += impulse * b->invMass;
  b->angVel += b->invI * cross(point - b->pos, impulse);
}
void World::setStatic(uint32_t id, bool s) {
  Body* b = getBody(id);
  if (!b) return;
  if (b->isStatic == s) return;
  b->isStatic = s;
  if (s) { b->invMass = 0.0f; b->invI = 0.0f; b->vel = Vec2(); b->angVel = 0.0f; }
  else {
    b->invMass = b->mass > 1e-9f ? 1.0f / b->mass : 0.0f;
    b->invI = b->inertia > 1e-9f ? 1.0f / b->inertia : 0.0f;
  }
}
bool World::isStatic(uint32_t id) const { const Body* b = getBody(id); return b ? b->isStatic : true; }
void* World::getUserData(uint32_t id) const { const Body* b = getBody(id); return b ? b->userData : nullptr; }
void World::setUserData(uint32_t id, void* d) { Body* b = getBody(id); if (b) b->userData = d; }
float World::getMass(uint32_t id) const { const Body* b = getBody(id); return b ? b->mass : 0.0f; }
AABB World::getAABB(uint32_t id) const { const Body* b = getBody(id); return b ? b->aabb : AABB{Vec2(), Vec2()}; }
Vec2 World::getGravity() const { return gravity_; }
void World::setGravity(Vec2 g) { gravity_ = g; }

}  // namespace rs2d
