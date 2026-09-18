#pragma once

// What the engine knows about an item, and why there is an item table at all.
//
// **Blocks were enough right up until the inventory was.** A hotbar of block
// ids can be placed and drawn, and that is what this project had. It cannot be
// *saved*, because a1.1.2's `InventoryPlayer` writes item ids and two of the
// things you carry are not blocks: a door is item 324 and the block it leaves
// behind is 64, sugar cane is item 338 and its block is 83. A saved hotbar of
// block ids would round-trip through `level.dat` as something the real client
// reads back as a different game.
//
// It is also why the door in the hand used to show *the bottom half of a door*.
// Block 64's texture is terrain tile 97, which is the lower panel, because that
// is what the block is drawn with; the door you hold is item 324 and its icon
// is tile 43 of `gui/items.png`. Nothing was wrong with the drawing code -- it
// was being asked about the wrong thing.
//
// **The table is generated, and every column but the name came out of a running
// jar.** `tools/genref.java --items` reads `Item.itemsList` for the icon, the
// stack size and the durability, and *measures* `places` by standing a real
// `EntityPlayer` in a real `World` and using each item on each face of each of
// five grounds. That is how a door reports 64 and sugar cane reports 83 without
// this project ever writing either number down. See that tool for the whole
// derivation, and data/<version>/items.json for the result.
//
// Names are ours, exactly as block names are, and for the same reason:
// `setItemName` postdates the alpha era and a1.1.2's `Item` class holds no
// strings at all.

#include "core/util/types.hpp"

namespace mc::item {

// Item ids are shorts on disk and on the wire -- `id` is a TAG_Short in the
// inventory list, and the two music discs are 2256 and 2257, well past a byte.
using ItemId = i16;

// Which image an icon indexes. **a1.1.2's own rule**, not a column somebody
// assigned: `RenderItem` draws from terrain.png below id 256 and from
// gui/items.png at or above it, which is exactly the boundary between the
// ItemBlocks the `Item` static initialiser builds for every block and the items
// declared after them.
enum class IconSheet : u8 {
    Terrain,
    Items,
};

// **What an item puts into the world when it is not a block.**
//
// Six items in a1.1.2 do this and between them they are four of the things a
// play session reported as simply not working. The shape of the bug is the same
// in every case and it is worth naming: `ItemPainting.onItemUse` and
// `ItemBoat.onItemRightClick` run to completion, build an entity, and hand it
// to `World.entityJoinedWorld` -- so a build with nowhere to put an entity
// performs the whole click and produces nothing at all, silently. "Boats and
// minecarts don't work (not even placeable)" is that, exactly.
//
// It is a column rather than a rule because `places` cannot express it:
// `places` is 0 for all six, correctly, and 0 also means "does nothing". The
// two have to be told apart.
enum class SpawnsEntity : u8 {
    None,
    Painting,
    Boat,
    Minecart,
    Arrow,
};

struct ItemDef {
    // `armour` for anything that is not a piece of armour. -1 rather than a
    // fifth slot number, so a signed comparison against a slot index cannot
    // accidentally match one.
    static constexpr i8 kNotArmour = -1;

    // `bucket` for anything that is not a bucket. See that field: 0, -1 and
    // every fluid block id are all taken, so this is -2.
    static constexpr i16 kNotABucket = -2;

    const char* name;

    // Tile index into `sheet`, from `Item.getIconIndex`.
    u16 icon;

    // The block this item puts into the world, or 0 for one that puts down
    // nothing. **Measured, not read** -- see the header note. A bucket reports
    // 0 and that is right: `ItemBucket` works from a ray trace in
    // `onItemRightClick` rather than from a face, so there is no placement here
    // to measure, and pouring a bucket is a thing this build cannot do yet.
    u16 places;

    // `Item.maxDamage`. Carried so a stack round-trips; nothing wears out yet.
    u16 durability;

    // `di.a(Lkh;)I` -- **getDamageVsEntity**, and it is the whole of how hard a
    // click lands: `EntityPlayer.attackTargetEntityWithCurrentItem` is
    // `int i = inventory.getDamageVsEntity(entity); if (i > 0)
    // entity.attackEntityFrom(this, i);` and nothing else scales it.
    //
    // 1 for the great majority, because `Item`'s own method returns that
    // constant and only two classes override it -- `bs` (ItemTool) answers
    // `material + kind`, so a wooden shovel is 1 and a diamond axe 6, and `iu`
    // (ItemSword) answers `4 + material * 2`, so a wooden sword is 4 and a
    // diamond one 10. **Gold is material 0**, beside wood, which is why a
    // golden sword hits for 4 and wears out in 32 uses.
    //
    // The unknown row carries 1 as well, which makes `def(held).damageVsEntity`
    // the empty hand's answer too -- `InventoryPlayer.getDamageVsEntity`
    // returns 1 when the slot holds nothing, so the fallback and the constant
    // are the same number by the jar's own arithmetic and not by coincidence.
    u8 damageVsEntity;

    // `Item.maxStackSize`. 64 for most things, 1 for tools and doors, 16 for a
    // snowball.
    u8 stack;

    IconSheet sheet;

    // `ItemArmor.armorType` -- **which of the four armour slots this piece
    // belongs in**, and `kNotArmour` for everything that is not armour at all.
    //
    // It is here rather than inferred from the name because the game infers
    // nothing either: `SlotArmor.isItemValid` is
    // `item instanceof ItemArmor && ((ItemArmor) item).armorType == slotType`,
    // one field compared against one number, and that is the only rule there
    // is. Without the column every slot took every item, so a helmet went on
    // the feet -- which is the bug this was added for.
    //
    // **0 is the helmet and 3 the boots, which is not the order the save file
    // stores them in.** `ContainerPlayer` builds its four slots as
    // `getSizeInventory() - 1 - i` against armorType `i`, so
    // `armorInventory[3]` is the helmet and `[0]` the boots -- the flip is in
    // `Inventory::armourSlotFor`, once, rather than at every reader.
    i8 armour;

    // `ItemBucket.isFull`, which is **one int carrying three meanings** and is
    // left as the jar has it rather than split into a shape that would have to
    // be re-joined at every branch of `onItemRightClick`:
    //
    //   * `0` -- the empty bucket, which fills from a source block;
    //   * a **flowing** fluid block id -- 8 and 10, not the still 9 and 11 --
    //     for a full one, which is the block it pours and the reason poured
    //     water spreads instead of sitting in its cell;
    //   * `-1` -- milk, which empties itself and puts nothing into the world.
    //
    // `kNotABucket` is a fourth value for everything else, chosen as -2 because
    // it is the one number the field cannot hold.
    i16 bucket;

    // Which entity this item spawns, and `None` for the great majority that
    // spawn none. See the note above the struct.
    SpawnsEntity spawns;

    // `ItemMinecart`'s own `a` field -- 0 a plain minecart, 1 a chest, 2 a
    // furnace -- and 0 for everything else. It is the constructor argument the
    // three minecart items are built with, so it is the game's own answer to
    // which cart an item makes rather than a reading of its name.
    u8 spawnVariant;

    // **Whether this item's icon tile is rewritten every tick**, which in
    // a1.1.2 is true of exactly one item and is the whole of how a compass
    // works.
    //
    // There is no compass *behaviour* in this version to implement: item 345 is
    // a plain `di` with no `onItemUse` and no `onItemRightClick`, and a build
    // that went looking for one would find nothing and conclude the item was
    // unimplemented. What the game actually does is register an `aa`
    // (TextureCompassFX) at startup and let `RenderEngine` overwrite the 256
    // texels of tile 54 of gui/items.png every frame with a needle aimed at
    // the world's spawn. So the compass is a *texture* and this is the flag
    // that says so.
    //
    // Read off the FX class by the generator rather than matched by name, and
    // it is a property of the tile rather than of the item -- see
    // tools/genref.java's emitItems and core/texture/compass_fx.hpp.
    bool animatedIcon;

    // **Whether this item turns grass or dirt into farmland** -- `fu`, ItemHoe,
    // asked as a class by the generator.
    //
    // It is a column for the same reason `spawns` is one, and it is the same
    // hole: `places` is measured by using the item on a face and reading the
    // cell the face offsets into, and a hoe writes its farmland into the cell
    // that was *struck*. So all five hoes measure `places` as 0 -- correctly --
    // and 0 also means "does nothing", which is what they did.
    bool tills;

    // **`lg`'s own `a` field -- the track a music disc plays**, and the empty
    // string for everything that is not one. `lg` (ItemRecord) is constructed
    // `new lg(2000, "13")` and `new lg(2001, "cat")`, and that string is three
    // things at once: the name handed to `World.playRecord`, the key the
    // streaming pool files `streaming/13.mus` under, and the only thing that
    // tells a disc from any other unstackable item.
    //
    // It is a column rather than a name match for the reason `spawns` is one:
    // the item table has no class, and "record_13" is *our* name -- a1.1.2's
    // `Item` carries no strings but this one.
    const char* record;

    // Whether the Creative hand offers it. Two rules decide this and they are
    // applied in the generator, not here -- see tools/genref.java's emitItems.
    // The short version: an item that places nothing is not offered, an
    // ItemBlock whose block has a carried form is not offered, and four block
    // states are named and excluded.
    bool palette;

    // False for every id this version leaves empty, which is most of the range:
    // a1.1.2 defines 163 items between 1 and 2257.
    bool known;
};

}  // namespace mc::item
