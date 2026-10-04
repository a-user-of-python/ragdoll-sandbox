// rs_game.cpp — Ragdoll Sandbox game logic.
//
// Implements Game/rs_game.h (except RS_RunLuaFile/RS_GetLuaError, which live
// in Scripting/lua_bindings.cpp). Uses Engine/rs2d for physics. Calls
// RS_LuaTick once per RS_Step via ../Scripting/lua_bindings.h.
//
// All original code. No assets, code, or files from any existing game.

#include "rs_game.h"
#include "../Scripting/lua_bindings.h"
#include "rs2d/physics.h"

#include <cmath>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <chrono>

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

enum class EntityType : uint8_t {
  Human, Crate, Barrel, Ball, Plank, Grenade, Rocket
};

// Limb indices for Human.
enum Limb {
  LIMB_HEAD = 0, LIMB_TORSO = 1, LIMB_PELVIS = 2,
  LIMB_UAL = 3, LIMB_FAL = 4, LIMB_UAR = 5, LIMB_FAR = 6,
  LIMB_THL = 7, LIMB_SHL = 8, LIMB_THR = 9, LIMB_SHR = 10,
  LIMB_COUNT = 11
};

struct JointRec {
  uint32_t jid;
  uint32_t bodyA;  // physics body ids (entity-local indices LIMB_*)
  uint32_t bodyB;
  int limbA;
  int limbB;
};

struct Entity {
  uint32_t id = 0;
  uint32_t seq = 0;              // creation sequence; higher = newer (M8 topmost)
  EntityType type = EntityType::Ball;
  std::vector<uint32_t> bodies;   // physics body ids, index = limb for Human
  std::vector<JointRec> joints;
  bool alive = true;
  float limbHP[LIMB_COUNT];
  bool limbAlive[LIMB_COUNT];
  bool dead = false;              // human death (torso+head destroyed)
  float hp = 0.0f;                // barrel / crate
  float fuse = 0.0f;              // grenade
  float life = 0.0f;              // rocket
  bool burning = false;
  float burnTime = 0.0f;
  bool frozen = false;
  float vx = 0.0f, vy = 0.0f;     // rocket velocity (kinematic)

  Entity() {
    for (int i = 0; i < LIMB_COUNT; ++i) {
      limbHP[i] = 100.0f;
      limbAlive[i] = true;
    }
  }
};

struct Particle {
  float x, y, vx, vy;
  float life, maxLife, size;
  float r, g, b, a;
  float a0 = 1.0f;   // initial alpha; fade scales this (M5)
  uint8_t type = 0;  // 0=blood 1=fire 2=smoke 3=spark 4=debris
  bool alive = false;
};

struct Tracer {
  float x0, y0, x1, y1;
  float r, g, b;
  float life;
};

struct FireZone {
  float x, y, radius;
  float life;
};

struct Grab {
  uint32_t handle = 0;
  uint32_t body = 0;
  float tx = 0, ty = 0;
  bool active = false;
};

// Forward declaration: implemented by Scripting/lua_bindings.cpp.
// Tears down the Lua state associated with a world. Called before the
// world is destroyed or fully reset.
void RS_ScriptingWorldDestroyed(RSWorld* w);

struct ExplosionJob {
  float x, y, radius, power;
};

struct RSWorld {
  rs2d::World physics;
  std::vector<Entity> entities;
  uint32_t nextEntityId = 1;
  uint32_t nextSeq = 1;              // M8: creation sequence for topmost queries
  std::vector<Particle> particles;
  int particleBudget = 4000;
  std::vector<Tracer> tracers;
  std::vector<FireZone> fires;
  std::vector<Grab> grabs;
  uint32_t nextGrabId = 1;
  uint32_t rngState = 0x12345678u;
  float stepAccum = 0.0f;
  float lastStepMs = 0.0f;
  int physicsSubsteps = 2;
  // M2: explicit explosion work queue (replaces recursion).
  std::vector<ExplosionJob> explosionQueue;
  bool processingExplosions = false;
  // M4: impact events queued by the contact listener, processed after step.
  struct ImpactEvent { uint32_t a, b; float px, py; float impulse; };
  std::vector<ImpactEvent> impacts;
  struct ImpactListener : public rs2d::ContactListener {
    RSWorld* w = nullptr;
    void postSolve(uint32_t a, uint32_t b, rs2d::Vec2 point,
                   rs2d::Vec2, float impulse) override {
      // Threshold ~400: resting contact is ~70-90/step; real impacts are 1000+.
      if (w && impulse > 400.0f)
        w->impacts.push_back({a, b, point.x, point.y, impulse});
    }
  };
  ImpactListener impactListener;

  RSWorld() : physics(rs2d::Vec2(0.0f, -1600.0f)) {
    particles.resize(4000);
    impactListener.w = this;
    physics.setContactListener(&impactListener);
  }
};

// ---------------------------------------------------------------------------
// RNG + particle helpers
// ---------------------------------------------------------------------------

static uint32_t xorshift(RSWorld* w) {
  uint32_t x = w->rngState;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  w->rngState = x ? x : 0x12345678u;
  return w->rngState;
}

static float randf(RSWorld* w, float lo, float hi) {
  float t = (xorshift(w) & 0xffffffu) / (float)0x1000000u;
  return lo + t * (hi - lo);
}

static void spawnParticle(RSWorld* w, float x, float y, float vx, float vy,
                          float life, float size,
                          float r, float g, float b, float a, uint8_t type) {
  int cap = w->particleBudget < (int)w->particles.size()
                ? w->particleBudget : (int)w->particles.size();
  if (cap <= 0) return;
  for (int i = 0; i < cap; ++i) {
    Particle& p = w->particles[i];
    if (!p.alive) {
      p.x = x; p.y = y; p.vx = vx; p.vy = vy;
      p.life = life; p.maxLife = life; p.size = size;
      p.r = r; p.g = g; p.b = b; p.a = a; p.a0 = a;
      p.type = type; p.alive = true;
      return;
    }
  }
  // Pool full: recycle a random slot so effects stay visible under load.
  Particle& p = w->particles[xorshift(w) % (uint32_t)cap];
  p.x = x; p.y = y; p.vx = vx; p.vy = vy;
  p.life = life; p.maxLife = life; p.size = size;
  p.r = r; p.g = g; p.b = b; p.a = a; p.a0 = a;
  p.type = type; p.alive = true;
}

static void bloodBurst(RSWorld* w, float x, float y, int n) {
  for (int i = 0; i < n; ++i) {
    float a = randf(w, 0, 6.28318f), sp = randf(w, 30, 220);
    spawnParticle(w, x, y, cosf(a) * sp, sinf(a) * sp,
                  randf(w, 0.4f, 1.0f), randf(w, 2.0f, 5.0f),
                  0.72f, 0.07f, 0.07f, 1.0f, 0);
  }
}

static void sparkBurst(RSWorld* w, float x, float y, int n,
                       float r, float g, float b) {
  for (int i = 0; i < n; ++i) {
    float a = randf(w, 0, 6.28318f), sp = randf(w, 100, 500);
    spawnParticle(w, x, y, cosf(a) * sp, sinf(a) * sp,
                  randf(w, 0.2f, 0.6f), randf(w, 1.5f, 3.5f),
                  r, g, b, 1.0f, 3);
  }
}

static void smokeBurst(RSWorld* w, float x, float y, int n) {
  for (int i = 0; i < n; ++i) {
    float a = randf(w, 0, 6.28318f), sp = randf(w, 20, 80);
    spawnParticle(w, x, y, cosf(a) * sp, sinf(a) * sp + 40,
                  randf(w, 1.0f, 2.5f), randf(w, 8.0f, 20.0f),
                  0.35f, 0.33f, 0.32f, 0.7f, 2);
  }
}

static void fireBurst(RSWorld* w, float x, float y, int n) {
  for (int i = 0; i < n; ++i) {
    float a = randf(w, 0, 6.28318f), sp = randf(w, 30, 120);
    spawnParticle(w, x + randf(w, -8, 8), y + randf(w, -8, 8),
                  cosf(a) * sp, sinf(a) * sp + 60,
                  randf(w, 0.3f, 0.8f), randf(w, 4.0f, 10.0f),
                  1.0f, randf(w, 0.3f, 0.6f), 0.1f, 1.0f, 1);
  }
}

static void debrisBurst(RSWorld* w, float x, float y, int n,
                        float r, float g, float b) {
  for (int i = 0; i < n; ++i) {
    float a = randf(w, 0, 6.28318f), sp = randf(w, 50, 300);
    spawnParticle(w, x, y, cosf(a) * sp, sinf(a) * sp,
                  randf(w, 0.8f, 2.0f), randf(w, 3.0f, 8.0f),
                  r, g, b, 1.0f, 4);
  }
}

// ---------------------------------------------------------------------------
// Entity lookup
// ---------------------------------------------------------------------------

static Entity* findEntity(RSWorld* w, uint32_t e) {
  if (e == 0) return nullptr;
  for (auto& en : w->entities)
    if (en.id == e && en.alive) return &en;
  return nullptr;
}

// Find entity containing physics body; limbOut gets limb/body index.
static Entity* findByBody(RSWorld* w, uint32_t body, int* limbOut) {
  for (auto& en : w->entities) {
    if (!en.alive) continue;
    for (size_t i = 0; i < en.bodies.size(); ++i) {
      if (en.bodies[i] == body) {
        if (limbOut) *limbOut = (int)i;
        return &en;
      }
    }
  }
  return nullptr;
}

static uint32_t allocEntityId(RSWorld* w) {
  // 0 is the "invalid entity" sentinel everywhere; never hand it out,
  // even on (practically unreachable) UINT32_MAX wraparound.
  uint32_t id = w->nextEntityId++;
  if (id == 0) id = w->nextEntityId++;
  return id;
}

// ---------------------------------------------------------------------------
// Damage + death
// ---------------------------------------------------------------------------

static void destroyJointsForLimb(RSWorld* w, Entity* en, int limb) {
  uint32_t body = en->bodies[limb];
  std::vector<JointRec> keep;
  for (auto& jr : en->joints) {
    if (jr.bodyA == body || jr.bodyB == body) {
      w->physics.destroyJoint(jr.jid);
    } else {
      keep.push_back(jr);
    }
  }
  en->joints.swap(keep);
}

static void damageEntity(RSWorld* w, Entity* en, int limb, float amount,
                         float hx, float hy);

static void explodeInternal(RSWorld* w, float x, float y, float radius,
                            float power);
static void drainExplosions(RSWorld* w);
static void releaseGrabsForBody(RSWorld* w, uint32_t body);

static float powerFromBarrel() { return 900.0f; }

static void killHuman(RSWorld* w, Entity* en) {
  if (en->dead) return;
  en->dead = true;
  rs2d::Vec2 p = w->physics.getPosition(en->bodies[LIMB_TORSO]);
  bloodBurst(w, p.x, p.y, 20);
}

static void damageEntity(RSWorld* w, Entity* en, int limb, float amount,
                         float hx, float hy) {
  if (!en || !en->alive || amount <= 0) return;
  switch (en->type) {
    case EntityType::Human: {
      if (limb < 0 || limb >= LIMB_COUNT) limb = LIMB_TORSO;
      if (!en->limbAlive[limb] || en->dead) {
        // Still spawn a little blood for hitting a corpse.
        if (en->dead) bloodBurst(w, hx, hy, 3);
        return;
      }
      en->limbHP[limb] -= amount;
      bloodBurst(w, hx, hy, (int)amount / 8 + 4);
      if (en->limbHP[limb] <= 0) {
        en->limbHP[limb] = 0;
        // Head or torso destroyed counts toward death; limbs detach.
        if (limb == LIMB_HEAD || limb == LIMB_TORSO) {
          en->limbAlive[limb] = false;
          destroyJointsForLimb(w, en, limb);
        } else {
          // Limb detaches: destroy its joints, it becomes a free body.
          destroyJointsForLimb(w, en, limb);
          en->limbAlive[limb] = false;
        }
        rs2d::Vec2 p = w->physics.getPosition(en->bodies[limb]);
        bloodBurst(w, p.x, p.y, 14);
      }
      // Death = torso + head both destroyed.
      if (!en->limbAlive[LIMB_TORSO] && !en->limbAlive[LIMB_HEAD])
        killHuman(w, en);
      break;
    }
    case EntityType::Barrel: {
      en->hp -= amount;
      sparkBurst(w, hx, hy, 4, 1.0f, 0.8f, 0.3f);
      if (en->hp <= 0 && en->alive) {
        rs2d::Vec2 p = w->physics.getPosition(en->bodies[0]);
        // Mark dead before exploding to avoid re-entrancy.
        en->alive = false;
        // M1: release grabs before the body is destroyed.
        releaseGrabsForBody(w, en->bodies[0]);
        // Remove the barrel body.
        w->physics.destroyBody(en->bodies[0]);
        en->bodies.clear();
        // M2: chain via the explosion work queue (iterative, no recursion).
        w->explosionQueue.push_back({p.x, p.y, 130.0f, powerFromBarrel()});
        drainExplosions(w);
      }
      break;
    }
    case EntityType::Crate: {
      en->hp -= amount;
      if (en->hp <= 0 && en->alive) {
        en->alive = false;
        rs2d::Vec2 p = w->physics.getPosition(en->bodies[0]);
        float ang = w->physics.getAngle(en->bodies[0]);
        (void)ang;
        debrisBurst(w, p.x, p.y, 14, 0.55f, 0.38f, 0.22f);
        smokeBurst(w, p.x, p.y, 4);
        // M1: release grabs before the body is destroyed.
        releaseGrabsForBody(w, en->bodies[0]);
        w->physics.destroyBody(en->bodies[0]);
        en->bodies.clear();
      }
      break;
    }
    default:
      // Balls, planks, grenades, rockets: no HP.
      break;
  }
  // Fire spread: damaging with heat can ignite.
  (void)hx; (void)hy;
}

// ---------------------------------------------------------------------------
// Explosion (M2: iterative work queue instead of recursion)
// ---------------------------------------------------------------------------

// One explosion's effects. May enqueue chained explosions via damageEntity.
static void explodeOnce(RSWorld* w, float x, float y, float radius,
                        float power) {
  // Particles.
  fireBurst(w, x, y, 24);
  smokeBurst(w, x, y, 12);
  sparkBurst(w, x, y, 20, 1.0f, 0.7f, 0.2f);
  debrisBurst(w, x, y, 10, 0.4f, 0.35f, 0.3f);

  // Radial impulse + damage via AABB query.
  // N4: page through ALL matches — the old single 256-cap query silently
  // dropped bodies in dense scenes (23+ humans), so far bodies got no
  // impulse and no damage.
  std::vector<uint32_t> hitBodies;
  {
    uint32_t page[256];
    int offset = 0;
    for (;;) {
      int cnt = 0;
      w->physics.queryAABB(rs2d::Vec2(x - radius, y - radius),
                           rs2d::Vec2(x + radius, y + radius),
                           page, &cnt, 256, offset);
      for (int i = 0; i < cnt; ++i) hitBodies.push_back(page[i]);
      if (cnt < 256) break;
      offset += cnt;
    }
  }
  // Collect (body, entity, limb) first — damage may destroy entities.
  struct Hit { uint32_t body; Entity* en; int limb; };
  std::vector<Hit> hits;
  for (size_t i = 0; i < hitBodies.size(); ++i) {
    int limb = 0;
    Entity* en = findByBody(w, hitBodies[i], &limb);
    if (en && en->alive) hits.push_back({hitBodies[i], en, limb});
  }
  for (size_t i = 0; i < hits.size(); ++i) {
    uint32_t b = hits[i].body;
    Entity* en = hits[i].en;
    int limb = hits[i].limb;
    if (!en->alive) continue;  // may have died from an earlier hit
    // Re-validate body still belongs (entity may have lost it).
    bool stillHas = false;
    for (uint32_t eb : en->bodies) if (eb == b) { stillHas = true; break; }
    if (!stillHas) continue;
    rs2d::Vec2 p = w->physics.getPosition(b);
    float dx = p.x - x, dy = p.y - y;
    float d = sqrtf(dx * dx + dy * dy);
    if (d > radius) continue;
    float fall = 1.0f - d / radius;  // 1 at center
    float imp = power * (0.3f + 0.7f * fall);
    rs2d::Vec2 dir = d > 1.0f ? rs2d::Vec2(dx / d, dy / d)
                              : rs2d::Vec2(0, 1);
    // Upward bias for a satisfying launch.
    dir.y += 0.35f;
    float dl = sqrtf(dir.x * dir.x + dir.y * dir.y);
    dir.x /= dl; dir.y /= dl;
    w->physics.applyImpulse(b, rs2d::Vec2(dir.x * imp, dir.y * imp));
    float dmg = 140.0f * fall + 20.0f * fall;
    damageEntity(w, en, limb, dmg, p.x, p.y);
  }
  // Chain-detonate nearby barrels (damageEntity handles it via hp<=0).
  // Ignite the area briefly.
  FireZone fz;
  fz.x = x; fz.y = y; fz.radius = radius * 0.5f; fz.life = 1.2f;
  w->fires.push_back(fz);
}

// Drain the explosion work queue iteratively. Re-entrant safe: if we're
// already draining (chained explosion), just return and let the outer
// loop pick up the new job.
static void drainExplosions(RSWorld* w) {
  if (w->processingExplosions) return;
  w->processingExplosions = true;
  while (!w->explosionQueue.empty()) {
    ExplosionJob job = w->explosionQueue.back();
    w->explosionQueue.pop_back();
    explodeOnce(w, job.x, job.y, job.radius, job.power);
  }
  w->processingExplosions = false;
}

static void explodeInternal(RSWorld* w, float x, float y, float radius,
                            float power) {
  w->explosionQueue.push_back({x, y, radius, power});
  drainExplosions(w);
}

// ---------------------------------------------------------------------------
// Spawning
// ---------------------------------------------------------------------------

static rs2d::BodyDef makeBodyDef() {
  rs2d::BodyDef d;
  d.friction = 0.5f;
  d.restitution = 0.05f;
  d.density = 0.002f;
  return d;
}

uint32_t RS_SpawnHuman(RSWorld* w, float x, float y) {
  if (!w) return 0;
  Entity en;
  en.id = allocEntityId(w);
  en.seq = w->nextSeq++;
  en.type = EntityType::Human;
  // HP per limb.
  for (int i = 0; i < LIMB_COUNT; ++i) en.limbHP[i] = 100.0f;
  en.limbHP[LIMB_HEAD] = 60.0f;
  en.limbHP[LIMB_TORSO] = 140.0f;

  struct LimbDef { float ox, oy, hw, hh, cr; bool circle; };
  // Offsets relative to spawn (x, y = pelvis center).
  const LimbDef defs[LIMB_COUNT] = {
    {0, 78, 0, 0, 14, true},     // HEAD (circle r=14)
    {0, 38, 18, 25, 0, false},   // TORSO (36x50)
    {0, 0, 16, 10, 0, false},    // PELVIS (32x20)
    {-25, 45, 6, 15, 0, false},  // UAL (12x30)
    {-25, 14, 5, 14, 0, false},  // FAL (10x28)
    {25, 45, 6, 15, 0, false},   // UAR
    {25, 14, 5, 14, 0, false},   // FAR
    {-10, -32, 7, 20, 0, false}, // THL (14x40)
    {-10, -70, 6, 19, 0, false}, // SHL (12x38)
    {10, -32, 7, 20, 0, false},  // THR
    {10, -70, 6, 19, 0, false},  // SHR
  };
  for (int i = 0; i < LIMB_COUNT; ++i) {
    rs2d::BodyDef d = makeBodyDef();
    d.position = rs2d::Vec2(x + defs[i].ox, y + defs[i].oy);
    if (defs[i].circle) {
      d.shape = rs2d::ShapeType::Circle;
      d.radius = defs[i].cr;
    } else {
      d.shape = rs2d::ShapeType::Box;
      d.halfExtents = rs2d::Vec2(defs[i].hw, defs[i].hh);
    }
    // Slightly lighter limbs for a floppy ragdoll feel.
    if (i != LIMB_TORSO && i != LIMB_PELVIS) d.density = 0.0015f;
    uint32_t b = w->physics.createBody(d);
    if (!b) { /* rollback */ for (uint32_t pb : en.bodies) w->physics.destroyBody(pb); return 0; }
    en.bodies.push_back(b);
  }

  // Revolute joints with angle limits.
  struct JDef { int a, b; float ax, ay; float lo, hi; };
  const JDef jdefs[] = {
    {LIMB_TORSO, LIMB_HEAD, 0, 62, -0.5f, 0.5f},     // neck
    {LIMB_TORSO, LIMB_PELVIS, 0, 14, -0.4f, 0.6f},   // waist
    {LIMB_TORSO, LIMB_UAL, -20, 55, -2.6f, 0.6f},    // shoulder L
    {LIMB_UAL, LIMB_FAL, -25, 30, -2.4f, 0.1f},      // elbow L
    {LIMB_TORSO, LIMB_UAR, 20, 55, -0.6f, 2.6f},     // shoulder R
    {LIMB_UAR, LIMB_FAR, 25, 30, -0.1f, 2.4f},       // elbow R
    {LIMB_PELVIS, LIMB_THL, -10, -12, -0.4f, 1.8f},  // hip L
    {LIMB_THL, LIMB_SHL, -10, -51, -0.1f, 2.2f},     // knee L
    {LIMB_PELVIS, LIMB_THR, 10, -12, -1.8f, 0.4f},   // hip R
    {LIMB_THR, LIMB_SHR, 10, -51, -2.2f, 0.1f},      // knee R
  };
  for (auto& jd : jdefs) {
    rs2d::RevoluteDef rd;
    rd.a = en.bodies[jd.a];
    rd.b = en.bodies[jd.b];
    rd.anchor = rs2d::Vec2(x + jd.ax, y + jd.ay);
    rd.lower = jd.lo; rd.upper = jd.hi;
    rd.enableLimit = true;
    uint32_t j = w->physics.createRevolute(rd);
    if (j) en.joints.push_back({j, en.bodies[jd.a], en.bodies[jd.b], jd.a, jd.b});
  }

  uint32_t id = en.id;
  w->entities.push_back(std::move(en));
  return id;
}

uint32_t RS_SpawnCrate(RSWorld* w, float x, float y, float size) {
  if (!w || size <= 0) return 0;
  Entity en;
  en.id = allocEntityId(w);
  en.seq = w->nextSeq++;
  en.type = EntityType::Crate;
  en.hp = 60.0f;
  rs2d::BodyDef d = makeBodyDef();
  d.shape = rs2d::ShapeType::Box;
  d.position = rs2d::Vec2(x, y);
  d.halfExtents = rs2d::Vec2(size * 0.5f, size * 0.5f);
  d.friction = 0.6f;
  uint32_t b = w->physics.createBody(d);
  if (!b) return 0;
  en.bodies.push_back(b);
  uint32_t id = en.id;
  w->entities.push_back(std::move(en));
  return id;
}

uint32_t RS_SpawnBarrel(RSWorld* w, float x, float y) {
  if (!w) return 0;
  Entity en;
  en.id = allocEntityId(w);
  en.seq = w->nextSeq++;
  en.type = EntityType::Barrel;
  en.hp = 30.0f;
  rs2d::BodyDef d = makeBodyDef();
  d.shape = rs2d::ShapeType::Circle;
  d.position = rs2d::Vec2(x, y);
  d.radius = 16.0f;
  d.friction = 0.4f;
  d.restitution = 0.1f;
  uint32_t b = w->physics.createBody(d);
  if (!b) return 0;
  en.bodies.push_back(b);
  uint32_t id = en.id;
  w->entities.push_back(std::move(en));
  return id;
}

uint32_t RS_SpawnBall(RSWorld* w, float x, float y, float r) {
  if (!w || r <= 0) return 0;
  Entity en;
  en.id = allocEntityId(w);
  en.seq = w->nextSeq++;
  en.type = EntityType::Ball;
  rs2d::BodyDef d = makeBodyDef();
  d.shape = rs2d::ShapeType::Circle;
  d.position = rs2d::Vec2(x, y);
  d.radius = r;
  d.friction = 0.3f;
  d.restitution = 0.4f;
  uint32_t b = w->physics.createBody(d);
  if (!b) return 0;
  en.bodies.push_back(b);
  uint32_t id = en.id;
  w->entities.push_back(std::move(en));
  return id;
}

uint32_t RS_SpawnPlank(RSWorld* w, float x, float y, float pw, float ph) {
  if (!w || pw <= 0 || ph <= 0) return 0;
  Entity en;
  en.id = allocEntityId(w);
  en.seq = w->nextSeq++;
  en.type = EntityType::Plank;
  rs2d::BodyDef d = makeBodyDef();
  d.shape = rs2d::ShapeType::Box;
  d.position = rs2d::Vec2(x, y);
  d.halfExtents = rs2d::Vec2(pw * 0.5f, ph * 0.5f);
  d.friction = 0.5f;
  uint32_t b = w->physics.createBody(d);
  if (!b) return 0;
  en.bodies.push_back(b);
  uint32_t id = en.id;
  w->entities.push_back(std::move(en));
  return id;
}

// M1: release any active grab on a body before the body is destroyed.
// Prevents stale grabs from latching onto a reused body id.
static void releaseGrabsForBody(RSWorld* w, uint32_t body) {
  for (auto& g : w->grabs) {
    if (g.active && g.body == body) { g.active = false; g.body = 0; }
  }
}

static void destroyEntityBodies(RSWorld* w, Entity& en) {
  for (uint32_t b : en.bodies) releaseGrabsForBody(w, b);
  for (auto& jr : en.joints) w->physics.destroyJoint(jr.jid);
  en.joints.clear();
  for (uint32_t b : en.bodies) w->physics.destroyBody(b);
  en.bodies.clear();
  en.alive = false;
}

// Erase entity by id (M3); no-op if not found. Bodies must already be
// destroyed (or pass destroyBodies=true).
static void eraseEntity(RSWorld* w, uint32_t e, bool destroyBodies) {
  for (size_t i = 0; i < w->entities.size(); ++i) {
    if (w->entities[i].id == e) {
      if (destroyBodies) destroyEntityBodies(w, w->entities[i]);
      w->entities[i] = std::move(w->entities.back());
      w->entities.pop_back();
      return;
    }
  }
}

// M3: sweep dead entities whose bodies are already gone (or destroy them).
static void sweepDeadEntities(RSWorld* w) {
  for (size_t i = 0; i < w->entities.size();) {
    if (!w->entities[i].alive) {
      // Safety: destroy any leftover bodies/joints.
      for (auto& jr : w->entities[i].joints) w->physics.destroyJoint(jr.jid);
      for (uint32_t b : w->entities[i].bodies) {
        releaseGrabsForBody(w, b);
        w->physics.destroyBody(b);
      }
      w->entities[i] = std::move(w->entities.back());
      w->entities.pop_back();
    } else {
      ++i;
    }
  }
}

void RS_Despawn(RSWorld* w, uint32_t e) {
  if (!w || e == 0) return;
  Entity* en = findEntity(w, e);
  if (!en) return;
  // destroyEntityBodies releases grabs; erase removes the record (M3).
  destroyEntityBodies(w, *en);
  eraseEntity(w, e, false);
}

void RS_DespawnAllHumans(RSWorld* w) {
  if (!w) return;
  // Collect ids first (despawn mutates the vector). Dynamic (M9).
  std::vector<uint32_t> ids;
  for (auto& en : w->entities) {
    if (en.alive && en.type == EntityType::Human) ids.push_back(en.id);
  }
  for (uint32_t id : ids) RS_Despawn(w, id);
}

// ---------------------------------------------------------------------------
// RS_ApplyImpulse (Lua rs.apply_impulse)
// ---------------------------------------------------------------------------

void RS_ApplyImpulse(RSWorld* w, uint32_t e, float ix, float iy) {
  if (!w || e == 0) return;
  Entity* en = findEntity(w, e);
  if (!en || en->bodies.empty()) return;
  // Main body: torso for humans, body 0 otherwise.
  uint32_t body = en->bodies[0];
  if (en->type == EntityType::Human && en->bodies.size() > (size_t)LIMB_TORSO)
    body = en->bodies[LIMB_TORSO];
  w->physics.applyImpulse(body, rs2d::Vec2(ix, iy));
}

// ---------------------------------------------------------------------------
// Weapons
// ---------------------------------------------------------------------------

struct WeaponDef { float damage; float range; float spread; int pellets; };
static WeaponDef weaponDef(int weapon) {
  switch (weapon) {
    case 0: return {25.0f, 700.0f, 0.03f, 1};   // pistol
    case 1: return {18.0f, 900.0f, 0.05f, 1};   // rifle
    case 2: return {9.0f, 350.0f, 0.12f, 8};    // shotgun
    case 3: return {90.0f, 1400.0f, 0.005f, 1}; // sniper
    case 4: return {12.0f, 600.0f, 0.08f, 1};   // smg
    default: return {20.0f, 700.0f, 0.05f, 1};
  }
}

void RS_FireHitscan(RSWorld* w, float x, float y, float angle, int weapon) {
  if (!w) return;
  WeaponDef wd = weaponDef(weapon);
  for (int p = 0; p < wd.pellets; ++p) {
    float a = angle + randf(w, -wd.spread, wd.spread);  // M6: was *3.0f
    rs2d::Vec2 p0(x, y);
    rs2d::Vec2 p1(x + cosf(a) * wd.range, y + sinf(a) * wd.range);
    rs2d::RaycastHit hit;
    float bestT = 1.0f;
    bool any = w->physics.raycast(p0, p1, &hit);
    float hx = p1.x, hy = p1.y;
    if (any) {
      hx = hit.point.x; hy = hit.point.y;
      // Damage falloff: full to 40% at max range.
      float dx = hx - x, dy = hy - y;
      float dist = sqrtf(dx * dx + dy * dy);
      float fall = 1.0f - 0.6f * (dist / wd.range);
      if (fall < 0.4f) fall = 0.4f;
      float dmg = wd.damage * fall;
      int limb = 0;
      Entity* en = findByBody(w, hit.body, &limb);
      if (en) {
        damageEntity(w, en, limb, dmg, hx, hy);
        // Impact impulse along the shot direction.
        w->physics.applyImpulseAt(hit.body,
            rs2d::Vec2(cosf(a) * dmg * 6.0f, sinf(a) * dmg * 6.0f),
            hit.point);
      }
      sparkBurst(w, hx, hy, 3, 1.0f, 0.85f, 0.4f);
      // Ricochet spark if it hit a prop.
      (void)bestT;
    }
    // Tracer segment (fades quickly).
    Tracer t;
    t.x0 = x; t.y0 = y; t.x1 = hx; t.y1 = hy;
    t.r = 1.0f; t.g = 0.9f; t.b = 0.5f;
    t.life = 0.08f;
    w->tracers.push_back(t);
    // Muzzle flash.
    sparkBurst(w, x, y, 2, 1.0f, 0.8f, 0.3f);
  }
}

void RS_ThrowGrenade(RSWorld* w, float x, float y, float vx, float vy) {
  if (!w) return;
  Entity en;
  en.id = allocEntityId(w);
  en.seq = w->nextSeq++;
  en.type = EntityType::Grenade;
  en.fuse = 2.0f;
  rs2d::BodyDef d = makeBodyDef();
  d.shape = rs2d::ShapeType::Circle;
  d.position = rs2d::Vec2(x, y);
  d.radius = 8.0f;
  d.friction = 0.4f;
  d.restitution = 0.3f;
  uint32_t b = w->physics.createBody(d);
  if (!b) return;
  w->physics.setVelocity(b, rs2d::Vec2(vx, vy));
  en.bodies.push_back(b);
  w->entities.push_back(std::move(en));
}

void RS_FireRocket(RSWorld* w, float x, float y, float angle) {
  if (!w) return;
  Entity en;
  en.id = allocEntityId(w);
  en.seq = w->nextSeq++;
  en.type = EntityType::Rocket;
  en.life = 5.0f;
  float sp = 700.0f;
  en.vx = cosf(angle) * sp;
  en.vy = sinf(angle) * sp;
  rs2d::BodyDef d = makeBodyDef();
  d.shape = rs2d::ShapeType::Box;
  d.position = rs2d::Vec2(x, y);
  d.halfExtents = rs2d::Vec2(14, 5);
  d.isStatic = false;
  // Kinematic: we move it manually each step (no gravity).
  d.density = 0.0001f;
  uint32_t b = w->physics.createBody(d);
  if (!b) return;
  // Store angle for rendering.
  w->physics.setTransform(b, w->physics.getPosition(b), angle);
  en.bodies.push_back(b);
  w->entities.push_back(std::move(en));
  // Launch smoke.
  smokeBurst(w, x, y, 6);
  fireBurst(w, x, y, 8);
}

void RS_MeleeSwing(RSWorld* w, float x, float y, float angle,
                   float range, float damage) {
  if (!w) return;
  float arc = 1.2f;  // radians, total arc width
  // Collect candidates.
  for (auto& en : w->entities) {
    if (!en.alive) continue;
    for (size_t i = 0; i < en.bodies.size(); ++i) {
      uint32_t b = en.bodies[i];
      rs2d::Vec2 p = w->physics.getPosition(b);
      float dx = p.x - x, dy = p.y - y;
      float d = sqrtf(dx * dx + dy * dy);
      if (d > range) continue;
      float ba = atan2f(dy, dx);
      float da = ba - angle;
      while (da > 3.14159f) da -= 6.28318f;
      while (da < -3.14159f) da += 6.28318f;
      if (fabsf(da) > arc * 0.5f) continue;
      // Hit!
      damageEntity(w, &en, (int)i, damage, p.x, p.y);
      // Knockback along swing direction.
      float kb = damage * 8.0f;
      w->physics.applyImpulse(b, rs2d::Vec2(cosf(angle) * kb,
                                            sinf(angle) * kb + kb * 0.3f));
      bloodBurst(w, p.x, p.y, 6);
      break;  // one limb per entity per swing
    }
  }
  // Swing arc visual: a few sparks along the arc.
  for (int i = 0; i < 6; ++i) {
    float a = angle - arc * 0.5f + arc * (i / 5.0f);
    sparkBurst(w, x + cosf(a) * range * 0.7f, y + sinf(a) * range * 0.7f,
               2, 0.9f, 0.9f, 0.9f);
  }
}

void RS_Explode(RSWorld* w, float x, float y, float radius, float power) {
  if (!w) return;
  explodeInternal(w, x, y, radius, power);
}

void RS_Ignite(RSWorld* w, float x, float y, float radius) {
  if (!w || radius <= 0) return;
  // Cap live fire zones: the fire tool ignites on every touch-move (60-120Hz),
  // and each tick iterates zones x entities x bodies. Beyond the cap, merge
  // into the nearest zone (refreshing its life) instead of growing forever.
  static constexpr size_t kMaxFireZones = 200;
  if (w->fires.size() >= kMaxFireZones) {
    size_t best = 0;
    float bestD2 = FLT_MAX;
    for (size_t i = 0; i < w->fires.size(); i++) {
      float dx = w->fires[i].x - x, dy = w->fires[i].y - y;
      float d2 = dx * dx + dy * dy;
      if (d2 < bestD2) { bestD2 = d2; best = i; }
    }
    w->fires[best].life = 6.0f;
    fireBurst(w, x, y, 16);
    return;
  }
  FireZone fz;
  fz.x = x; fz.y = y; fz.radius = radius; fz.life = 6.0f;
  w->fires.push_back(fz);
  fireBurst(w, x, y, 16);
  smokeBurst(w, x, y, 8);
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

uint32_t RS_GrabBegin(RSWorld* w, float x, float y) {
  if (!w) return 0;
  uint32_t body = w->physics.pickBody(rs2d::Vec2(x, y));
  if (!body) return 0;
  Grab g;
  g.handle = w->nextGrabId++;
  g.body = body;
  g.tx = x; g.ty = y;
  g.active = true;
  w->grabs.push_back(g);
  return g.handle;
}

void RS_GrabMove(RSWorld* w, uint32_t grab, float x, float y) {
  if (!w || grab == 0) return;
  for (auto& g : w->grabs) {
    if (g.handle == grab && g.active) {
      g.tx = x; g.ty = y;
      return;
    }
  }
}

void RS_GrabEnd(RSWorld* w, uint32_t grab) {
  if (!w || grab == 0) return;
  // M3: erase the grab record instead of just deactivating.
  for (size_t i = 0; i < w->grabs.size(); ++i) {
    if (w->grabs[i].handle == grab) {
      w->grabs[i] = std::move(w->grabs.back());
      w->grabs.pop_back();
      return;
    }
  }
}

void RS_SetFrozen(RSWorld* w, uint32_t e, int frozen) {
  if (!w) return;
  Entity* en = findEntity(w, e);
  if (!en) return;
  en->frozen = frozen != 0;
  for (uint32_t b : en->bodies) w->physics.setStatic(b, en->frozen);
}

void RS_HealHuman(RSWorld* w, uint32_t e) {
  if (!w) return;
  Entity* en = findEntity(w, e);
  if (!en || en->type != EntityType::Human) return;
  // N6: only revive limbs that are still physically attached (have a joint).
  // Detached limbs lost their joints when destroyed; marking them alive
  // would create a zombie — rendered alive but falling as disconnected debris.
  for (int i = 0; i < LIMB_COUNT; ++i) {
    if (!en->limbAlive[i]) {
      uint32_t body = en->bodies[i];
      bool attached = false;
      for (auto& jr : en->joints) {
        if (jr.bodyA == body || jr.bodyB == body) { attached = true; break; }
      }
      if (!attached) continue;  // detached: stays dead
      en->limbAlive[i] = true;
    }
    en->limbHP[i] = (i == LIMB_TORSO) ? 140.0f : (i == LIMB_HEAD ? 60.0f : 100.0f);
  }
  // Only clear death if head and torso are both alive again.
  if (en->limbAlive[LIMB_TORSO] && en->limbAlive[LIMB_HEAD]) {
    en->dead = false;
  }
  en->burning = false;
  en->burnTime = 0.0f;
  bloodBurst(w, w->physics.getPosition(en->bodies[LIMB_TORSO]).x,
             w->physics.getPosition(en->bodies[LIMB_TORSO]).y, 8);
}

uint32_t RS_EntityAtPoint(RSWorld* w, float x, float y) {
  if (!w) return 0;
  // M8: query all bodies near the point, return the most recently created
  // (topmost) entity, not just the first match.
  uint32_t bodies[64];
  int count = 0;
  w->physics.queryAABB(rs2d::Vec2(x - 1.0f, y - 1.0f),
                       rs2d::Vec2(x + 1.0f, y + 1.0f),
                       bodies, &count, 64);
  uint32_t best = 0, bestSeq = 0;
  for (int i = 0; i < count; ++i) {
    int limb = 0;
    Entity* en = findByBody(w, bodies[i], &limb);
    if (en && en->seq > bestSeq) { bestSeq = en->seq; best = en->id; }
  }
  return best;
}

void RS_SetPhysicsSubsteps(RSWorld* w, int n) {
  if (!w) return;
  if (n >= 1 && n <= 4) w->physicsSubsteps = n;
}

// ---------------------------------------------------------------------------
// Step
// ---------------------------------------------------------------------------

static void stepGameLogic(RSWorld* w, float dt) {
  // --- Grenade fuses ---
  for (auto& en : w->entities) {
    if (!en.alive) continue;
    if (en.type == EntityType::Grenade) {
      en.fuse -= dt;
      // Blink spark as fuse burns down.
      if (en.fuse < 1.0f && !en.bodies.empty()) {
        rs2d::Vec2 p = w->physics.getPosition(en.bodies[0]);
        if ((xorshift(w) & 7) == 0)
          sparkBurst(w, p.x, p.y, 1, 1.0f, 0.6f, 0.1f);
      }
      if (en.fuse <= 0 && !en.bodies.empty()) {
        rs2d::Vec2 p = w->physics.getPosition(en.bodies[0]);
        en.alive = false;
        releaseGrabsForBody(w, en.bodies[0]);  // M1
        w->physics.destroyBody(en.bodies[0]);
        en.bodies.clear();
        explodeInternal(w, p.x, p.y, 150.0f, 1100.0f);
      }
    } else if (en.type == EntityType::Rocket) {
      en.life -= dt;
      if (en.bodies.empty()) { en.alive = false; continue; }
      uint32_t b = en.bodies[0];
      rs2d::Vec2 p = w->physics.getPosition(b);
      // Move kinematically (no gravity).
      rs2d::Vec2 np(p.x + en.vx * dt, p.y + en.vy * dt);
      // Impact check: raycast from p to np.
      rs2d::RaycastHit hit;
      bool impact = w->physics.raycast(p, np, &hit);
      // Also check life expiry.
      if (impact || en.life <= 0) {
        float ix = impact ? hit.point.x : np.x;
        float iy = impact ? hit.point.y : np.y;
        en.alive = false;
        releaseGrabsForBody(w, b);  // M1
        w->physics.destroyBody(b);
        en.bodies.clear();
        explodeInternal(w, ix, iy, 120.0f, 800.0f);
      } else {
        float ang = w->physics.getAngle(b);
        w->physics.setTransform(b, np, ang);
        // Trail.
        if ((xorshift(w) & 3) == 0) {
          smokeBurst(w, p.x, p.y, 1);
          fireBurst(w, p.x, p.y, 1);
        }
      }
    }
  }

  // --- Grab springs (stiff spring toward target) ---
  for (auto& g : w->grabs) {
    if (!g.active || !g.body) continue;
    // M1: validate the body still belongs to a live entity (defense in
    // depth; releaseGrabsForBody should have caught destruction already).
    int dummyLimb = 0;
    if (!findByBody(w, g.body, &dummyLimb)) {
      g.active = false; g.body = 0;
      continue;
    }
    rs2d::Vec2 p = w->physics.getPosition(g.body);
    rs2d::Vec2 v = w->physics.getVelocity(g.body);
    // Critically-damped-ish spring: F = k*(target - p) - c*v.
    const float k = 60.0f;
    const float c = 8.0f;
    rs2d::Vec2 f((g.tx - p.x) * k - v.x * c,
                 (g.ty - p.y) * k - v.y * c);
    w->physics.applyImpulse(g.body, rs2d::Vec2(f.x * dt, f.y * dt));
  }

  // --- Fire zones: damage over time, spread, visuals ---
  for (size_t fi = 0; fi < w->fires.size();) {
    FireZone& fz = w->fires[fi];
    fz.life -= dt;
    // Visuals.
    if ((xorshift(w) & 1) == 0) {
      fireBurst(w, fz.x + randf(w, -fz.radius, fz.radius) * 0.7f,
                fz.y + randf(w, -fz.radius, fz.radius) * 0.4f, 2);
    }
    if ((xorshift(w) & 7) == 0) {
      smokeBurst(w, fz.x + randf(w, -fz.radius, fz.radius) * 0.5f,
                 fz.y + fz.radius * 0.5f, 1);
    }
    // Damage entities in radius.
    for (auto& en : w->entities) {
      if (!en.alive) continue;
      for (size_t i = 0; i < en.bodies.size(); ++i) {
        rs2d::Vec2 p = w->physics.getPosition(en.bodies[i]);
        float dx = p.x - fz.x, dy = p.y - fz.y;
        if (dx * dx + dy * dy < fz.radius * fz.radius) {
          // Humans and crates burn; barrels take heat damage (may explode).
          if (en.type == EntityType::Human ||
              en.type == EntityType::Crate ||
              en.type == EntityType::Barrel) {
            damageEntity(w, &en, (int)i, 25.0f * dt, p.x, p.y);
            if (!en.burning && en.alive) {
              en.burning = true;
              en.burnTime = 3.0f;
            }
          }
          break;
        }
      }
    }
    if (fz.life <= 0) {
      w->fires.erase(w->fires.begin() + fi);
    } else {
      ++fi;
    }
  }

  // --- Burning entities: ongoing damage + spread ---
  for (auto& en : w->entities) {
    if (!en.alive || !en.burning) continue;
    en.burnTime -= dt;
    if (!en.bodies.empty()) {
      rs2d::Vec2 p = w->physics.getPosition(en.bodies[0]);
      if ((xorshift(w) & 1) == 0) fireBurst(w, p.x, p.y, 1);
    }
    // Damage self.
    if (en.type == EntityType::Human) {
      // Burn the torso (or a random alive limb).
      int limb = LIMB_TORSO;
      if (!en.limbAlive[limb]) {
        for (int i = 0; i < LIMB_COUNT; ++i) {
          if (en.limbAlive[i]) { limb = i; break; }
        }
      }
      if (!en.bodies.empty()) {
        rs2d::Vec2 p = w->physics.getPosition(en.bodies[limb]);
        damageEntity(w, &en, limb, 15.0f * dt, p.x, p.y);
      }
    } else if (en.type == EntityType::Crate || en.type == EntityType::Barrel) {
      if (!en.bodies.empty()) {
        rs2d::Vec2 p = w->physics.getPosition(en.bodies[0]);
        damageEntity(w, &en, 0, 20.0f * dt, p.x, p.y);
      }
    }
    if (en.burnTime <= 0) en.burning = false;
    // Spread: small chance to ignite a new fire zone at this entity.
    if (en.burning && (xorshift(w) % 240) == 0 && !en.bodies.empty()) {
      rs2d::Vec2 p = w->physics.getPosition(en.bodies[0]);
      FireZone fz;
      fz.x = p.x; fz.y = p.y; fz.radius = 40.0f; fz.life = 2.0f;
      w->fires.push_back(fz);
    }
  }

  // --- Particles: CPU integration ---
  for (auto& p : w->particles) {
    if (!p.alive) continue;
    p.life -= dt;
    if (p.life <= 0) { p.alive = false; continue; }
    p.x += p.vx * dt;
    p.y += p.vy * dt;
    // Gravity for blood/debris/sparks; fire/smoke rise.
    if (p.type == 0 || p.type == 3 || p.type == 4) {
      p.vy -= 900.0f * dt;
    } else if (p.type == 1 || p.type == 2) {
      p.vy += 60.0f * dt;
      p.vx *= (1.0f - 1.5f * dt);
    }
    // Fade alpha by life, scaling the initial alpha (M5).
    float t = p.life / p.maxLife;
    p.a = p.a0 * (t < 1.0f ? t : 1.0f);
  }

  // --- Tracers fade ---
  for (size_t i = 0; i < w->tracers.size();) {
    w->tracers[i].life -= dt;
    if (w->tracers[i].life <= 0) {
      w->tracers.erase(w->tracers.begin() + i);
    } else {
      ++i;
    }
  }
}

// M4: process queued impact events (from the contact listener) after the
// physics substeps. Damages human limbs involved in hard impacts.
static void processImpacts(RSWorld* w) {
  for (auto& im : w->impacts) {
    // Damage scales with impulse above the 400 threshold.
    float dmg = (im.impulse - 400.0f) * 0.03f;
    if (dmg <= 0) continue;
    int limbA = -1, limbB = -1;
    // findByBody validates against live entities (guards destroyed bodies).
    Entity* ea = findByBody(w, im.a, &limbA);
    Entity* eb = findByBody(w, im.b, &limbB);
    if (ea && ea->type == EntityType::Human && !ea->dead && limbA >= 0) {
      damageEntity(w, ea, limbA, dmg, im.px, im.py);
      bloodBurst(w, im.px, im.py, 3);
    }
    if (eb && eb != ea && eb->type == EntityType::Human && !eb->dead &&
        limbB >= 0) {
      damageEntity(w, eb, limbB, dmg, im.px, im.py);
      bloodBurst(w, im.px, im.py, 3);
    }
  }
  w->impacts.clear();
}

void RS_Step(RSWorld* w, float dt) {
  if (!w || dt <= 0) return;
  auto t0 = std::chrono::steady_clock::now();

  // Fixed-timestep accumulator: 1/60 ticks, max 4 ticks.
  // M7: physicsSubsteps (1..4) subdivides each tick AND scales iterations.
  const float stepDt = 1.0f / 60.0f;
  w->stepAccum += dt;
  if (w->stepAccum > stepDt * 4) w->stepAccum = stepDt * 4;  // clamp
  int n = w->physicsSubsteps;
  if (n < 1) n = 1; if (n > 4) n = 4;
  int velIters = 8, posIters = 3;
  switch (n) {
    case 1: velIters = 6; posIters = 2; break;
    case 3: velIters = 10; posIters = 4; break;
    case 4: velIters = 12; posIters = 5; break;
    default: break;  // 2: 8/3
  }
  float subDt = stepDt / (float)n;
  int steps = 0;
  while (w->stepAccum >= stepDt && steps < 4) {
    for (int s = 0; s < n; ++s)
      w->physics.step(subDt, velIters, posIters);
    stepGameLogic(w, stepDt);
    w->stepAccum -= stepDt;
    ++steps;
  }
  // M4: impact damage after physics, before Lua.
  processImpacts(w);

  // Lua on_tick, once per RS_Step (not per substep).
  RS_LuaTick(w);

  // M3: erase dead entities now that no iteration is in progress.
  sweepDeadEntities(w);

  auto t1 = std::chrono::steady_clock::now();
  w->lastStepMs =
      std::chrono::duration<float, std::milli>(t1 - t0).count();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

RSWorld* RS_CreateWorld(void) { return new RSWorld(); }

void RS_DestroyWorld(RSWorld* w) {
  if (!w) return;
  RS_ScriptingWorldDestroyed(w); // delete bound Lua state before freeing w
  delete w;
}

void RS_ClearWorld(RSWorld* w) {
  if (!w) return;
  RS_ScriptingWorldDestroyed(w); // drop mod state before clearing entities
  for (auto& en : w->entities) {
    if (en.alive) destroyEntityBodies(w, en);
  }
  w->entities.clear();
  w->grabs.clear();
  w->fires.clear();
  w->tracers.clear();
  w->explosionQueue.clear();
  w->impacts.clear();
  w->processingExplosions = false;
  for (auto& p : w->particles) p.alive = false;
  // N5: do NOT reset nextEntityId/nextSeq. A mod holding an entity id
  // across a clear would see it silently reissued to a different entity
  // (use-after-clear aliasing). Ids stay monotonic for the world's lifetime.
  w->stepAccum = 0.0f;
}

// ---------------------------------------------------------------------------
// Render data
// ---------------------------------------------------------------------------

// Original palette.
static void limbColor(int limb, bool dead, float* r, float* g, float* b) {
  // Skin, shirt blue, pants brown-grey, dark boots.
  switch (limb) {
    case LIMB_HEAD: *r = 0.93f; *g = 0.76f; *b = 0.60f; break;  // skin
    case LIMB_TORSO: *r = 0.22f; *g = 0.42f; *b = 0.72f; break;  // shirt
    case LIMB_PELVIS: *r = 0.32f; *g = 0.28f; *b = 0.24f; break; // pants
    case LIMB_UAL: case LIMB_UAR:
      *r = 0.22f; *g = 0.42f; *b = 0.72f; break;                 // sleeves
    case LIMB_FAL: case LIMB_FAR:
      *r = 0.93f; *g = 0.76f; *b = 0.60f; break;                 // skin
    case LIMB_THL: case LIMB_THR:
      *r = 0.32f; *g = 0.28f; *b = 0.24f; break;                 // pants
    case LIMB_SHL: case LIMB_SHR:
      *r = 0.20f; *g = 0.19f; *b = 0.22f; break;                 // boots
    default: *r = 0.8f; *g = 0.8f; *b = 0.8f; break;
  }
  if (dead) { *r *= 0.35f; *g *= 0.35f; *b *= 0.38f; }
}

int RS_GetRenderItems(RSWorld* w, RSRenderItem* out, int maxItems) {
  if (!w || !out || maxItems <= 0) return 0;
  int n = 0;
  for (auto& en : w->entities) {
    if (!en.alive) continue;
    for (size_t i = 0; i < en.bodies.size() && n < maxItems; ++i) {
      uint32_t b = en.bodies[i];
      rs2d::Vec2 p = w->physics.getPosition(b);
      float ang = w->physics.getAngle(b);
      RSRenderItem& it = out[n++];
      it.x = p.x; it.y = p.y; it.angle = ang;
      it.tint = 0;
      float r = 0.8f, g = 0.8f, bl = 0.8f;
      bool dead = false;
      switch (en.type) {
        case EntityType::Human: {
          int limb = (int)i;
          dead = en.dead;
          limbColor(limb, dead, &r, &g, &bl);
          if (en.burning) it.tint = 2;
          else if (dead) it.tint = 1;
          // Detached limbs render darker.
          if (!en.limbAlive[limb] && !dead) { r *= 0.6f; g *= 0.6f; bl *= 0.6f; }
          break;
        }
        case EntityType::Crate:
          r = 0.55f; g = 0.38f; bl = 0.22f;
          if (en.burning) it.tint = 2;
          break;
        case EntityType::Barrel:
          r = 0.75f; g = 0.15f; bl = 0.12f;
          if (en.burning) it.tint = 2;
          break;
        case EntityType::Ball:
          r = 0.30f; g = 0.55f; bl = 0.85f;
          break;
        case EntityType::Plank:
          r = 0.60f; g = 0.45f; bl = 0.28f;
          break;
        case EntityType::Grenade:
          r = 0.20f; g = 0.35f; bl = 0.20f;
          break;
        case EntityType::Rocket:
          r = 0.80f; g = 0.80f; bl = 0.85f;
          break;
      }
      it.r = r; it.g = g; it.b = bl; it.a = 1.0f;
      // Shape and size: infer from physics body.
      // We stored the shape in the body; query via a heuristic:
      // humans: head (limb 0) is circle; others are boxes.
      // For props, we know the type.
      bool isCircle = false;
      float sw = 20, sh = 20;
      if (en.type == EntityType::Human) {
        int limb = (int)i;
        if (limb == LIMB_HEAD) { isCircle = true; sw = sh = 28; }
        else {
          // Box sizes from spawn (half-extents * 2).
          switch (limb) {
            case LIMB_TORSO: sw = 36; sh = 50; break;
            case LIMB_PELVIS: sw = 32; sh = 20; break;
            case LIMB_UAL: case LIMB_UAR: sw = 12; sh = 30; break;
            case LIMB_FAL: case LIMB_FAR: sw = 10; sh = 28; break;
            case LIMB_THL: case LIMB_THR: sw = 14; sh = 40; break;
            case LIMB_SHL: case LIMB_SHR: sw = 12; sh = 38; break;
            default: break;
          }
        }
      } else if (en.type == EntityType::Barrel || en.type == EntityType::Ball ||
                 en.type == EntityType::Grenade) {
        isCircle = true;
        // Get radius from AABB.
        rs2d::AABB ab = w->physics.getAABB(b);
        sw = ab.mx.x - ab.mn.x;
        sh = ab.mx.y - ab.mn.y;
      } else {
        rs2d::AABB ab = w->physics.getAABB(b);
        sw = ab.mx.x - ab.mn.x;
        sh = ab.mx.y - ab.mn.y;
        // For rotated boxes, AABB overestimates; acceptable for renderer.
      }
      it.shape = isCircle ? 1 : 0;
      it.w = sw; it.h = sh;
    }
  }
  // Tracers as segments.
  for (auto& t : w->tracers) {
    if (n >= maxItems) break;
    RSRenderItem& it = out[n++];
    it.shape = 2;
    it.x = t.x0; it.y = t.y0;
    it.w = t.x1; it.h = t.y1;
    it.angle = 0;
    it.r = t.r; it.g = t.g; it.b = t.b;
    it.a = t.life / 0.08f;  // fade
    it.tint = 0;
  }
  // Fire zones as large translucent circles (for the renderer to draw glow).
  for (auto& fz : w->fires) {
    if (n >= maxItems) break;
    RSRenderItem& it = out[n++];
    it.shape = 1;
    it.x = fz.x; it.y = fz.y; it.angle = 0;
    it.w = fz.radius * 2; it.h = fz.radius * 2;
    it.r = 1.0f; it.g = 0.5f; it.b = 0.1f; it.a = 0.25f;
    it.tint = 2;
  }
  return n;
}

int RS_GetParticles(RSWorld* w, RSParticle* out, int maxItems) {
  if (!w || !out || maxItems <= 0) return 0;
  int n = 0;
  int cap = w->particleBudget < (int)w->particles.size()
                ? w->particleBudget : (int)w->particles.size();
  for (int i = 0; i < cap && n < maxItems; ++i) {
    const Particle& p = w->particles[i];
    if (!p.alive) continue;
    RSParticle& o = out[n++];
    o.x = p.x; o.y = p.y; o.vx = p.vx; o.vy = p.vy;
    o.life = p.life; o.maxLife = p.maxLife; o.size = p.size;
    o.r = p.r; o.g = p.g; o.b = p.b; o.a = p.a;
    o.type = p.type;
  }
  return n;
}

void RS_SetParticleBudget(RSWorld* w, int max) {
  if (!w) return;
  if (max < 0) max = 0;
  if (max > (int)w->particles.size()) max = (int)w->particles.size();
  w->particleBudget = max;
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

int RS_GetEntityCount(RSWorld* w) {
  if (!w) return 0;
  int n = 0;
  for (auto& en : w->entities) if (en.alive) ++n;
  return n;
}

int RS_GetBodyCount(RSWorld* w) {
  if (!w) return 0;
  return w->physics.bodyCount();
}

float RS_GetStepMs(RSWorld* w) {
  if (!w) return 0.0f;
  return w->lastStepMs;
}

// NOTE: RS_RunLuaFile and RS_GetLuaError are NOT defined here.
// They live in Scripting/lua_bindings.cpp (the Lua module owns them).
// Defining them here would cause duplicate symbols at link time.
