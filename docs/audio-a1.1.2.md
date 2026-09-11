# Sound in a1.1.2, and what this port does about it

Everything here was read out of `minecraft-a1.1.2_01-client.jar` with `javap -p -c`, not
from a wiki and not from memory. Class names are the obfuscated ones, because that is what
is actually in the jar; where a real name is known it is given beside it.

| Obfuscated | What it is |
|---|---|
| `of` | SoundManager |
| `eb` | SoundPool |
| `ah` | SoundPoolEntry — `String a` (name), `URL b` |
| `fr` | GameSettings |
| `bf` | ThreadDownloadResources |
| `PlayerControllerSP` | PlayerControllerSP |
| `ep` | the `.mus` codec, Mojang's own |

**The jar ships no audio at all.** Every non-class entry in it is a `.png`, plus
`default.gif`, `title/splashes.txt` and a zero-byte entry named `null`. There are no
`.ogg`, `.wav` or `.mus` files and no list of any. This matters more than it sounds: it
means no track name can be hardcoded, here or there — see *The pool is the folder* below.

## The music ticker — `SoundManager`'s field, `SoundManager`'s method

This is the whole of "the random music that happens in the game". One `int`, one
`java.util.Random`, and nine lines.

`SoundManager`'s constructor seeds it:

The countdown is a random integer below 12000 ticks, so 0 to 10 minutes.

and `SoundManager`'s method runs it, once per tick:

Each tick it does nothing if the sound system is not loaded or the music volume option is zero,
or if a background-music or streaming source is currently playing. Otherwise, while the countdown
is above zero it decrements it and stops. When it reaches zero it picks a random entry from the
music pool; if there is none it stops. It then re-arms the countdown with 24000 plus a random
draw below 24000 (20 to 40 minutes), starts the entry as non-looping background music, sets its
volume from the music option, and plays it.

The bytecode, for the two constants that matter:

```
of.<init>   58: sipush 12000   61: invokevirtual Random.nextInt   64: putfield i:I
of.c()      41: getfield i:I   45: ifle 59   48..55: i = i - 1     58: return
            71: sipush 24000   79: invokevirtual Random.nextInt
            82: sipush 24000   85: iadd       86: putfield i:I
```

### The rule that is easy to get wrong

**The counter is not decremented while anything is playing.** Both `playing()` checks sit
at pc 19 and pc 30, *before* the decrement at pc 41. So the 20–40 minutes is silence
between tracks, and the wall-clock period is the track's own length **plus** that.

A port that decrements unconditionally plays music roughly one track-length more often
than the real game — about 13 tracks in six hours where a1.1.2 plays 12 — and nothing
about it looks wrong in the code. `tests/audio_test.cpp` pins it with
`counterDoesNotAdvanceWhileATrackIsPlaying`, and `--music-schedule` shows it: the printed
gaps are the counter gap plus the track length, because the harness reads each file's
length out of its Ogg headers.

### Where it is called from

Exactly one place in the jar: `PlayerControllerSP`'s method (`PlayerControllerSP.onUpdate`), as its last
statement. a method of another original class and a method of another original class — the base and multiplayer controllers — are `{ return; }`.
**a1.1.2 therefore plays no background music in multiplayer.**

`Minecraft.i()` (runTick) gates the controller update on `!isGamePaused && theWorld != null`,
and `Minecraft.run()` drives runTick from `new ir(20.0F)` — the same 20 Hz timer
`core/tick/tick_timer.hpp` transcribes. So: 20 Hz, world open, singleplayer, not paused.

## The pools — `eb`

`Minecraft.installResource(String key, File)` splits `key` at the **first** `/` and
switches on what is in front of it. There are exactly five categories:

| Category | Pool | `isGetRandomSound` |
|---|---|---|
| `sound`, `newsound` | sounds (`SoundManager`'s field) | true |
| `streaming` | records (`SoundManager`'s field) | **false** |
| `music`, `newmusic` | music (`SoundManager`'s field) | true |

`music` and `newmusic` feed the same pool, which is what the ticker draws from.

### Key derivation — a method of another original class

The original name, extension included, is kept. The stored key is the name up to its first dot.
For pools that pick at random, trailing digits are then stripped off the key one at a time.
Finally every `/` becomes a `.`.

So `sound/random/bow.ogg` → `random.bow`, `music/calm1.ogg` → `calm`,
`newmusic/hal4.ogg` → `hal`, and `streaming/13.mus` → `13` — the streaming pool keeps its
digits, because a record is asked for by its exact name and stripping them would make
`13` and `mellohi` unaddressable.

### The pool is the folder

a method of another original class — the no-argument form, and the ticker's only caller — ignores keys entirely:

It returns nothing if the pool is empty, and otherwise the entry at a uniformly random index of
the flat list.

It is uniform over the **flat list of every entry in the pool**. Two consequences, and
both look like bugs from the outside:

- A `newmusic/` with nine files and a `music/` with three makes a calm track 3/12 likely,
  not 50/50, and the balance shifts as the player adds files. **This is a1.1.2's
  behaviour. Do not "fix" it.**
- The set of tracks is whatever is on disk. The jar names none of them, so a hardcoded
  `calm1/calm2/calm3` table would be both wrong and unfaithful.

Note also that `eb` keeps **its own** `Random` (a field of another original class), separate from the SoundManager's
`SoundManager`'s field. The two streams are independent, so which track is picked cannot perturb when the
next one starts — which is why adding a file to the card does not move the schedule.

## Volumes and attenuation

| Path | Gain | Attenuation |
|---|---|---|
| Music, `SoundManager`'s method | `musicVolume`, **unscaled** | none (`ATTENUATION_NONE`) |
| Interface, `SoundManager`'s method | `min(vol,1) * 0.25f * soundVolume` | none |
| Positional, `SoundManager`'s method | `min(vol,1) * soundVolume` | linear, fade distance `16.0f`, or `16.0f*vol` when `vol > 1` |
| Record, `SoundManager`'s method | `0.5f * soundVolume` | linear, `64.0f` |

The `0.25f` on interface sounds is real and audible; music has no such factor. Positional
sounds rotate a source name `"sound_" + (id++ % 256)`. A record stops **both** `"streaming"`
and `"BgMusic"` before it starts, and uses `soundVolume` rather than `musicVolume` — but it
does **not** touch the music counter; the counter simply sees `playing("streaming")` and
freezes.

`SoundManager`'s method, called when the options screen changes, stops `"BgMusic"` outright when
`musicVolume` reaches zero and otherwise just resets its volume.

## Positional sounds — `SoundManager`'s method

The other half of the effect path, and it is a different sum rather than the same one scaled:

In order: return if not loaded or the sound volume option is zero; pick a random entry for the
name; if there is one and the volume is above zero, name a source `sound_` plus a counter that
wraps at 256. The audible range is 16, widened to 16 times the volume when the volume exceeds
1.0, and the source is positional in that case (the same rule decides whether it is a
priority source). Pitch is set; the volume is then clamped to 1.0 and multiplied by the
sound-volume option; play.

**No `0.25f`.** That factor belongs to the interface path alone, and it is why a footstep at
`volume * 0.15` is audible at all rather than a twentieth of nothing.

**A volume above 1 does not get louder, it gets further.** It stretches the fade distance and is
then clamped back to 1 at the source. Nothing in a1.1.2's block sounds uses that, but the
mechanism is transcribed because leaving it out would make one line of the method a lie.

The `2` in `newSource` is paulscode's `ATTENUATION_LINEAR`: the gain falls linearly to nothing
across the fade distance. That is a *library* rule rather than a jar one and it is the one part
of this that could not be read out of the class file — it is transcribed from what the constant
means, and named as such in `audio::positionalGain`.

**What is not carried across the seam is stereo.** Panning needs a listener *orientation*, and
`audio::Backend` has only `playSample(id, gain, pitch)`. So a sound here is attenuated by
distance and centred, and a footstep behind you sounds like a footstep beside you. The seam is
where that would be fixed, not the caller.

## Interface sounds — `SoundManager`'s method

The whole of "a button makes a noise", and it is nine lines. Transcribed from `of.class`:

In order: return if the sound system is not loaded or the sound volume option is zero; pick a
random entry for the name and return if there is none; advance a counter that wraps at 256 and
name the source `sound_` plus that counter; create a non-positional source (no attenuation);
clamp the volume to 1.0 and then multiply by **0.25**; set pitch; set the source volume to that
result times the sound-volume option; play.

The bytecode for the two lines that matter:

```
of.a(Ljava/lang/String;FF)V
     97: fload_2  98: fconst_1  99: fcmpl  100: ifle 105  103: fconst_1  104: fstore_2
    105: fload_2  106: ldc #2 // float 0.25f  108: fmul  109: fstore_2
```

**The clip comes before the multiply**, so asking for more than full volume is asking for
full and not for a quarter more than it. Positional sounds read a volume above 1 as a longer
fade distance instead; the interface path simply clips. `interfaceGain` in
`core/audio/sound_engine.hpp` is those two lines on their own, so they can be checked without
a decoder, a file or a console.

### Where the menu click comes from

`bh.class` is GuiScreen, and a method of another original class is `mouseClicked`:

```
 45: invokevirtual #53  // fk.c(Minecraft, int, int) -- button hit test
 48: ifeq 77                                       -- missed: no sound, no action
 61: getfield  #34      // Minecraft.sndManager
 64: ldc       #6       // String random.click
 66: fconst_1  67: fconst_1
 68: invokevirtual #66  // of.a(String, float, float)
```

Three facts fall out of that, and this port depends on all three:

* the sound is **`random.click` at volume 1.0 and pitch 1.0**, through the interface path,
  so the final gain is `0.25 * soundVolume`;
* it plays **only when the hit test passed**, and a field of another original class is false for a disabled button — a
  greyed-out button is silent;
* `fu` (GuiSlider) extends `fk`, so **clicking a slider clicks too**. The drag is silent; the
  press is not.

a1.1.2 also plays `random.click` for itself, quieter and lower, when a button block pops back
out or a lever is thrown: `random.click` at **volume 0.3, pitch 0.5** (`no.class`, `hu.class`,
`al.class`), with 0.6 for the on-state. Those are positional, but the (volume, pitch) pair is
the game's own second setting of the same sound.

### What this port does with it

| Gesture | Sound | Source |
|---|---|---|
| A on an enabled row, or an arrow that changes a value | `random.click`, 1.0 / 1.0 | a field of another original class, unchanged |
| The cursor moves to another row | `random.click`, 0.3 / 0.5 | **ours** — see below |
| B or START (leave the screen) | silence | Escape is silent in `bh` |
| A on a disabled row (Multiplayer) | silence | a field of another original class returned false |

**The cursor-move click is a deviation and there is no original to be faithful to.** a1.1.2's
menus are pointed at with a mouse and have no cursor to move, so nothing in the jar says what a
d-pad press should sound like. Rather than invent a tone, it plays the quieter, lower setting
the game already uses for its own buttons — so what a player hears is two settings of one
sound: quiet and low for moving, full and open for choosing. It is played in exactly one place,
`Menu::step` in `platform/ctr/menu.cpp`, and only when the cursor actually went somewhere.

The Sound screen's own volume row is the one place the click is *also* a preview: the engine is
told the new volume before the click is played, so the row is audible while it is being set.
a1.1.2 gets that for free, because its slider is clicked and a field of another original class fires after the value has
already been written.

### Effects are loaded before they are asked for

The one structural difference, and it is CONTRIBUTING's rule and not taste. a1.1.2 hands
paulscode a `URL` at the moment of the click and lets the library go and read the file. We
cannot: no filesystem access and no decompression in the per-frame path, and an SD read on the
frame a button was pressed is exactly that. So `SoundEngine::preloadSound(key)` decodes the
entries under a key at boot — `random/click.ogg` is a third of a second of audio, under 50 KB
decoded — and `playSoundFX` is afterwards a handle and two floats.

**A sound that was never preloaded is silence, deliberately, and not an error.** The preload
list is therefore an honest statement of what this port can make a noise about. It is one key
today because the menus are the only emitter; when `dig.*` needs three hundred files that
cannot all be resident, the answer is a decode request queued onto the audio worker, behind the
same `playSoundFX`.

On the console the samples live in their own `linearAlloc`, flushed out of the data cache once,
and play on ndsp channels 1–4 handed out round-robin. **The ring steals**, exactly as a1.1.2's
rotating `"sound_" + (id % 256)` steals: a click that sometimes does not happen would be worse
than one that cuts another off. Channel 0 stays the music voice and is touched only by the
decode thread, so the two threads never name the same channel — see `platform/ctr/audio.hpp`,
which also records the one thing that is an assumption rather than a fact: that libctru's
per-channel state is not a single structure two threads can tear. Its sources are not installed
here, and the decode thread is 3DS-only so ThreadSanitizer cannot reach it either.

`--audio-list <resources>` decodes the click on the host exactly as the console does at boot and
prints the two gains, so a player's folder can be checked before it is carried to a card. Against
the real a1.1.2 resources folder: `random.click` is **12,332 frames, 2 ch, 44,100 Hz — 280 ms**,
about 49 KB of PCM, and the gains are 0.250 for a choice and 0.075 for a move.

## Footsteps and breaking — `bb`, and the three places it is read

**There is no `dig.*` in a1.1.2.** That is worth saying first, because every later version has
one and the name is in every wiki: a1.1.2's `StepSound` (`bb`) has two getters and *both* of
them return `"step." + name` on the base class. Breaking a block plays a footstep. Only two of
the nine singletons disagree, and they do it by overriding one getter:

| singleton | walked on | broken | volume | pitch |
|---|---|---|---|---|
| `Block`'s field (Block's own default) | `step.stone` | `step.stone` | 1.0 | 1.0 |
| `Block`'s field wood | `step.wood` | `step.wood` | 1.0 | 1.0 |
| `Block`'s field gravel | `step.gravel` | `step.gravel` | 1.0 | 1.0 |
| `Block`'s field grass | `step.grass` | `step.grass` | 1.0 | 1.0 |
| `Block`'s field stone | `step.stone` | `step.stone` | 1.0 | 1.0 |
| `Block`'s field metal | `step.stone` | `step.stone` | 1.0 | **1.5** |
| `Block`'s field glass (`u`) | `step.stone` | **`random.glass`** | 1.0 | 1.0 |
| `Block`'s field cloth | `step.cloth` | `step.cloth` | 1.0 | 1.0 |
| `Block`'s field sand (`t`) | `step.sand` | **`step.gravel`** | 1.0 | 1.0 |

Which getter is which cannot be read off the bytecode — on the base class they are the same two
lines — so they are named by their callers and that mapping lives in `extract_blocks.py`'s
`MEMBER_MAP`. Get it backwards and glass breaks with a footstep instead of a smash, which is
the sort of thing that would survive a year.

The table is generated. `tools/extract_blocks.py` reads the singletons out of Block's own
initialiser, resolves the two overrides, follows `setStepSound` through the chained
constructor calls, and covers the six blocks that never call it: water and lava take Block's
constructor default, and the two staircases **copy the block they are modelled on** — which is
what makes wooden stairs sound like wood. All seventy rows were then checked against a running
jar, field by field.

### The three call sites, and their three sums

| Event | Sound | Volume | Pitch |
|---|---|---|---|
| Footstep, `Entity.moveEntity` | `getStepSound()` | `volume * 0.15F` | `pitch` |
| Break, `PlayerController.onPlayerDestroyBlock` | `getBreakSound()` | `(volume + 1) / 2` | `pitch * 0.8F` |
| Place, `ItemBlock.onItemUse` | **`getStepSound()`** | `(volume + 1) / 2` | `pitch * 0.8F` |

Placing is the odd one and it is not a slip here: `ItemBlock` plays the *step* getter at the
*break* loudness, so a block is put down with a footstep. Glass is placed with `step.stone` and
broken with `random.glass`. A door plays nothing at all, because `ItemDoor` is not an
`ItemBlock` and never reaches that line.

All three are positional, so none of them gets the interface path's `0.25f`. `core/audio/block_sound.hpp`
holds the three sums and nothing else.

### The footstep trigger — inside `moveEntity`

Each step, the horizontal distance moved is scaled by 0.6 and added to a running float
`distanceWalkedModified`. The trigger is skipped for an entity that cannot trigger walking, and for
one that is on the ground *and* sneaking. Otherwise the block under the feet is found by flooring
x and z and taking y minus a fixed 0.2 offset (a double literal) minus the entity's `yOffset`. When
the running distance exceeds the integer `nextStepDistance` and that block is not air, the
threshold goes up by one and a step plays: if the cell directly above the block is snow, snow's
step sound is used instead of the block's; otherwise the block's own step sound is used, but only
when the block's material is not a liquid. Either way the sound's volume is scaled by 0.15 and its
pitch is passed through. The block's own walked-on hook then runs, liquid or not.

Five things in twelve lines, and each is audible:

- **It measures what was covered, not what was asked for**, so walking into a wall is silent.
- **The distance accumulates outside the test**, so a sneaking player banks it and pays out the
  moment they stand up.
- **Sneaking on the ground is silent**, and that `flag` is read at the *top* of `moveEntity`,
  before anything has moved.
- **`nextStepDistance++`, not `= (int)distance + 1`**, so steps that were earned are paid out
  one at a time.
- **Snow on top wins outright** — including over a liquid, which the `else if` would otherwise
  have silenced.

`playSoundAtEntity` passes `posY - yOffset`, which is the feet.

Where it lives here: `PlayerBody::move` decides *which block* earned a step and leaves it in
`stepSoundDue`; the platform layer, which is the half with a listener, turns that into a cue
and plays it. `Block.onEntityWalking` is the one line not ported — its only a1.1.2 override is
redstone ore lighting up when trodden on, and `move()` takes a const world.

## Where the resources came from, and why they are not downloaded

`bf` (ThreadDownloadResources) fetched an S3 bucket listing from
`http://s3.amazonaws.com/MinecraftResources/` and walked it in two passes — `sound/` and
`newsound/` first, then everything else, music included — writing each key verbatim to
`<.minecraft>/resources/<key>`.

That server has been gone for years. The layout is what survives, so this port reads the
same tree from `sdmc:/3dalpha/resources/` and a folder copied from any alpha- or beta-era
install works unchanged. See [assets.md](assets.md#sounds).

## Where this port differs, and why

1. **No multiplayer gate.** There is no multiplayer client yet (M5). The pump is called
   from the singleplayer path only; when M5 arrives it must not add a second call site.
2. **The random sources are seeded explicitly.** a1.1.2 uses `new Random()` — the wall
   clock — so its schedule is not reproducible between runs and nothing depends on it
   being. `MusicTicker` and `SoundPool` take a seed; the game passes a clock, the tests
   pass a constant. Identical distribution, testable outcome. `TickWorld` makes the same
   trade for the same reason.
3. **Five voices of ndsp's twenty-four.** a1.1.2's SoundSystem carries far more sources
   than anything can use here. Channel 0 is music, 1–4 are one-shot effects, and the rest
   are free for the records and positional sounds that arrive with their first emitter.
4. **The DSP resamples.** Files are 44.1 kHz and ndsp mixes at ~32,728 Hz.
   `ndspChnSetRate` is set to the file's rate with `NDSP_INTERP_LINEAR` and the hardware
   does the rest; a CPU resampler on top of Tremor would be the second-largest cost in the
   subsystem and would buy nothing.
5. **`.mus` is not decoded.** Records use Mojang's own container (`ep`). `streaming/`
   entries are indexed and counted, and cannot be played.
6. **An all-digit file name.** `1.ogg` sends Java's digit-strip loop past the front of the
   string — `charAt(-1)`, an exception. `poolKey` stops at the empty string instead. A card
   can hold that file and a player is not owed a hang for it.
7. **Silence is not degradation.** With no `dspfirm.cdc`, no `resources/` folder, no
   decoder in the build, or `audio` off, the backend reports unavailable and `SoundManager`'s method's
   own first line — `if (!loaded)` — is what returns. The path is the original's.

## Not yet ported

Everything that needs something this port does not have yet. Listed so it is a gap and not
an oversight:

| Sound | Blocked on |
|---|---|
| Everything not preloaded | A decode queue on the audio worker — see *Effects are loaded before they are asked for*. Less pressing than it was: the whole a1.1.2 effect set is 35 files and the backend now holds 48 |
| `random.fizz` | Reachable now; `core/tick/fluid.cpp:230` names the site |
| `fire.fire` | Reachable now; `core/tick/fire.hpp` ticks |
| Ambient cave | Reachable now; the `soundCounter` is transcribed in [tick-a1.1.2.md](tick-a1.1.2.md) but not implemented |
| Ambient water and lava loops | `randomDisplayTick`, a client display path that does not exist here at all |
| `random.bow`, `random.explode`, `random.fuse` | No items, no entities, nothing lights TNT |
| `mob.*` | No mobs — M6 |
| Records | No jukebox, no items, and `.mus` is undecoded |

The seam they hang off is `audio::Backend` plus the pools above, so each arrives as a call
site rather than as a subsystem — as the menu click already did.

## What a block behaviour plays, and the seam it plays through

Six of these were on the list above and are not any more. They needed the same thing and it was not
a decoder: **`core/tick/` has no sound engine and must not grow one.** The tick runs on a worker and
the mixer does not, so a `SoundEngine&` reaching into a block behaviour would tie the two together.
`TickWorld::setSoundSink` is a function pointer and a context, set once by the frame loop, on
exactly the terms `setEntityQuery` and `setDropSink` already had — and unset means silence, which is
the honest answer for every headless tool here.

Every volume and pitch below is the class file's own:

| Event | Sound | Volume | Pitch | Position |
|---|---|--:|---|---|
| A pressure plate arming — a field of another original class | `random.click` | 0.3 | 0.6 | `(i+0.5, j+**0.1**, k+0.5)` |
| …and disarming | `random.click` | 0.3 | 0.5 | the same |
| A lever flicked — `BlockLever`'s method | `random.click` | 0.3 | 0.6 on, 0.5 off | `(i+0.5, j+0.5, k+0.5)` |
| A button pressed — a method of another original class | `random.click` | 0.3 | 0.6 | the same |
| …and letting itself back out, 20 ticks later | `random.click` | 0.3 | 0.5 | the same |
| A door — `BlockDoor`'s method | `random.door_open` / `random.door_close` | 1.0 | `rand.nextFloat() * 0.1F + 0.9F` | the same |
| Flint and steel — `ItemFlintAndSteel`'s field | `fire.ignite` | 1.0 | `rand.nextFloat() * 0.4F + 0.8F` | the cell it lit |

Two details worth keeping:

- **The plate plays from `j + 0.1`**, not `j + 0.5`. It is a quarter of a block tall and the
  original plays from just above the floor.
- **The door's pitch comes out of the world's own Random**, so opening a door moves the stream every
  later random tick reads. Flint and steel's comes out of `Item.itemRand`, a *different* generator —
  `core/item/use.cpp` keeps its own for exactly that reason.

This was reported as a plate feeling unresponsive rather than as a plate being silent, and that is
the point: a plate is flush with the floor and its whole state is one bit of metadata, so the click
**is** the feedback.
