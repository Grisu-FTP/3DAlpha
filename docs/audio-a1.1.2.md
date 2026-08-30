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
3. **One music voice of ndsp's twenty-four.** a1.1.2's SoundSystem carries far more
   sources than anything can use here. Channel 0 is music; the rest are free for the
   effects and records that arrive with their first emitter.
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
   decoder in the build, or `audio` off, the backend reports unavailable and `of.c()`'s
   own first line — `if (!loaded)` — is what returns. The path is the original's.

## Not yet ported

Everything that needs something this port does not have yet. Listed so it is a gap and not
an oversight:

| Sound | Blocked on |
|---|---|
| `dig.*` (place and break), `random.click` in world | Block placement and breaking — M3 |
| `step.*` | A player body and collision, plus a `stepSound` column in `blocks.json` — M3 |
| `random.fizz` | Reachable now; `core/tick/fluid.cpp:230` names the site |
| `fire.fire`, `fire.ignite` | Reachable now; `core/tick/fire.hpp` ticks |
| Ambient cave | Reachable now; the `soundCounter` is transcribed in [tick-a1.1.2.md](tick-a1.1.2.md) but not implemented |
| Ambient water and lava loops | `randomDisplayTick`, a client display path that does not exist here at all |
| `random.bow`, `random.explode`, `random.fuse` | No items, no entities, nothing lights TNT |
| `mob.*` | No mobs — M6 |
| Records | No jukebox, no items, and `.mus` is undecoded |

The seam they will hang off is `audio::Backend` plus the pools above, so each arrives as a
call site rather than as a subsystem.
