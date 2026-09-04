#pragma once

#include <cstddef>
#include <cstdint>

enum class MobEffectType : std::uint32_t {
    Empty = 0,
    Speed = 1,
    Slowness = 2,
    Haste = 3,
    MiningFatigue = 4,
    Strength = 5,
    InstantHealth = 6,
    InstantDamage = 7,
    JumpBoost = 8,
    Nausea = 9,
    Regeneration = 10,
    Resistance = 11,
    FireResistance = 12,
    WaterBreathing = 13,
    Invisibility = 14,
    Blindness = 15,
    NightVision = 16,
    Hunger = 17,
    Weakness = 18,
    Poison = 19,
    Wither = 20,
    HealthBoost = 21,
    Absorption = 22,
    Saturation = 23,
    Levitation = 24,
    FatalPoison = 25,
    ConduitPower = 26,
    SlowFalling = 27,
    BadOmen = 28,
    VillageHero = 29,
    Darkness = 30,
    TrialOmen = 31,
    WindCharged = 32,
    Weaving = 33,
    Oozing = 34,
    Infested = 35,
    RaidOmen = 36,
    BreathOfTheNautilus = 37
};

struct MobEffectInstance {
    MobEffectType id;
    std::int32_t duration;
    std::byte padding08[0x18];
    std::int32_t amplifier;
    bool displayOnScreenTextureAnimation;
    bool ambient;
    bool noCounter;
    bool effectVisible;
    std::byte padding28[0x58];
};

static_assert(offsetof(MobEffectInstance, id) == 0x0);
static_assert(offsetof(MobEffectInstance, duration) == 0x4);
static_assert(offsetof(MobEffectInstance, amplifier) == 0x20);
static_assert(offsetof(MobEffectInstance, displayOnScreenTextureAnimation) == 0x24);
static_assert(offsetof(MobEffectInstance, ambient) == 0x25);
static_assert(offsetof(MobEffectInstance, noCounter) == 0x26);
static_assert(offsetof(MobEffectInstance, effectVisible) == 0x27);
static_assert(sizeof(MobEffectInstance) == 0x80);

struct MobEffectsComponent {
    MobEffectInstance* begin;
    MobEffectInstance* end;
    MobEffectInstance* capacity;
};

static_assert(sizeof(MobEffectsComponent) == 0x18);
