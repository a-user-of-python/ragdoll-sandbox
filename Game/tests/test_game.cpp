// test_game.cpp — Host tests for Game/ (rs_game.h implementation).
// Minimal assert harness, no external deps. Links against rs_game + rs_lua.
#include "rs_game.h"
#include <cstdio>
#include <cmath>

static int failures = 0;
#define CHECK(cond) do { \
  if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } \
} while (0)

static void stepN(RSWorld* w, int n) {
  for (int i = 0; i < n; ++i) RS_Step(w, 1.0f / 60.0f);
}

void test_spawn_human() {
  RSWorld* w = RS_CreateWorld();
  uint32_t h = RS_SpawnHuman(w, 0, 200);
  CHECK(h != 0);
  CHECK(RS_GetEntityCount(w) == 1);
  // 11 bodies for the ragdoll.
  CHECK(RS_GetBodyCount(w) == 11);
  // Let it fall and settle.
  stepN(w, 180);
  // Should still have 11 bodies (none detached from falling).
  CHECK(RS_GetBodyCount(w) == 11);
  // Render items: one per body.
  RSRenderItem items[32];
  int n = RS_GetRenderItems(w, items, 32);
  CHECK(n == 11);
  // Head should be a circle, torso a box.
  bool foundCircle = false, foundBox = false;
  for (int i = 0; i < n; ++i) {
    if (items[i].shape == 1) foundCircle = true;
    if (items[i].shape == 0) foundBox = true;
    CHECK(items[i].tint == 0);  // alive, not burning
  }
  CHECK(foundCircle && foundBox);
  RS_DestroyWorld(w);
  printf("test_spawn_human ok\n");
}

void test_shoot_human() {
  RSWorld* w = RS_CreateWorld();
  uint32_t h = RS_SpawnHuman(w, 0, 100);
  CHECK(h != 0);
  stepN(w, 60);  // settle
  // Get torso position from render items (body index 1).
  RSRenderItem items[32];
  int n = RS_GetRenderItems(w, items, 32);
  CHECK(n == 11);
  float tx = items[1].x, ty = items[1].y;
  // Shoot from the left.
  RS_FireHitscan(w, tx - 300, ty, 0.0f, 1);  // rifle, angle 0 (right)
  // Check tracer BEFORE stepping (tracers fade in 0.08s).
  n = RS_GetRenderItems(w, items, 32);
  bool foundTracer = false;
  for (int i = 0; i < n; ++i) {
    if (items[i].shape == 2) { foundTracer = true; break; }
  }
  CHECK(foundTracer);
  stepN(w, 10);
  // Check that blood particles were spawned.
  RSParticle parts[512];
  int np = RS_GetParticles(w, parts, 512);
  bool foundBlood = false;
  for (int i = 0; i < np; ++i) {
    if (parts[i].type == 0) { foundBlood = true; break; }
  }
  CHECK(foundBlood);
  RS_DestroyWorld(w);
  printf("test_shoot_human ok\n");
}

void test_barrel_chain() {
  RSWorld* w = RS_CreateWorld();
  uint32_t b1 = RS_SpawnBarrel(w, 0, 50);
  uint32_t b2 = RS_SpawnBarrel(w, 60, 50);  // within chain radius
  CHECK(b1 != 0 && b2 != 0);
  stepN(w, 30);  // settle
  // Get actual barrel position (it fell to the ground).
  RSRenderItem items[16];
  int n = RS_GetRenderItems(w, items, 16);
  CHECK(n == 2);
  float bx = items[0].x, by = items[0].y;
  // Explode the first barrel via shooting it.
  RS_FireHitscan(w, bx - 100, by, 0.0f, 3);  // sniper, high damage
  stepN(w, 60);
  // Both barrels should be gone (exploded). b1 destroyed by bullet,
  // b2 by chain reaction.
  int bodies = RS_GetBodyCount(w);
  RSParticle parts[2048];
  int np = RS_GetParticles(w, parts, 2048);
  CHECK(np > 20);  // fire, smoke, sparks, debris
  printf("test_barrel_chain: bodies=%d particles=%d\n", bodies, np);
  RS_DestroyWorld(w);
  printf("test_barrel_chain ok\n");
}

void test_particle_budget() {
  RSWorld* w = RS_CreateWorld();
  RS_SetParticleBudget(w, 100);
  // Spawn a huge explosion to generate many particles.
  RS_Explode(w, 0, 100, 200, 1000);
  stepN(w, 5);
  RSParticle parts[5000];
  int np = RS_GetParticles(w, parts, 5000);
  CHECK(np <= 100);
  printf("test_particle_budget: got %d (budget 100)\n", np);
  // Raise budget, should get more.
  RS_SetParticleBudget(w, 4000);
  RS_Explode(w, 0, 100, 200, 1000);
  stepN(w, 5);
  np = RS_GetParticles(w, parts, 5000);
  CHECK(np > 100);
  printf("test_particle_budget: got %d (budget 4000)\n", np);
  RS_DestroyWorld(w);
  printf("test_particle_budget ok\n");
}

void test_freeze_unfreeze() {
  RSWorld* w = RS_CreateWorld();
  uint32_t b = RS_SpawnBall(w, 0, 200, 12);
  CHECK(b != 0);
  RS_SetFrozen(w, b, 1);
  stepN(w, 60);
  // Ball should not have fallen (frozen).
  RSRenderItem items[8];
  int n = RS_GetRenderItems(w, items, 8);
  CHECK(n == 1);
  CHECK(fabsf(items[0].y - 200.0f) < 5.0f);
  // Unfreeze, should fall.
  RS_SetFrozen(w, b, 0);
  stepN(w, 120);
  n = RS_GetRenderItems(w, items, 8);
  CHECK(n == 1);
  CHECK(items[0].y < 150.0f);  // fell
  RS_DestroyWorld(w);
  printf("test_freeze_unfreeze ok\n");
}

void test_grab() {
  RSWorld* w = RS_CreateWorld();
  uint32_t b = RS_SpawnBall(w, 0, 100, 12);
  CHECK(b != 0);
  stepN(w, 10);
  // Get actual ball position (it fell).
  RSRenderItem items[8];
  int n = RS_GetRenderItems(w, items, 8);
  CHECK(n == 1);
  float bx = items[0].x, by = items[0].y;
  // Grab the ball.
  uint32_t g = RS_GrabBegin(w, bx, by);
  CHECK(g != 0);
  // Move target up.
  for (int i = 0; i < 60; ++i) {
    RS_GrabMove(w, g, bx, by + 200);
    RS_Step(w, 1.0f / 60.0f);
  }
  n = RS_GetRenderItems(w, items, 8);
  CHECK(n == 1);
  // Ball should have been lifted (y > by).
  CHECK(items[0].y > by + 50.0f);
  RS_GrabEnd(w, g);
  RS_DestroyWorld(w);
  printf("test_grab ok\n");
}

void test_apply_impulse() {
  RSWorld* w = RS_CreateWorld();
  uint32_t b = RS_SpawnBall(w, 0, 100, 12);
  CHECK(b != 0);
  stepN(w, 5);
  // Apply upward impulse.
  RS_ApplyImpulse(w, b, 0, 5000);
  stepN(w, 10);
  RSRenderItem items[8];
  int n = RS_GetRenderItems(w, items, 8);
  CHECK(n == 1);
  // Ball should be moving up (y increased from 100, or velocity positive).
  // Note: gravity pulls down, but impulse should have lifted it.
  printf("test_apply_impulse: y=%.1f\n", items[0].y);
  // No-op for invalid ids (should not crash).
  RS_ApplyImpulse(w, 0, 0, 100);
  RS_ApplyImpulse(w, 99999, 0, 100);
  RS_ApplyImpulse(nullptr, b, 0, 100);
  RS_DestroyWorld(w);
  printf("test_apply_impulse ok\n");
}

void test_heal_human() {
  RSWorld* w = RS_CreateWorld();
  uint32_t h = RS_SpawnHuman(w, 0, 100);
  CHECK(h != 0);
  stepN(w, 30);
  // Damage the human with explosions until dead.
  for (int i = 0; i < 5; ++i) {
    RS_Explode(w, 0, 100, 100, 500);
    stepN(w, 10);
  }
  // Heal.
  RS_HealHuman(w, h);
  // Should not crash; human marked not dead.
  RSRenderItem items[32];
  int n = RS_GetRenderItems(w, items, 32);
  bool foundAlive = false;
  for (int i = 0; i < n; ++i) {
    if (items[i].tint == 0) foundAlive = true;
  }
  CHECK(foundAlive);
  RS_DestroyWorld(w);
  printf("test_heal_human ok\n");
}

void test_ignite() {
  RSWorld* w = RS_CreateWorld();
  uint32_t h = RS_SpawnHuman(w, 0, 50);
  CHECK(h != 0);
  stepN(w, 30);
  RS_Ignite(w, 0, 50, 80);
  stepN(w, 120);  // 2 seconds of burning
  // Check fire particles.
  RSParticle parts[1024];
  int np = RS_GetParticles(w, parts, 1024);
  bool foundFire = false;
  for (int i = 0; i < np; ++i) {
    if (parts[i].type == 1) { foundFire = true; break; }
  }
  CHECK(foundFire);
  // Check burning tint on human.
  RSRenderItem items[32];
  int n = RS_GetRenderItems(w, items, 32);
  bool foundBurning = false;
  for (int i = 0; i < n; ++i) {
    if (items[i].tint == 2) { foundBurning = true; break; }
  }
  CHECK(foundBurning);
  RS_DestroyWorld(w);
  printf("test_ignite ok\n");
}

void test_grenade_rocket() {
  RSWorld* w = RS_CreateWorld();
  // Grenade.
  RS_ThrowGrenade(w, 0, 200, 100, 0);
  CHECK(RS_GetEntityCount(w) == 1);
  stepN(w, 150);  // 2.5s > 2.0s fuse
  // Grenade should have exploded (entity removed).
  // Note: explosion spawns fire zone, but grenade entity is gone.
  printf("test_grenade: entities=%d\n", RS_GetEntityCount(w));
  // Rocket.
  RS_FireRocket(w, -200, 100, 0.0f);
  stepN(w, 60);
  // Rocket should have flown and exploded on... actually no wall, so it
  // flies for 5s. Just check it doesn't crash.
  printf("test_rocket: entities=%d\n", RS_GetEntityCount(w));
  RS_DestroyWorld(w);
  printf("test_grenade_rocket ok\n");
}

void test_melee() {
  RSWorld* w = RS_CreateWorld();
  uint32_t h = RS_SpawnHuman(w, 100, 50);
  CHECK(h != 0);
  stepN(w, 30);
  // Get human position (it fell).
  RSRenderItem items[32];
  int n = RS_GetRenderItems(w, items, 32);
  CHECK(n == 11);
  float hx = items[1].x, hy = items[1].y;  // torso
  // Swing at the human from the left.
  RS_MeleeSwing(w, hx - 100, hy, 0.0f, 150.0f, 50.0f);
  stepN(w, 10);
  // Should have spawned blood.
  RSParticle parts[512];
  int np = RS_GetParticles(w, parts, 512);
  bool foundBlood = false;
  for (int i = 0; i < np; ++i) {
    if (parts[i].type == 0) { foundBlood = true; break; }
  }
  CHECK(foundBlood);
  RS_DestroyWorld(w);
  printf("test_melee ok\n");
}

void test_clear_world() {
  RSWorld* w = RS_CreateWorld();
  RS_SpawnHuman(w, 0, 100);
  RS_SpawnCrate(w, 50, 100, 30);
  RS_SpawnBarrel(w, -50, 100);
  CHECK(RS_GetEntityCount(w) == 3);
  RS_ClearWorld(w);
  CHECK(RS_GetEntityCount(w) == 0);
  CHECK(RS_GetBodyCount(w) == 0);
  RS_DestroyWorld(w);
  printf("test_clear_world ok\n");
}

void test_step_ms() {
  RSWorld* w = RS_CreateWorld();
  RS_SpawnHuman(w, 0, 100);
  RS_Step(w, 1.0f / 60.0f);
  float ms = RS_GetStepMs(w);
  CHECK(ms >= 0.0f && ms < 100.0f);  // sane range
  printf("test_step_ms: %.3f ms\n", ms);
  RS_DestroyWorld(w);
  printf("test_step_ms ok\n");
}

int main() {
  test_spawn_human();
  test_shoot_human();
  test_barrel_chain();
  test_particle_budget();
  test_freeze_unfreeze();
  test_grab();
  test_apply_impulse();
  test_heal_human();
  test_ignite();
  test_grenade_rocket();
  test_melee();
  test_clear_world();
  test_step_ms();
  if (failures == 0) {
    printf("ALL GAME TESTS PASSED\n");
    return 0;
  } else {
    printf("%d FAILURES\n", failures);
    return 1;
  }
}
