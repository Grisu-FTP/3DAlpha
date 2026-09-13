// The entity half of getCollidingBoundingBoxes. See entity_boxes.hpp.

#include "core/entity/entity_boxes.hpp"

#include "core/entity/arrow.hpp"
#include "core/entity/boat.hpp"
#include "core/entity/falling_block.hpp"
#include "core/entity/item_entity.hpp"
#include "core/entity/minecart.hpp"
#include "core/entity/mob.hpp"
#include "core/entity/painting.hpp"
#include "core/entity/player_body.hpp"
#include "core/entity/primed_tnt.hpp"
#include "core/tick/tick_world.hpp"

namespace mc::entity {
namespace {

// One pool's worth of boxes.
//
// The eight pools share no base class -- they are eight independent structs,
// each with its own idea of what an empty slot looks like (a `bool alive` on
// some, a zero count or a zero block id on others) -- so what they have in
// common is spelled as a template and a small accessor rather than an
// interface. `boxOf` answers null for a slot that is not live, which is the
// one question this file needs and the one each pool words differently.
//
// **A box is emitted only if it intersects the swept volume**, which is the
// original's own final gate: the candidate query is widened by 0.25 on every
// axis and then each box is tested against the unwidened one before it is
// added. Walking the pools directly makes the widening invisible -- it only
// ever mattered because the original's candidates come out of chunk lists.
//
// **`self` is skipped**, because `getCollidingBoundingBoxes` takes the moving
// entity as its first argument and leaves it out. A cart handed its own box
// would find it in the way of every move it ever tried.
template <class Pool, class BoxOf>
void emitPool(const Pool* pool, const AABB& swept, const void* self,
              tick::TickWorld::SolidBoxSink sink, void* sinkCtx, BoxOf boxOf)
{
    if (pool == nullptr) {
        return;
    }
    for (int i = 0; i < pool->count(); ++i) {
        const auto& entity = (*pool)[i];
        if (static_cast<const void*>(&entity) == self) {
            continue;
        }
        const AABB* box = boxOf(entity);
        if (box == nullptr || !box->intersects(swept)) {
            continue;
        }
        sink(sinkCtx, *box);
    }
}

void solidBoxes(void* ctx, const AABB& swept, const void* self,
                bool moverCollidesWithEntities, tick::TickWorld::SolidBoxSink sink,
                void* sinkCtx)
{
    const EntityBoxes* entities = static_cast<const EntityBoxes*>(ctx);

    // `kh.f_()`: the two classes that answer with a box, which everything that
    // moves has to clip against.
    emitPool(entities->boats, swept, self, sink, sinkCtx,
             [](const Boat& b) { return b.alive ? &b.box : nullptr; });
    emitPool(entities->minecarts, swept, self, sink, sinkCtx,
             [](const Minecart& c) { return c.alive ? &c.box : nullptr; });

    // `kh.b_(kh)`: null unless the mover is one of those same two classes, in
    // which case it is the *neighbour's* box, whatever the neighbour is.
    if (!moverCollidesWithEntities) {
        return;
    }
    emitPool(entities->mobs, swept, self, sink, sinkCtx,
             [](const Mob& m) { return m.alive ? &m.body.box : nullptr; });
    emitPool(entities->items, swept, self, sink, sinkCtx,
             [](const ItemEntity& i) { return i.alive() ? &i.box : nullptr; });
    emitPool(entities->arrows, swept, self, sink, sinkCtx,
             [](const Arrow& a) { return a.alive ? &a.box : nullptr; });
    emitPool(entities->paintings, swept, self, sink, sinkCtx,
             [](const Painting& p) { return p.alive ? &p.box : nullptr; });
    emitPool(entities->primedTnt, swept, self, sink, sinkCtx,
             [](const PrimedTnt& t) { return t.alive() ? &t.box : nullptr; });
    emitPool(entities->fallingBlocks, swept, self, sink, sinkCtx,
             [](const FallingBlock& f) { return f.alive() ? &f.box : nullptr; });

    // The player is a body rather than a pool, so it is the same three tests
    // written out.
    const PlayerBody* player = entities->player;
    if (player != nullptr && static_cast<const void*>(player) != self
        && player->box.intersects(swept)) {
        sink(sinkCtx, player->box);
    }
}

}  // namespace

void bindEntityBoxes(tick::TickWorld& world, const EntityBoxes* entities)
{
    if (entities == nullptr) {
        world.setSolidBoxQuery(nullptr, nullptr);
        return;
    }
    // The query only reads the pools; the const is cast away for the seam's
    // `void*` and nowhere else.
    world.setSolidBoxQuery(&solidBoxes, const_cast<EntityBoxes*>(entities));
}

}  // namespace mc::entity
