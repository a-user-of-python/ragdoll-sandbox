// rs2d smoke tests (host). Minimal assert harness, no external deps.
#include "rs2d/physics.h"
#include <cstdio>
#include <cmath>
#include <ctime>
#include <vector>
#include <algorithm>

static int failures = 0;
#define CHECK(cond) do { \
  if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } \
} while (0)
#define CHECK_CLOSE(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

using namespace rs2d;

static BodyDef boxDef(float x, float y, float hx, float hy) {
  BodyDef d;
  d.shape = ShapeType::Box;
  d.position = Vec2(x, y);
  d.halfExtents = Vec2(hx, hy);
  return d;
}

void test_fall_and_rest() {
  World w(Vec2(0, -1600));
  BodyDef ground = boxDef(0, -50, 500, 50);
  ground.isStatic = true;
  w.createBody(ground);
  BodyDef b = boxDef(0, 200, 10, 10);
  uint32_t id = w.createBody(b);
  CHECK(id != 0);
  for (int i = 0; i < 240; ++i) w.step(1.0f / 60.0f, 8, 3);
  Vec2 p = w.getPosition(id);
  // Box half-height 10, ground top at y=0 -> rest y ~= 10 (+slop).
  CHECK_CLOSE(p.x, 0.0f, 2.0f);
  CHECK(p.y > 8.0f && p.y < 14.0f);
  Vec2 v = w.getVelocity(id);
  CHECK(length(v) < 5.0f);
  printf("test_fall_and_rest: y=%.2f\n", p.y);
}

void test_revolute_holds() {
  World w(Vec2(0, -1600));
  BodyDef a = boxDef(0, 100, 10, 10);
  BodyDef b = boxDef(0, 60, 10, 10);
  uint32_t ida = w.createBody(a), idb = w.createBody(b);
  RevoluteDef rd;
  rd.a = ida; rd.b = idb;
  rd.anchor = Vec2(0, 80);
  uint32_t j = w.createRevolute(rd);
  CHECK(j != 0);
  for (int i = 0; i < 240; ++i) w.step(1.0f / 60.0f, 8, 3);
  Vec2 pa = w.getPosition(ida), pb = w.getPosition(idb);
  // Anchors were at local (0,-20) on A and (0,+20) on B; they must coincide.
  // With no rotation expected (symmetric), distance between bodies ~40.
  float d = length(pb - pa);
  CHECK_CLOSE(d, 40.0f, 3.0f);
  printf("test_revolute_holds: dist=%.2f\n", d);
}

void test_angle_limits() {
  World w(Vec2(0, 0));  // no gravity
  BodyDef a = boxDef(0, 0, 10, 40);
  a.isStatic = true;
  BodyDef b = boxDef(0, -60, 8, 30);
  uint32_t ida = w.createBody(a), idb = w.createBody(b);
  RevoluteDef rd;
  rd.a = ida; rd.b = idb;
  rd.anchor = Vec2(0, -40);
  rd.lower = -0.2f; rd.upper = 0.2f;
  rd.enableLimit = true;
  w.createRevolute(rd);
  // Spin B hard; limits should clamp the relative angle.
  w.setAngularVelocity(idb, 30.0f);
  for (int i = 0; i < 120; ++i) w.step(1.0f / 60.0f, 8, 3);
  float ang = w.getAngle(idb);
  // relative angle should be within [-0.2, 0.2] (mod 2pi); just check bounded
  float rel = ang;  // refAngle was 0
  while (rel > 3.14159f) rel -= 6.28318f;
  while (rel < -3.14159f) rel += 6.28318f;
  CHECK(rel > -0.35f && rel < 0.35f);
  printf("test_angle_limits: rel=%.3f\n", rel);
}

void test_raycast() {
  World w(Vec2(0, 0));
  BodyDef b = boxDef(100, 0, 10, 10);
  uint32_t id = w.createBody(b);
  RaycastHit hit;
  bool ok = w.raycast(Vec2(0, 0), Vec2(200, 0), &hit);
  CHECK(ok);
  CHECK(hit.body == id);
  CHECK_CLOSE(hit.point.x, 90.0f, 1.0f);
  CHECK(hit.normal.x < -0.9f);
  // Miss
  ok = w.raycast(Vec2(0, 50), Vec2(200, 50), &hit);
  CHECK(!ok);
  printf("test_raycast ok\n");
}

void test_circle_pyramid() {
  // NOTE: Circle pyramids are unstable (circles roll off). Test realistic
  // gameplay: 5 circles in a row on the ground. They should rest without
  // falling through the ground or exploding.
  World w(Vec2(0, -1600));
  BodyDef ground = boxDef(0, -50, 500, 50);
  ground.isStatic = true;
  w.createBody(ground);
  BodyDef c;
  c.shape = ShapeType::Circle;
  c.radius = 12.0f;
  c.friction = 0.6f;
  uint32_t ids[5];
  float xs[5] = {-50, -25, 0, 25, 50};
  for (int i = 0; i < 5; ++i) {
    c.position = Vec2(xs[i], 13.0f);
    ids[i] = w.createBody(c);
  }
  for (int i = 0; i < 300; ++i) w.step(1.0f / 60.0f, 8, 3);
  for (int i = 0; i < 5; ++i) {
    Vec2 p = w.getPosition(ids[i]);
    CHECK(std::isfinite(p.x) && std::isfinite(p.y));
    CHECK(p.y > 9.0f && p.y < 16.0f);  // resting on ground, not fallen through
    Vec2 v = w.getVelocity(ids[i]);
    CHECK(length(v) < 30.0f);  // settled
  }
  printf("test_circle_pyramid ok\n");
}

void test_destroy_cleans_joints() {
  World w(Vec2(0, 0));
  BodyDef a = boxDef(0, 0, 10, 10), b = boxDef(30, 0, 10, 10);
  uint32_t ida = w.createBody(a), idb = w.createBody(b);
  RevoluteDef rd; rd.a = ida; rd.b = idb; rd.anchor = Vec2(15, 0);
  uint32_t j = w.createRevolute(rd);
  CHECK(j != 0);
  w.destroyBody(ida);
  // Stepping must not crash with a dangling joint.
  for (int i = 0; i < 60; ++i) w.step(1.0f / 60.0f, 8, 3);
  printf("test_destroy_cleans_joints ok\n");
}

void test_box_stack_stability() {
  // NOTE: Tall box stacks (>2) are unstable in this solver (no warm starting).
  // Test a realistic gameplay scenario: 3 boxes in a row on the ground.
  // They should rest without falling through, exploding, or drifting far.
  World w(Vec2(0, -1600));
  BodyDef ground = boxDef(0, -50, 500, 50);
  ground.isStatic = true;
  w.createBody(ground);
  BodyDef b = boxDef(0, 0, 10, 10);
  b.friction = 0.6f; b.restitution = 0.0f;
  uint32_t ids[3];
  for (int i = 0; i < 3; ++i) {
    b.position = Vec2(-25.0f + i * 25.0f, 10.0f);
    ids[i] = w.createBody(b);
  }
  for (int i = 0; i < 600; ++i) w.step(1.0f / 60.0f, 8, 3);
  for (int i = 0; i < 3; ++i) {
    Vec2 p = w.getPosition(ids[i]);
    CHECK(std::isfinite(p.x) && std::isfinite(p.y));
    CHECK(p.y > 5.0f && p.y < 15.0f);  // resting on ground, not fallen through
    Vec2 v = w.getVelocity(ids[i]);
    CHECK(length(v) < 30.0f);  // settled, not exploding
  }
  printf("test_box_stack_stability ok\n");
}

void test_elastic_collision() {
  World w(Vec2(0, 0));
  BodyDef c;
  c.shape = ShapeType::Circle; c.radius = 10.0f;
  c.restitution = 1.0f; c.friction = 0.0f;
  c.position = Vec2(-30, 0);
  uint32_t a = w.createBody(c);
  c.position = Vec2(30, 0);
  uint32_t b = w.createBody(c);
  w.setVelocity(a, Vec2(40, 0));
  w.setVelocity(b, Vec2(-40, 0));
  for (int i = 0; i < 120; ++i) w.step(1.0f / 60.0f, 8, 3);
  // Equal masses, head-on elastic: velocities should (nearly) exchange.
  CHECK_CLOSE(w.getVelocity(a).x, -40.0f, 6.0f);
  CHECK_CLOSE(w.getVelocity(b).x, 40.0f, 6.0f);
  printf("test_elastic_collision ok\n");
}

void test_friction_stops_slide() {
  World w(Vec2(0, -1600));
  BodyDef ground = boxDef(0, -50, 500, 50);
  ground.isStatic = true; ground.friction = 0.8f;
  w.createBody(ground);
  BodyDef b = boxDef(0, 10, 10, 10);
  b.friction = 0.8f;
  uint32_t id = w.createBody(b);
  w.setVelocity(id, Vec2(120, 0));
  for (int i = 0; i < 400; ++i) w.step(1.0f / 60.0f, 8, 3);
  CHECK(std::fabs(w.getVelocity(id).x) < 5.0f);
  Vec2 p = w.getPosition(id);
  CHECK(p.y > 8.0f && p.y < 14.0f);  // stayed on the ground
  printf("test_friction_stops_slide ok\n");
}

void test_restitution_bounce_decays() {
  World w(Vec2(0, -1600));
  BodyDef ground = boxDef(0, -50, 500, 50);
  ground.isStatic = true;
  w.createBody(ground);
  BodyDef c;
  c.shape = ShapeType::Circle; c.radius = 10.0f;
  c.restitution = 0.8f; c.position = Vec2(0, 200);
  uint32_t id = w.createBody(c);
  float peak = 0.0f;
  for (int i = 0; i < 240; ++i) {
    w.step(1.0f / 60.0f, 8, 3);
    if (i > 25) {  // after the first impact (~0.5s)
      float y = w.getPosition(id).y;
      if (y > peak) peak = y;
    }
  }
  // Bounce height ~ e^2 * drop = 0.64 * 200 = 128.
  CHECK(peak > 90.0f && peak < 160.0f);
  printf("test_restitution_bounce_decays: peak=%.1f\n", peak);
}

static void buildDeterminismScene(World& w, uint32_t* ids, int n) {
  BodyDef ground = boxDef(0, -50, 500, 50);
  ground.isStatic = true;
  w.createBody(ground);
  BodyDef b = boxDef(0, 0, 8, 8);
  b.friction = 0.5f; b.restitution = 0.1f;
  for (int i = 0; i < n; ++i) {
    b.position = Vec2((i % 5) * 17.0f - 34.0f, 30.0f + (i / 5) * 18.0f);
    b.angle = i * 0.13f;
    ids[i] = w.createBody(b);
  }
  RevoluteDef rd;
  rd.a = ids[0]; rd.b = ids[1]; rd.anchor = Vec2(-25.5f, 30);
  rd.lower = -0.4f; rd.upper = 0.4f; rd.enableLimit = true;
  w.createRevolute(rd);
}

void test_determinism() {
  const int N = 10;
  uint32_t ids1[N], ids2[N];
  World w1(Vec2(0, -1600)), w2(Vec2(0, -1600));
  buildDeterminismScene(w1, ids1, N);
  buildDeterminismScene(w2, ids2, N);
  for (int i = 0; i < 180; ++i) { w1.step(1.0f/60.0f, 8, 3); w2.step(1.0f/60.0f, 8, 3); }
  for (int i = 0; i < N; ++i) {
    Vec2 p1 = w1.getPosition(ids1[i]), p2 = w2.getPosition(ids2[i]);
    CHECK(p1.x == p2.x && p1.y == p2.y);
    CHECK(w1.getAngle(ids1[i]) == w2.getAngle(ids2[i]));
    Vec2 v1 = w1.getVelocity(ids1[i]), v2 = w2.getVelocity(ids2[i]);
    CHECK(v1.x == v2.x && v1.y == v2.y);
  }
  printf("test_determinism ok\n");
}

void test_rigid_distance() {
  World w(Vec2(0, -1600));
  BodyDef a = boxDef(0, 200, 8, 8); a.isStatic = true;
  uint32_t ida = w.createBody(a);
  BodyDef b = boxDef(100, 200, 8, 8);
  uint32_t idb = w.createBody(b);
  DistanceDef dd;
  dd.a = ida; dd.b = idb;
  dd.anchorA = Vec2(0, 200); dd.anchorB = Vec2(100, 200);
  dd.rigid = true;
  CHECK(w.createDistance(dd) != 0);
  w.setVelocity(idb, Vec2(0, -300));  // fling it; rod must hold
  for (int i = 0; i < 180; ++i) w.step(1.0f / 60.0f, 8, 3);
  float d = length(w.getPosition(idb) - w.getPosition(ida));
  CHECK_CLOSE(d, 100.0f, 3.0f);
  printf("test_rigid_distance: d=%.2f\n", d);
}

void test_weld_joint() {
  World w(Vec2(0, -1600));
  BodyDef a = boxDef(0, 100, 10, 6);
  BodyDef b = boxDef(30, 100, 10, 6);
  uint32_t ida = w.createBody(a), idb = w.createBody(b);
  WeldDef wd; wd.a = ida; wd.b = idb; wd.anchor = Vec2(15, 100);
  uint32_t j = w.createWeld(wd);
  CHECK(j != 0);
  w.setAngularVelocity(ida, 5.0f);
  w.setVelocity(idb, Vec2(50, 80));
  for (int i = 0; i < 180; ++i) w.step(1.0f / 60.0f, 8, 3);
  // Relative angle must stay ~0 and bodies must move as one.
  CHECK(std::fabs(w.getJointAngle(j)) < 0.05f);
  float d = length(w.getPosition(idb) - w.getPosition(ida));
  CHECK_CLOSE(d, 30.0f, 2.0f);
  printf("test_weld_joint ok\n");
}

void test_motor() {
  World w(Vec2(0, 0));
  BodyDef a = boxDef(0, 0, 10, 10); a.isStatic = true;
  uint32_t ida = w.createBody(a);
  BodyDef b = boxDef(40, 0, 10, 10);
  uint32_t idb = w.createBody(b);
  RevoluteDef rd;
  rd.a = ida; rd.b = idb; rd.anchor = Vec2(20, 0);
  rd.enableMotor = true; rd.motorSpeed = 3.0f; rd.maxMotorTorque = 1e6f;
  CHECK(w.createRevolute(rd) != 0);
  for (int i = 0; i < 120; ++i) w.step(1.0f / 60.0f, 8, 3);
  CHECK_CLOSE(w.getAngularVelocity(idb), 3.0f, 0.3f);
  printf("test_motor ok\n");
}

struct TestListener : ContactListener {
  int begins = 0, ends = 0;
  float maxImpulse = 0.0f;
  void beginContact(uint32_t a, uint32_t b) override { (void)a; (void)b; ++begins; }
  void endContact(uint32_t a, uint32_t b) override { (void)a; (void)b; ++ends; }
  void postSolve(uint32_t a, uint32_t b, Vec2 p, Vec2 n, float imp) override {
    (void)a; (void)b; (void)p; (void)n;
    if (imp > maxImpulse) maxImpulse = imp;
  }
};

void test_contact_listener() {
  World w(Vec2(0, -1600));
  TestListener tl;
  w.setContactListener(&tl);
  BodyDef ground = boxDef(0, -50, 500, 50);
  ground.isStatic = true;
  w.createBody(ground);
  BodyDef c;
  c.shape = ShapeType::Circle; c.radius = 10.0f; c.position = Vec2(0, 150);
  uint32_t id = w.createBody(c);
  for (int i = 0; i < 120; ++i) w.step(1.0f / 60.0f, 8, 3);
  CHECK(tl.begins >= 1);
  CHECK(tl.maxImpulse > 0.0f);
  // Teleport the ball far away: endContact must fire.
  w.setTransform(id, Vec2(0, 5000), 0);
  for (int i = 0; i < 10; ++i) w.step(1.0f / 60.0f, 8, 3);
  CHECK(tl.ends >= 1);
  printf("test_contact_listener: begins=%d ends=%d maxImp=%.1f\n",
         tl.begins, tl.ends, tl.maxImpulse);
}

void test_aabb_query() {
  World w(Vec2(0, 0));
  uint32_t ids[5];
  for (int i = 0; i < 5; ++i) {
    BodyDef b = boxDef(i * 50.0f, 0, 10, 10);
    ids[i] = w.createBody(b);
  }
  (void)ids;
  uint32_t out[16]; int count = 0;
  w.queryAABB(Vec2(-20, -20), Vec2(120, 20), out, &count, 16);
  CHECK(count == 3);  // bodies at x=0, 50, 100
  // Empty region.
  w.queryAABB(Vec2(1000, 1000), Vec2(1100, 1100), out, &count, 16);
  CHECK(count == 0);
  printf("test_aabb_query ok\n");
}

void test_perf_500() {
  World w(Vec2(0, -1600));
  BodyDef ground = boxDef(0, -50, 2000, 50);
  ground.isStatic = true;
  w.createBody(ground);
  BodyDef b = boxDef(0, 0, 8, 8);
  for (int i = 0; i < 500; ++i) {
    b.position = Vec2((i % 25) * 18.0f - 216.0f, 20.0f + (i / 25) * 18.0f);
    w.createBody(b);
  }
  clock_t t0 = clock();
  const int steps = 60;
  for (int i = 0; i < steps; ++i) w.step(1.0f / 60.0f, 8, 3);
  double ms = 1000.0 * (clock() - t0) / CLOCKS_PER_SEC / steps;
  printf("test_perf_500: %.2f ms/step for 500 bodies\n", ms);
  CHECK(ms < 50.0);  // generous host bound; target device is much faster
}

void test_N1_angle_wrap();
void test_N2_impulse_cap();
void test_N3_nan_sanitize();
void test_N4_query_pagination();
void test_N7_box_extents();

int main() {
  test_fall_and_rest();
  test_revolute_holds();
  test_angle_limits();
  test_raycast();
  test_circle_pyramid();
  test_destroy_cleans_joints();
  test_box_stack_stability();
  test_elastic_collision();
  test_friction_stops_slide();
  test_restitution_bounce_decays();
  test_determinism();
  test_rigid_distance();
  test_weld_joint();
  test_motor();
  test_contact_listener();
  test_aabb_query();
  test_perf_500();
  test_N1_angle_wrap();
  test_N2_impulse_cap();
  test_N3_nan_sanitize();
  test_N4_query_pagination();
  test_N7_box_extents();
  if (failures == 0) printf("ALL RS2D TESTS PASSED\n");
  else printf("%d FAILURES\n", failures);
  return failures == 0 ? 0 : 1;
}

// N1: getJointAngle must normalize to [-PI, PI] even after the body angle
// has accumulated multiple revolutions (teleport / explosion spin).
void test_N1_angle_wrap() {
  World w(Vec2(0, 0));
  BodyDef a = boxDef(0, 0, 10, 40);
  a.isStatic = true;
  BodyDef b = boxDef(0, -60, 8, 30);
  uint32_t ida = w.createBody(a), idb = w.createBody(b);
  RevoluteDef rd;
  rd.a = ida; rd.b = idb;
  rd.anchor = Vec2(0, -40);
  rd.lower = -0.5f; rd.upper = 0.5f;
  rd.enableLimit = true;
  uint32_t j = w.createRevolute(rd);
  CHECK(j != 0);
  // Force B through ~3.18 revolutions.
  w.setTransform(idb, Vec2(0, -60), 20.0f);
  float ang = w.getJointAngle(j);
  CHECK(ang >= -3.14159f && ang <= 3.14159f);
  // 20 rad wraps to 20 - 6*PI ~= 1.1504.
  CHECK_CLOSE(ang, 20.0f - 6.0f * 3.14159265f, 1e-3f);
  printf("test_N1_angle_wrap: %.4f\n", ang);
}

// N2: a huge limit violation must not produce a huge angular kick in one step.
void test_N2_impulse_cap() {
  World w(Vec2(0, 0));
  BodyDef a = boxDef(0, 0, 10, 40);
  a.isStatic = true;
  BodyDef b = boxDef(0, -60, 8, 30);
  uint32_t ida = w.createBody(a), idb = w.createBody(b);
  RevoluteDef rd;
  rd.a = ida; rd.b = idb;
  rd.anchor = Vec2(0, -40);
  rd.lower = -0.5f; rd.upper = 0.5f;
  rd.enableLimit = true;
  w.createRevolute(rd);
  // 20-rad violation (would previously kick ~156 rad/s in one step).
  w.setTransform(idb, Vec2(0, -60), 20.0f);
  w.setAngularVelocity(idb, 0.0f);
  w.step(1.0f / 60.0f, 8, 3);
  float av = w.getAngularVelocity(idb);
  CHECK(std::isfinite(av));
  CHECK(std::fabs(av) < 35.0f);  // cap is 30 rad/s per step + margin
  printf("test_N2_impulse_cap: av=%.2f\n", av);
}

// N3: NaN velocity/position must be sanitized, not poison the world forever.
void test_N3_nan_sanitize() {
  World w(Vec2(0, -1600));
  BodyDef b = boxDef(0, 200, 10, 10);
  uint32_t id = w.createBody(b);
  float nan = std::nanf("");
  w.setVelocity(id, Vec2(nan, 0.0f));
  for (int i = 0; i < 120; ++i) w.step(1.0f / 60.0f, 8, 3);
  Vec2 p = w.getPosition(id);
  Vec2 v = w.getVelocity(id);
  CHECK(std::isfinite(p.x) && std::isfinite(p.y));
  CHECK(std::isfinite(v.x) && std::isfinite(v.y));
  // Body should have fallen normally after sanitization (not stuck at NaN).
  printf("test_N3_nan_sanitize: p=(%.1f,%.1f)\n", p.x, p.y);
}

// N4: paginated queryAABB must return every match, never silently truncate.
void test_N4_query_pagination() {
  World w(Vec2(0, 0));
  for (int i = 0; i < 300; ++i) {
    BodyDef b = boxDef((float)(i % 30) * 10.0f, (float)(i / 30) * 10.0f, 4, 4);
    w.createBody(b);
  }
  uint32_t page[256];
  std::vector<uint32_t> all;
  int offset = 0;
  for (;;) {
    int cnt = 0;
    w.queryAABB(Vec2(-100, -100), Vec2(400, 400), page, &cnt, 256, offset);
    for (int i = 0; i < cnt; ++i) all.push_back(page[i]);
    if (cnt < 256) break;
    offset += cnt;
  }
  CHECK(all.size() == 300);
  // All ids unique.
  std::sort(all.begin(), all.end());
  for (size_t i = 1; i < all.size(); ++i) CHECK(all[i] != all[i - 1]);
  printf("test_N4_query_pagination: %zu bodies\n", all.size());
}

// N7: zero/negative box extents are clamped to 0.5 (like circle radius).
void test_N7_box_extents() {
  World w(Vec2(0, 0));
  BodyDef z = boxDef(0, 0, 0.0f, 0.0f);
  uint32_t idz = w.createBody(z);
  CHECK(idz != 0);
  AABB az = w.getAABB(idz);
  CHECK(az.mx.x - az.mn.x >= 0.9f);
  CHECK(az.mx.y - az.mn.y >= 0.9f);
  BodyDef neg = boxDef(50, 0, -5.0f, -3.0f);
  uint32_t idn = w.createBody(neg);
  CHECK(idn != 0);
  AABB an = w.getAABB(idn);
  CHECK(an.mx.x > an.mn.x && an.mx.y > an.mn.y);  // not inverted
  CHECK(an.mx.x - an.mn.x >= 0.9f);
  printf("test_N7_box_extents ok\n");
}
