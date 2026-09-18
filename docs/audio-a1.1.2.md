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
| `ia` | PlayerControllerSP |
| `ep` | the `.mus` codec, Mojang's own |

**The jar ships no audio at all.** Every non-class entry in it is a `.png`, plus
`default.gif`, `title/splashes.txt` and a zero-byte entry named `null`. There are no
`.ogg`, `.wav` or `.mus` files and no list of any. This matters more than it sounds: it
means no track name can be hardcoded, here or there — see *The pool is the folder* below.

## The music ticker — `of.i`, `of.c()`

This is the whole of "the random music that happens in the game". One `int`, one
`java.util.Random`, and nine lines.

`of.<init>` seeds it:

```java
this.i = this.h.nextInt(12000);      // 0..11999 ticks = 0..10 minutes
```

and `of.c()` runs it, once per tick:

```java
public void playMusicTicker() {
    if (!loaded || options.musicVolume == 0.0F) return;
    if (SoundSystem.playing("BgMusic"))  return;
    if (SoundSystem.playing("streaming")) return;
    if (this.i > 0) { --this.i; return; }
    SoundPoolEntry e = this.musicPool.getRandomSound();
    if (e == null) return;
    this.i = this.rand.nextInt(24000) + 24000;          // 24000..47999 = 20..40 min
    SoundSystem.backgroundMusic("BgMusic", e.url, e.name, false);
    SoundSystem.setVolume("BgMusic", options.musicVolume);
    SoundSystem.play("BgMusic");
}
```

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

Exactly one place in the jar: `ia.c()` (`PlayerControllerSP.onUpdate`), as its last
statement. `hq.c()` and `il.c()` — the base and multiplayer controllers — are `{ return; }`.
**a1.1.2 therefore plays no background music in multiplayer.**

`Minecraft.i()` (runTick) gates the controller update on `!isGamePaused && theWorld != null`,
and `Minecraft.run()` drives runTick from `new ir(20.0F)` — the same 20 Hz timer
`core/tick/tick_timer.hpp` transcribes. So: 20 Hz, world open, singleplayer, not paused.

## The pools — `eb`

`Minecraft.installResource(String key, File)` splits `key` at the **first** `/` and
switches on what is in front of it. There are exactly five categories:

| Category | Pool | `isGetRandomSound` |
|---|---|---|
| `sound`, `newsound` | sounds (`of.b`) | true |
| `streaming` | records (`of.c`) | **false** |
| `music`, `newmusic` | music (`of.d`) | true |

`music` and `newmusic` feed the same pool, which is what the ticker draws from.

### Key derivation — `eb.a(String, File)`

```java
String original = name;                          // kept, extension and all
name = name.substring(0, name.indexOf("."));     // "calm1.ogg"      -> "calm1"
if (this.isGetRandomSound)
    while (Character.isDigit(name.charAt(name.length() - 1)))
        name = name.substring(0, name.length() - 1);   //             -> "calm"
name = name.replaceAll("/", ".");                // "random/bow"     -> "random.bow"
```

So `sound/random/bow.ogg` → `random.bow`, `music/calm1.ogg` → `calm`,
`newmusic/hal4.ogg` → `hal`, and `streaming/13.mus` → `13` — the streaming pool keeps its
digits, because a record is asked for by its exact name and stripping them would make
`13` and `mellohi` unaddressable.

### The pool is the folder

`eb.a()` — the no-argument form, and the ticker's only caller — ignores keys entirely:

```java
public SoundPoolEntry getRandomSound() {
    if (this.all.size() == 0) return null;
    return (SoundPoolEntry) this.all.get(this.rand.nextInt(this.all.size()));
}
```

It is uniform over the **flat list of every entry in the pool**. Two consequences, and
both look like bugs from the outside:

- A `newmusic/` with nine files and a `music/` with three makes a calm track 3/12 likely,
  not 50/50, and the balance shifts as the player adds files. **This is a1.1.2's
  behaviour. Do not "fix" it.**
- The set of tracks is whatever is on disk. The jar names none of them, so a hardcoded
  `calm1/calm2/calm3` table would be both wrong and unfaithful.

Note also that `eb` keeps **its own** `Random` (`eb.c`), separate from the SoundManager's
`of.h`. The two streams are independent, so which track is picked cannot perturb when the
next one starts — which is why adding a file to the card does not move the schedule.

## Volumes and attenuation

| Path | Gain | Attenuation |
|---|---|---|
| Music, `of.c()` | `musicVolume`, **unscaled** | none (`ATTENUATION_NONE`) |
| Interface, `of.a(name, vol, pitch)` | `min(vol,1) * 0.25f * soundVolume` | none |
| Positional, `of.b(name, x,y,z, vol, pitch)` | `min(vol,1) * soundVolume` | linear, fade distance `16.0f`, or `16.0f*vol` when `vol > 1` |
| Record, `of.a(name, x,y,z, vol, pitch)` | `0.5f * soundVolume` | linear, `64.0f` |

The `0.25f` on interface sounds is real and audible; music has no such factor. Positional
sounds rotate a source name `"sound_" + (id++ % 256)`. A record stops **both** `"streaming"`
and `"BgMusic"` before it starts, and uses `soundVolume` rather than `musicVolume` — but it
does **not** touch the music counter; the counter simply sees `playing("streaming")` and
freezes.

`of.a()`, called when the options screen changes, stops `"BgMusic"` outright when
`musicVolume` reaches zero and otherwise just resets its volume.

## Positional sounds — `of.b(String, float, float, float, float, float)`

The other half of the effect path, and it is a different sum rather than the same one scaled:

```java
if (!loaded || options.soundVolume == 0.0F) return;
SoundPoolEntry entry = soundPool.getRandomSoundFromSoundPool(name);
if (entry != null && volume > 0.0F) {
    String src = "sound_" + (++counter % 256);
    float range = 16.0F;
    if (volume > 1.0F) range = 16.0F * volume;
    sndSystem.newSource(volume > 1.0F, src, entry.url, entry.name, false, x, y, z, 2, range);
    sndSystem.setPitch(src, pitch);
    if (volume > 1.0F) volume = 1.0F;
    sndSystem.setVolume(src, volume * options.soundVolume);
    sndSystem.play(src);
}
```

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

## Interface sounds — `of.a(String, float, float)`

The whole of "a button makes a noise", and it is nine lines. Transcribed from `of.class`:

```java
public void playSoundFX(String name, float volume, float pitch) {
    if (!loaded || options.soundVolume == 0.0F) return;
    SoundPoolEntry e = this.soundPool.getRandomSoundFromSoundPool(name);
    if (e == null) return;
    this.latestSoundID = (this.latestSoundID + 1) % 256;
    String src = "sound_" + this.latestSoundID;
    SoundSystem.newSource(false, src, e.url, e.name, false, 0,0,0, 0, 0);  // ATTENUATION_NONE
    if (volume > 1.0F) volume = 1.0F;
    volume = volume * 0.25F;
    SoundSystem.setPitch(src, pitch);
    SoundSystem.setVolume(src, volume * options.soundVolume);
    SoundSystem.play(src);
}
```

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

`bh.class` is GuiScreen, and `bh.a(int, int, int)` is `mouseClicked`:

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
* it plays **only when the hit test passed**, and `fk.c` is false for a disabled button — a
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
| A on an enabled row, or an arrow that changes a value | `random.click`, 1.0 / 1.0 | `bh.a`, unchanged |
| The cursor moves to another row | `random.click`, 0.3 / 0.5 | **ours** — see below |
| B or START (leave the screen) | silence | Escape is silent in `bh` |
| A on a disabled row (Multiplayer) | silence | `fk.c` returned false |

**The cursor-move click is a deviation and there is no original to be faithful to.** a1.1.2's
menus are pointed at with a mouse and have no cursor to move, so nothing in the jar says what a
d-pad press should sound like. Rather than invent a tone, it plays the quieter, lower setting
the game already uses for its own buttons — so what a player hears is two settings of one
sound: quiet and low for moving, full and open for choosing. It is played in exactly one place,
`Menu::step` in `platform/ctr/menu.cpp`, and only when the cursor actually went somewhere.

The Sound screen's own volume row is the one place the click is *also* a preview: the engine is
told the new volume before the click is played, so the row is audible while it is being set.
a1.1.2 gets that for free, because its slider is clicked and `bh.a` fires after the value has
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
| `ly.e` (Block's own default) | `step.stone` | `step.stone` | 1.0 | 1.0 |
| `ly.f` wood | `step.wood` | `step.wood` | 1.0 | 1.0 |
| `ly.g` gravel | `step.gravel` | `step.gravel` | 1.0 | 1.0 |
| `ly.h` grass | `step.grass` | `step.grass` | 1.0 | 1.0 |
| `ly.i` stone | `step.stone` | `step.stone` | 1.0 | 1.0 |
| `ly.j` metal | `step.stone` | `step.stone` | 1.0 | **1.5** |
| `ly.k` glass (`u`) | `step.stone` | **`random.glass`** | 1.0 | 1.0 |
| `ly.l` cloth | `step.cloth` | `step.cloth` | 1.0 | 1.0 |
| `ly.m` sand (`t`) | `step.sand` | **`step.gravel`** | 1.0 | 1.0 |

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

```java
this.distanceWalkedModified += (float)(MathHelper.sqrt_double(dx*dx + dz*dz) * 0.6D);
if (this.canTriggerWalking && !flag) {          // flag = onGround && isSneaking()
    int i = floor(posX), j = floor(posY - 0.20000000298023224D - yOffset), k = floor(posZ);
    int id = world.getBlockId(i, j, k);
    if (this.distanceWalkedModified > (float)this.nextStepDistance && id > 0) {
        this.nextStepDistance++;
        StepSound ss = Block.blocksList[id].stepSound;
        if (world.getBlockId(i, j + 1, k) == Block.snow.blockID) {
            ss = Block.snow.stepSound;
            world.playSoundAtEntity(this, ss.getStepSound(), ss.getVolume() * 0.15F, ss.getPitch());
        } else if (!Block.blocksList[id].blockMaterial.isLiquid()) {
            world.playSoundAtEntity(this, ss.getStepSound(), ss.getVolume() * 0.15F, ss.getPitch());
        }
        Block.blocksList[id].onEntityWalking(world, i, j, k, this);
    }
}
```

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
and plays it. `Block.onEntityWalking` — the last line of that block — is `tick::entityWalkedOnBlock`,
called by whoever owns the tick right after the move, because `move()` takes a const world and
must not be able to write blocks. **Two classes override it in a1.1.2**, not one: `mi`
(farmland), which reverts to dirt on `rand.nextInt(4) == 0`, and `ai` (redstone ore), which
lights up. `km`, the staircase, forwards to the block it is modelled on and so does nothing.
The cell is the one underfoot, unaffected by the snow and liquid substitutions above it:
`move()` leaves it in `steppedOn` / `stepBlock{X,Y,Z}`.

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
   are free for the positional sounds that arrive with their first emitter.

   **A record shares channel 0 with the music, and that is faithful rather than a
   compromise.** `of.a(String,FFFFF)` — playStreaming — stops `BgMusic` the moment a disc
   starts, and `of.c()` will not start a track while `playing("streaming")`, so the two never
   overlap in a1.1.2 either. What the port keeps of the streaming source is its own volume
   sum — `0.5F * options.soundVolume`, on **soundVolume** rather than musicVolume — and its
   own range, `16.0F * 4.0F`: four times a one-shot's, re-attenuated every tick as the
   listener moves.
4. **The DSP resamples.** Files are 44.1 kHz and ndsp mixes at ~32,728 Hz.
   `ndspChnSetRate` is set to the file's rate with `NDSP_INTERP_LINEAR` and the hardware
   does the rest; a CPU resampler on top of Tremor would be the second-largest cost in the
   subsystem and would buy nothing.
5. **`.mus` is decoded.** Records use Mojang's own container, which is class `ep` wrapping
   `hk` — an `InputStream` that XORs every byte with the high byte of a 32-bit state and
   then advances that state on the byte it just produced:

   ```
   key = fileName.hashCode();          // "13.mus", extension included
   b = buf[i] ^= (byte)(key >> 8);     // and b is the *decoded* byte
   key = key * 498729871 + 85731 * b;  // b sign-extended
   ```

   So it is an Ogg Vorbis file behind a one-byte cipher, and the state advancing on the
   plaintext is why a `.mus` is opened **unseekable**: byte n cannot be deciphered without
   every byte before it. `VorbisStream::createMus` is the whole of it, and
   `3dalpha --record-dump <resources> 13 out.wav` writes one out to listen to. The codec is
   chosen off the extension rather than by the caller: a beta-era `streaming/` folder holds
   `13.mus` *and* `13.ogg`, and the pool keys both as `13`.
6. **An all-digit file name.** `1.ogg` sends Java's digit-strip loop past the front of the
   string — `charAt(-1)`, an exception. `poolKey` stops at the empty string instead. A card
   can hold that file and a player is not owed a hang for it.
7. **Silence is not degradation.** With no `dspfirm.cdc`, no `resources/` folder, no
   decoder in the build, or `audio` off, the backend reports unavailable and `of.c()`'s
   own first line — `if (!loaded)` — is what returns. The path is the original's.

## Not yet ported

Everything that needs something this port does not have yet. Listed so it is a gap and not
an oversight:

| Sound | Blocked on |
|---|---|
| Everything not preloaded | A decode queue on the audio worker — see *Effects are loaded before they are asked for*. The list is `audio::preloadEffects` and it is **measured**, not estimated: `--audio-list` against a real resources folder decodes **110 samples / 6.9 MB** since the display tick landed (108 / 6.6 MB after the monsters, 72 / 3.4 MB before them), and `ctr::kMaxSamples` is 128 |
| Ambient cave | Reachable now; the `soundCounter` is transcribed in [tick-a1.1.2.md](tick-a1.1.2.md) but not implemented |
| Positional panning | A listener orientation the backend seam does not carry. Distance attenuation is transcribed; the stereo placement paulscode does around it is not |

**Three more rows left this table when the particles landed** (status.md 29), all through the
same door: `randomDisplayTick` is a real path now (`core/tick/display.cpp`), so the water
trickle (`jp.b`'s `liquid.water`, one dart in 64 over *flowing* water only) and the fire
crackle (`og.b`'s `fire.fire`, one in 24) are played where the jar plays them. `jp.i`'s
`random.fizz` -- the hiss when lava turns to stone -- went with the steam it belongs to, which
closes the site `core/tick/fluid.cpp` had been naming.

**The records row left this table on 2026-09-14**, and both halves of it went at once: `cv`
is a real block behaviour now (`core/tick/behaviour.cpp`, `core/tick/drop.cpp`) and `.mus`
decodes (`core/audio/vorbis_stream.cpp`). A skeleton killing a creeper is still a1.1.2's only
source of a disc, and it plays.

**Two more rows left it when TNT landed:** `random.fuse` now has a second caller that is not
a creeper — `q.b(Lcn;IIII)V`, a block of TNT being broken, burnt or powered — and
`random.explode` has its second: `jd.i()`, at strength 4 rather than a creeper's 3. Both keys
were already preloaded for the monsters, so TNT's sounds arrived as call sites and not as
samples. See `docs/status.md` and `core/entity/primed_tnt.hpp`.

**Four rows left this table when the monsters landed:** `random.explode` and `random.fuse` (a
creeper lights and goes off), the monsters' own `mob.*` (eleven keys across five kinds), and
`random.hurt` — which is the *player's* `ge.d()` and is still not attached to any health, but is
played now, because something can finally hit them. See `docs/status.md` §25.

The seam they hang off is `audio::Backend` plus the pools above, so each arrives as a call
site rather than as a subsystem — as the menu click already did.

## What an entity plays

The other half of the table below, and it needed one thing the block behaviours did not: a
place to say what an entity *is* doing, because the entity classes share their sounds by
inheritance rather than by table.

**Which entity makes a noise is derived, not chosen.** `kh.e_()` — Entity.onUpdate on the base
class — is one line, `y()`, so an entity reaches `onEntityUpdate` exactly when its own
`onUpdate` calls `super.onUpdate()`. Disassembling the eight entity classes this port has:

| Class | Is | Calls `super.onUpdate()` | So it splashes |
|---|---|---|---|
| `ge` | EntityLiving | yes | the player and all four animals |
| `dx` | EntityItem | yes | ✔ |
| `kg` | EntityArrow | yes | ✔ |
| `dc` | EntityBoat | yes | ✔ |
| `ff` | EntityFallingSand | **no** | ✘ |
| `jd` | EntityTNTPrimed | **no** | ✘ |
| `jc` | EntityPainting | **no** | ✘ |
| `oc` | EntityMinecart | **no** | ✘ |

So a falling sand block landing in a river is silent in a1.1.2, and so is a minecart — and so
is a block of primed TNT, whose `e_()` opens on the `prevPosX` store with no `super` call
anywhere in it. That is the version's, not a gap here.

| Event | Sound | Volume | Pitch | Position |
|---|---|--:|---|---|
| Entering water — `kh.y()` | `random.splash` | `min(1, sqrt(mx²·0.2 + my² + mz²·0.2) · 0.2)` | `1.0 + (r − r) · 0.4` | `posY − yOffset` |
| A burning entity getting wet — `kh.c()`'s tail | `random.fizz` | 0.7 | `1.6 + (r − r) · 0.4` | the same |
| A stack landing in lava — `dx.e_()` | `random.fizz` | **0.4** | **`2.0 + r · 0.4`** | the same |
| A stack picked up — `dx.b(dm)` | `random.pop` | 0.2 | `((r − r) · 0.7 + 1) · 2` | the same |
| An arrow striking anything — `kg.e_()`, both sites | `random.drr` | 1.0 | `1.2 / (r · 0.2 + 0.9)` | the same |
| A mob's idle, hurt and death — `ge.y()`, `ge.a(kh,I)` | `mob.*` per `MobDef` | `getSoundVolume` | `(r − r) · 0.2 + 1.0` | mid-height |
| A chicken laying — `mz.j()` | `mob.chickenplop` | 1.0 | `(r − r) · 0.2 + 1.0` | the same |
| A skeleton loosing — `cw.a(kh,F)` | `random.bow` | 1.0 | `1.0 / (r · 0.4 + 0.8)` | at the skeleton |
| A creeper lighting — `dd.a(kh,F)` | `random.fuse` | 1.0 | **0.5**, flat | the same |
| TNT lighting — `q.b(cn,IIII)` | `random.fuse` | 1.0 | **1.0**, flat | `posY − yOffset`, so `y + 0.01` |
| A blast — `je.a(...)` | `random.explode` | **4.0** | `(1 + (r − r) · 0.2) · 0.7` | the blast's centre |
| A slime taking off — `ma.b_()` | `mob.slime` | 0.6 | `((r − r) · 0.2 + 1.0) · 0.8` | at the slime |
| …and landing — `ma.e_()` | `mob.slime` | 0.6 | `((r − r) · 0.2 + 1.0) / 0.8` | the same |
| A slime doing damage — `ma.b(dm)` | `mob.slimeattack` | 1.0 | `(r − r) · 0.2 + 1.0` | the same |
| The player being hurt — `ge.a(kh,I)` | `random.hurt` | 1.0 | `(r − r) · 0.2 + 1.0` | at the player |

**A block of TNT and a creeper light at different pitches** — 1.0 against the creeper's flat
0.5 — which is the one way to tell by ear which of the two is about to go off. **TNT lit by a
blast makes no sound at all**: `q.c(Lcn;III)V` spawns the entity and stops, which is what makes
a chain reaction sound like a chain rather than a hundred fuses at once.

**A creeper and a slime have no idle sound at all**, and that is `ge.c()` returning null with no
override rather than a gap in the table. It is the whole of why one gets behind you. **Volume 4 is
the loudest thing in the game** and nothing else asks for more than 1. **A slime's two `mob.slime`
calls differ only in whether the pitch is multiplied or divided by 0.8**, so a hop is low and a
landing is high; and each has its own size gate — taking off needs size above 1, landing needs
size above 2, so the smallest slimes are silent either way.

Four things worth keeping:

- **The splash volume is a motion, and it is read before the tick moves anything.** `y()` is
  the first thing `onUpdate` does, so what it squares is the motion the *previous* tick left.
  A dive is loud, wading in is nearly silent, and the two come out of one expression. The 0.2
  weighting on the horizontal terms is why.
- **It is an edge, not a level.** `inWater` and `firstUpdate` are both needed: without the
  first a swimmer would splash twenty times a second, and without the second every boat afloat
  would announce itself on the tick a world was loaded.
- **A floating boat does flicker, and faithfully.** `dc` does not override `g_()`, which insets
  the box by 0.4 top and bottom — and a hull is 0.6 tall, so the probe is inverted and its
  answer changes as buoyancy rocks it across a cell boundary. It costs nothing audible because
  bobbing is ~0.04 of motion, which puts the splash near 0.03. Measured in
  `tests/entity_sound_test.cpp` rather than asserted.
- **The two fizzes are different sounds out of one file.** 0.7/1.6 for an entity going out,
  0.4/2.0 for a stack hitting lava — and the second draws once where the first draws twice, so
  every lava fizz is at or above pitch 2.

The three `random.drr`/`random.bow` numbers above are not new; what was new is that they were
*audible*. See *Effects are loaded before they are asked for*: a key that was never decoded
plays nothing, and neither of those keys — nor any `mob.*` — had ever been on the boot list.
`audio::preloadEffects` (`core/audio/effect_preload.hpp`) is now the one list, shared with the
host harness that measures what it costs, and half of it is derived from the block and mob
tables rather than typed out.

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
| A pressure plate arming — `al.h` | `random.click` | 0.3 | 0.6 | `(i+0.5, j+**0.1**, k+0.5)` |
| …and disarming | `random.click` | 0.3 | 0.5 | the same |
| A lever flicked — `no.a(...dm)` | `random.click` | 0.3 | 0.6 on, 0.5 off | `(i+0.5, j+0.5, k+0.5)` |
| A button pressed — `hu.a(...dm)` | `random.click` | 0.3 | 0.6 | the same |
| …and letting itself back out, 20 ticks later | `random.click` | 0.3 | 0.5 | the same |
| A door — `fw.a(Lcn;IIIZ)V` | `random.door_open` / `random.door_close` | 1.0 | `rand.nextFloat() * 0.1F + 0.9F` | the same |
| Flint and steel — `nx.a` | `fire.ignite` | 1.0 | `rand.nextFloat() * 0.4F + 0.8F` | the cell it lit |

Two details worth keeping:

- **The plate plays from `j + 0.1`**, not `j + 0.5`. It is a quarter of a block tall and the
  original plays from just above the floor.
- **The door's pitch comes out of the world's own Random**, so opening a door moves the stream every
  later random tick reads. Flint and steel's comes out of `Item.itemRand`, a *different* generator —
  `core/item/use.cpp` keeps its own for exactly that reason.

This was reported as a plate feeling unresponsive rather than as a plate being silent, and that is
the point: a plate is flush with the floor and its whole state is one bit of metadata, so the click
**is** the feedback.
