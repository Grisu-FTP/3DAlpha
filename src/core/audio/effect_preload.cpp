// See effect_preload.hpp for why this list is not in the platform layer.

#include "core/audio/effect_preload.hpp"

#include "core/audio/block_sound.hpp"
#include "core/audio/sound_engine.hpp"
#include "core/entity/mob.hpp"

namespace mc::audio {

usize preloadEffects(SoundEngine& engine)
{
    usize loaded = 0;

    // The menus, and the only emitter this port had for a long time.
    loaded += engine.preloadSound("random.click");

    // **What a block behaviour plays.** All of these are cued from
    // `core/tick/` through `TickWorld::playSoundAt`, or from
    // `core/item/use.cpp`. See docs/audio-a1.1.2.md, *What a block behaviour
    // plays, and the seam it plays through*.
    loaded += engine.preloadSound("random.door_open");
    loaded += engine.preloadSound("random.door_close");
    loaded += engine.preloadSound("fire.ignite");

    // **What a block's *display* tick plays**, which is a second seam and not
    // the one above: `Block.randomDisplayTick` runs off `cn.m(III)V`'s thousand
    // darts rather than off a world tick, and two of the seven blocks that
    // answer it make a noise as well as a particle -- see core/tick/display.hpp.
    //
    //   * `liquid.water` -- `jp.b`, one dart in 64 over **flowing** water. A
    //     still source is silent; only the stream off it trickles.
    //   * `fire.fire` -- `og.b`, one dart in 24 over any fire block.
    //
    // Both are keys nothing in this build could reach before the display tick
    // existed, which is the failure mode this file is here to make visible.
    loaded += engine.preloadSound("liquid.water");
    loaded += engine.preloadSound("fire.fire");

    // **What an entity plays.**
    //
    //   * `random.pop` -- a dropped item picked up, `dx.b(dm)`.
    //   * `random.fizz` -- a stack landing in lava (`dx.e_()`) and a burning
    //     animal reaching water (`kh.c()`'s tail). Two different volumes and
    //     two different pitches out of one file.
    //   * `random.splash` -- `kh.y()`'s water entry, which the player, the four
    //     animals, dropped items, arrows and boats all reach. See
    //     core/entity/water_entry.hpp.
    //   * `random.drr` -- an arrow striking a block or an entity, `kg.e_()`.
    //   * `random.bow` -- firing one, `jg.a(ev, cn, dm)`.
    //
    // `random.bow` and `random.drr` were played by code that shipped without
    // ever being on this list, so the bow was mute; that is the failure mode
    // this file exists to make visible.
    loaded += engine.preloadSound("random.pop");
    loaded += engine.preloadSound("random.fizz");
    loaded += engine.preloadSound("random.splash");
    loaded += engine.preloadSound("random.drr");
    loaded += engine.preloadSound("random.bow");

    // **What a monster plays that is not one of its three `getSound` keys.**
    //
    //   * `random.fuse` -- a creeper lighting, `dd.a(Lkh;F)V`, once per fuse at
    //     volume 1 and pitch 0.5.
    //   * `random.explode` -- the blast itself, `je.a(...)`, at volume **4**,
    //     which is the loudest thing in the game and is four times what
    //     anything else asks for.
    //   * `mob.slimeattack` -- `ma.b(Ldm;)V`, a slime doing damage by standing
    //     on you. It is the one `mob.*` key that is not in `MobDef`, exactly as
    //     `mob.chickenplop` is, and for the same reason: no `getSound` method
    //     names it.
    //
    // The nine kinds' twenty-seven `getSound` keys come off `MobDef` below.
    loaded += engine.preloadSound("random.fuse");
    loaded += engine.preloadSound("random.explode");
    loaded += engine.preloadSound("mob.slimeattack");

    // **`random.hurt` -- the player's own**, and it is here now because
    // something can finally hurt them. `ge.d()` returns it and `dm` does not
    // override it, so it is what a zombie's fist, a skeleton's arrow, a
    // creeper's blast and a slime landing on you all play. Every *mob* has its
    // own key and never reaches this one -- see `MobDef`.
    loaded += engine.preloadSound("random.hurt");

    // Derived, both of them. See the header.
    loaded += preloadBlockSounds(engine);
    loaded += entity::preloadMobSounds(engine);
    return loaded;
}

}  // namespace mc::audio
