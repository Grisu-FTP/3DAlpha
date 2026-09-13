// The three short methods behind breaking and eating, over the generated
// tables. See tool_rules.hpp.

#include "core/item/tool_rules.hpp"

#include "core/block/registry.hpp"
#include "core/item/registry.hpp"

#include "harvest.hpp"  // generated; see tools/configure.py

#include <limits>

namespace mc::item {

namespace {

const mcver::ToolRule* ruleFor(ItemId held)
{
    if (held <= 0) {
        return nullptr;
    }
    for (int i = 0; i < mcver::kToolRuleCount; ++i) {
        if (mcver::kToolRules[i].item == held) {
            return &mcver::kToolRules[i];
        }
    }
    return nullptr;
}

}  // namespace

bool canHarvest(ItemId held, block::BlockId block)
{
    if (int(block) >= mcver::kHarvestBlockTableSize || !mcver::kBlockNeedsTool[block]) {
        return true;
    }
    const mcver::ToolRule* rule = ruleFor(held);
    if (rule == nullptr) {
        return false;
    }
    for (int i = 0; i < rule->harvestCount; ++i) {
        if (mcver::kToolHarvests[rule->harvestFirst + i] == block) {
            return true;
        }
    }
    return false;
}

float strVsBlock(ItemId held, block::BlockId block)
{
    const mcver::ToolRule* rule = ruleFor(held);
    if (rule == nullptr) {
        return 1.0f;
    }
    for (int i = 0; i < rule->efficiencyCount; ++i) {
        const mcver::ToolEfficiency& e = mcver::kToolEfficiencies[rule->efficiencyFirst + i];
        if (e.block == block) {
            return e.value;
        }
    }
    return 1.0f;
}

float playerStrVsBlock(ItemId held, block::BlockId block, bool eyeInWater, bool onGround)
{
    // `dm.a(Lly;)F`: `f = inventory.getStrVsBlock(block); if (isInsideOfMaterial
    // (water)) f /= 5; if (!onGround) f /= 5;`
    float f = strVsBlock(held, block);
    if (eyeInWater) {
        f /= 5.0f;
    }
    if (!onGround) {
        f /= 5.0f;
    }
    return f;
}

float relativeHardness(ItemId held, block::BlockId block, bool eyeInWater, bool onGround)
{
    const float hardness = block::def(block).hardness;
    if (hardness < 0.0f) {
        return 0.0f;
    }
    // Stated rather than left to IEEE division, which gives the same answer:
    // a sanitiser build that traps float division by zero should not fire on
    // every torch.
    if (hardness == 0.0f) {
        return std::numeric_limits<float>::infinity();
    }
    if (!canHarvest(held, block)) {
        return 1.0f / hardness / 100.0f;
    }
    return playerStrVsBlock(held, block, eyeInWater, onGround) / hardness / 30.0f;
}

int wearOnBreak(ItemId held)
{
    const mcver::ToolRule* rule = ruleFor(held);
    return rule != nullptr ? rule->wearOnBreak : 0;
}

int wearOnHit(ItemId held)
{
    const mcver::ToolRule* rule = ruleFor(held);
    return rule != nullptr ? rule->wearOnHit : 0;
}

namespace {

// Whether the block this item puts down is fire -- flint and steel, which wears
// rather than spends.
bool placesFire(const ItemDef& d)
{
    return d.places != 0 && block::def(block::BlockId(d.places)).tick == block::TickBehaviour::Fire;
}

}  // namespace

int spendOnUse(ItemId held)
{
    if (held <= 0) {
        return 0;
    }
    const ItemDef& d = def(held);
    if (d.places != 0 && !placesFire(d)) {
        return 1;
    }
    if (d.spawns == SpawnsEntity::Painting || d.spawns == SpawnsEntity::Boat
        || d.spawns == SpawnsEntity::Minecart) {
        return 1;
    }
    return 0;
}

int wearOnUse(ItemId held)
{
    if (held <= 0) {
        return 0;
    }
    const ItemDef& d = def(held);
    return (d.tills || placesFire(d)) ? 1 : 0;
}

int foodHeals(ItemId held)
{
    if (held <= 0) {
        return 0;
    }
    for (int i = 0; i < mcver::kFoodRuleCount; ++i) {
        if (mcver::kFoodRules[i].item == held) {
            return mcver::kFoodRules[i].heals;
        }
    }
    return 0;
}

}  // namespace mc::item
