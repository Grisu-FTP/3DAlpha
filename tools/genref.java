// Reference-vector generator for seed-exact world generation.
//
// Run by a maintainer, never by the build and never by a player:
//
//     java tools/genref.java --random > tests/java_random_vectors.hpp
//
// The output is checked in. This is the same arrangement as
// tools/extract_blocks.py: derive once from an authoritative source, commit the
// result, and keep the tool around so the derivation can be re-run and argued
// with. See CONTRIBUTING.md.
//
// Why a real JVM rather than values copied out of the documentation: the whole
// point of the exercise is to prove our C++ agrees with what actually runs, and
// a transcription error in a hand-copied constant would be invisible in exactly
// the way this file exists to prevent. Floating-point results are emitted as
// raw bit patterns for the same reason -- a decimal round trip through the text
// could hide a one-ulp disagreement, which is precisely the size of error that
// desynchronises a noise lattice.
//
// --random needs no jar: java.util.Random is part of Java, and its algorithm is
// specified. The jar-reflecting modes (noise lattices, whole chunks) come with
// stage 2 and will take a --jar path.

import java.util.Random;

public class genref {

    // Bounds chosen to exercise every branch of nextInt(bound): a power of two
    // takes the high-bits shortcut, a small odd bound takes the modulus path
    // with a vanishing rejection rate, and 2^30+1 rejects roughly half the time
    // so the retry loop is actually covered rather than merely present.
    private static final int[] BOUNDS = {16, 17, 3, 1073741825};

    private static final long[] SEEDS = {
        0L, 1L, -1L, 42L, 1234567890L, -8675309L, 8676971110L, Long.MIN_VALUE, Long.MAX_VALUE,
    };

    private static final int N = 8;

    public static void main(String[] args) {
        if (args.length == 1 && args[0].equals("--random")) {
            emitRandom();
            return;
        }
        if (args.length == 1 && args[0].equals("--strictmath")) {
            emitStrictMath();
            return;
        }
        if (args.length == 3 && args[0].equals("--jar") && args[2].equals("--noise")) {
            emitNoise(args[1]);
            return;
        }
        if (args.length == 3 && args[0].equals("--jar") && args[2].equals("--terrain")) {
            emitTerrain(args[1]);
            return;
        }
        if (args.length == 1 && args[0].equals("--sintable")) {
            emitSinTable();
            return;
        }
        if (args.length == 4 && args[0].equals("--jar") && args[2].equals("--world")) {
            emitWorld(args[1], args[3]);
            return;
        }
        if (args.length == 4 && args[0].equals("--jar") && args[2].equals("--ore")) {
            emitOre(args[1], args[3]);
            return;
        }
        if (args.length == 4 && args[0].equals("--jar") && args[2].equals("--liquid")) {
            emitLiquid(args[1], args[3]);
            return;
        }
        if (args.length == 4 && args[0].equals("--jar") && args[2].equals("--plant")) {
            emitPlant(args[1], args[3]);
            return;
        }
        if (args.length == 4 && args[0].equals("--jar") && args[2].equals("--light")) {
            emitLight(args[1], args[3]);
            return;
        }
        if (args.length == 4 && args[0].equals("--jar") && args[2].equals("--sky")) {
            emitSky(args[1], args[3]);
            return;
        }
        if (args.length == 4 && args[0].equals("--jar") && args[2].equals("--flower")) {
            emitFlower(args[1], args[3]);
            return;
        }
        if (args.length == 3 && args[0].equals("--jar") && args[2].equals("--opaquecube")) {
            emitOpaqueCube(args[1]);
            return;
        }
        if (args.length == 4 && args[0].equals("--jar") && args[2].equals("--tree")) {
            emitTree(args[1], args[3]);
            return;
        }
        if (args.length == 4 && args[0].equals("--jar") && args[2].equals("--bigtree")) {
            emitBigTree(args[1], args[3]);
            return;
        }
        if (args.length == 4 && args[0].equals("--jar") && args[2].equals("--dungeon")) {
            emitDungeon(args[1], args[3]);
            return;
        }
        if (args.length == 4 && args[0].equals("--jar") && args[2].equals("--populate")) {
            emitPopulate(args[1], args[3]);
            return;
        }
        if (args.length == 4 && args[0].equals("--jar") && args[2].equals("--generate")) {
            emitGenerate(args[1], args[3]);
            return;
        }
        if (args.length == 4 && args[0].equals("--jar") && args[2].equals("--collision")) {
            emitCollision(args[1], args[3]);
            return;
        }
        if (args.length == 4 && args[0].equals("--jar") && args[2].equals("--player")) {
            emitPlayerBody(args[1], args[3]);
            return;
        }
        if (args.length == 4 && args[0].equals("--jar") && args[2].equals("--raytrace")) {
            emitRayTrace(args[1], args[3]);
            return;
        }
        if (args.length == 4 && args[0].equals("--jar") && args[2].equals("--place")) {
            emitPlacement(args[1], args[3]);
            return;
        }
        if (args.length == 4 && args[0].equals("--jar") && args[2].equals("--items")) {
            emitItems(args[1], args[3]);
            return;
        }
        if (args.length == 3 && args[0].equals("--jar") && args[2].equals("--drops")) {
            emitDrops(args[1]);
            return;
        }
        if (args.length == 3 && args[0].equals("--jar") && args[2].equals("--art")) {
            emitArt(args[1]);
            return;
        }
        if (args.length == 3 && args[0].equals("--jar") && args[2].equals("--harvest")) {
            emitHarvest(args[1]);
            return;
        }
        if (args.length == 3 && args[0].equals("--jar") && args[2].equals("--recipes")) {
            emitRecipes(args[1]);
            return;
        }
        System.err.println("       java tools/genref.java --jar <client.jar> --generate <scratch-dir>");
        System.err.println("usage: java tools/genref.java --random     > tests/java_random_vectors.hpp");
        System.err.println("       java tools/genref.java --strictmath > tests/strict_math_vectors.hpp");
        System.err.println("       java tools/genref.java --jar <client.jar> --noise   > tests/noise_vectors.hpp");
        System.err.println("       java tools/genref.java --jar <client.jar> --terrain > tests/terrain_vectors.hpp");
        System.err.println("       java tools/genref.java --sintable > tests/sin_table_vectors.hpp");
        System.err.println("       java tools/genref.java --jar <client.jar> --world <scratch-dir> > tests/world_vectors.hpp");
        System.err.println("       java tools/genref.java --jar <client.jar> --ore <scratch-dir> > tests/ore_vectors.hpp");
        System.err.println("       java tools/genref.java --jar <client.jar> --liquid <scratch-dir> > tests/liquid_vectors.hpp");
        System.err.println("       java tools/genref.java --jar <client.jar> --collision <scratch-dir> > tests/collision_box_vectors.hpp");
        System.err.println("       java tools/genref.java --jar <client.jar> --player <scratch-dir> > tests/player_body_vectors.hpp");
        System.err.println("       java tools/genref.java --jar <client.jar> --raytrace <scratch-dir> > tests/ray_trace_vectors.hpp");
        System.err.println("       java tools/genref.java --jar <client.jar> --place <scratch-dir> > tests/placement_vectors.hpp");
        System.err.println("       java tools/genref.java --jar <client.jar> --items <scratch-dir> > data/a1.1.2/items.json");
        System.err.println("       java tools/genref.java --jar <client.jar> --drops > data/a1.1.2/drops.json");
        System.err.println("       java tools/genref.java --jar <client.jar> --art > data/a1.1.2/paintings.json");
        System.exit(2);
    }

    // ---------------------------------------------------------------------
    // Noise, straight out of the original's own classes
    // ---------------------------------------------------------------------
    //
    // The two generators are reflected rather than reimplemented, which is the
    // entire point: a reimplementation checked against a reimplementation
    // proves nothing. `v` (NoiseGeneratorPerlin) and `lp` (NoiseGeneratorOctaves)
    // both take only a java.util.Random, so unlike the chunk provider they need
    // no World instance and load straight out of the jar.
    //
    // Everything is emitted as raw bit patterns, inputs included. 684.412 is not
    // exactly representable as a decimal literal, and a test whose *inputs* went
    // through a decimal round trip would be checking the wrong function.

    private static final String PERLIN = "v";
    private static final String OCTAVES = "lp";

    // The descriptor of the lattice fill on `v`, and of the octave loop on `lp`.
    // Looked up by descriptor rather than by name: the names are single letters
    // and several methods share them.
    private static final String PERLIN_FILL_DESC = "([DDDDIIIDDDD)V";
    private static final String OCTAVES_FILL_DESC = "([DDDDIIIDDD)[D";

    private static void emitNoise(String jarPath) {
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> perlin = loader.loadClass(PERLIN);
            Class<?> octaves = loader.loadClass(OCTAVES);

            p("// Generated by tools/genref.java --jar <client.jar> --noise. Do not edit by hand.");
            p("//");
            p("// Taken from the a1.1.2 client jar's own noise generators, by reflection --");
            p("// class `v` (NoiseGeneratorPerlin) and `lp` (NoiseGeneratorOctaves). Doubles are");
            p("// bit patterns on both sides: a decimal round trip on the *inputs* would mean");
            p("// testing the function against slightly different arguments than the JVM saw.");
            p("//");
            p("// Regenerate with:");
            p("//   java tools/genref.java --jar <client.jar> --noise > tests/noise_vectors.hpp");
            p("");
            p("#pragma once");
            p("");
            p("#include \"core/util/types.hpp\"");
            p("");
            p("namespace mc::test {");
            p("");

            emitPerlinConstruction(perlin);
            emitOctaveCases(octaves);

            p("}  // namespace mc::test");
        } catch (Exception e) {
            System.err.println("genref --noise failed: " + e);
            e.printStackTrace();
            System.exit(1);
        }
    }

    // The permutation table and the three offsets, so that a failure says
    // "construction" or "evaluation" instead of just "noise".
    private static void emitPerlinConstruction(Class<?> perlin) throws Exception {
        java.lang.reflect.Constructor<?> ctor = perlin.getConstructor(Random.class);
        java.lang.reflect.Field permField = fieldOfType(perlin, int[].class);
        java.lang.reflect.Field[] offsets = doubleFields(perlin);
        permField.setAccessible(true);
        for (java.lang.reflect.Field f : offsets) {
            f.setAccessible(true);
        }

        long[] seeds = {0L, 1L, 1234567890L, -8675309L};

        p("struct PerlinVector {");
        p("    i64 seed;");
        p("    u64 offsetBits[3];");
        p("    i32 permutation[512];");
        p("};");
        p("");
        p("inline constexpr int kPerlinVectorCount = " + seeds.length + ";");
        p("");
        p("inline constexpr PerlinVector kPerlinVectors[kPerlinVectorCount] = {");
        for (long seed : seeds) {
            Object gen = ctor.newInstance(new Random(seed));
            int[] perm = (int[]) permField.get(gen);

            p("    {");
            p("        " + lit(seed) + ",");
            StringBuilder off = new StringBuilder();
            for (int i = 0; i < 3; i++) {
                off.append(i == 0 ? "" : ", ")
                   .append(hex64(Double.doubleToRawLongBits(offsets[i].getDouble(gen))));
            }
            p("        {" + off + "},");
            p("        {");
            for (int row = 0; row < perm.length; row += 16) {
                StringBuilder sb = new StringBuilder();
                for (int i = row; i < row + 16 && i < perm.length; i++) {
                    sb.append(i == row ? "" : ", ").append(perm[i]);
                }
                p("            " + sb + ",");
            }
            p("        },");
            p("    },");
        }
        p("};");
        p("");
    }

    // The lattice fills the chunk provider actually asks for. Shapes and scales
    // are the ones in ChunkProviderGenerate.initializeNoiseField, plus a couple
    // of degenerate shapes (a single point, a flat slab) that catch off-by-one
    // errors in the loop bounds without needing a whole chunk to see them.
    private static void emitOctaveCases(Class<?> octaves) throws Exception {
        java.lang.reflect.Constructor<?> ctor = octaves.getConstructor(Random.class, int.class);
        java.lang.reflect.Method fill = methodByDescriptor(octaves, OCTAVES_FILL_DESC);
        fill.setAccessible(true);

        // {seed, octaveCount, x, y, z, xs, ys, zs, xScale, yScale, zScale}
        double[][] cases = {
            // The real thing: the 5x17x5 lattice, at the constants nw uses.
            {0, 16, 0, 0, 0, 5, 17, 5, 684.412 / 80.0, 684.412 / 160.0, 684.412 / 80.0},
            {1234567890L, 16, 5, 0, 5, 5, 17, 5, 684.412 / 80.0, 684.412 / 160.0, 684.412 / 80.0},
            // The 2D fields: depth and scale noise, ys = 1.
            {0, 10, 0, 0, 0, 5, 1, 5, 200.0, 1.0, 200.0},
            {-8675309L, 8, -10, 0, 7, 5, 1, 5, 1.121, 1.121, 1.121},
            // The surface pass, at its own scale.
            {42, 4, 16, 0, 16, 16, 1, 16, 0.03125, 1.0, 0.03125},
            // Degenerate shapes.
            {7, 1, 0, 0, 0, 1, 1, 1, 1.0, 1.0, 1.0},
            {7, 3, 0.5, 0.25, -0.75, 2, 3, 4, 0.7, 0.9, 1.3},
            // Negative coordinates, which exercise the floor path.
            {99, 6, -1000.5, -20.25, -333.125, 4, 5, 4, 1.7, 2.3, 1.7},
        };

        StringBuilder table = new StringBuilder();
        for (int c = 0; c < cases.length; c++) {
            double[] k = cases[c];
            long seed = (long) k[0];
            int octaveCount = (int) k[1];
            double x = k[2], y = k[3], z = k[4];
            int xs = (int) k[5], ys = (int) k[6], zs = (int) k[7];
            double xScale = k[8], yScale = k[9], zScale = k[10];

            Object gen = ctor.newInstance(new Random(seed), octaveCount);
            double[] out = (double[]) fill.invoke(gen, null, x, y, z, xs, ys, zs,
                                                  xScale, yScale, zScale);

            p("inline constexpr u64 kNoiseExpected" + c + "[" + out.length + "] = {");
            for (int row = 0; row < out.length; row += 4) {
                StringBuilder sb = new StringBuilder();
                for (int i = row; i < row + 4 && i < out.length; i++) {
                    sb.append(i == row ? "" : ", ")
                      .append(hex64(Double.doubleToRawLongBits(out[i])));
                }
                p("    " + sb + ",");
            }
            p("};");
            p("");

            table.append("    {")
                 .append(lit(seed)).append(", ")
                 .append(octaveCount).append(", ")
                 .append(hex64(Double.doubleToRawLongBits(x))).append(", ")
                 .append(hex64(Double.doubleToRawLongBits(y))).append(", ")
                 .append(hex64(Double.doubleToRawLongBits(z))).append(", ")
                 .append(xs).append(", ").append(ys).append(", ").append(zs).append(", ")
                 .append(hex64(Double.doubleToRawLongBits(xScale))).append(", ")
                 .append(hex64(Double.doubleToRawLongBits(yScale))).append(", ")
                 .append(hex64(Double.doubleToRawLongBits(zScale))).append(", ")
                 .append(out.length).append(", kNoiseExpected").append(c)
                 .append("},\n");
        }

        p("struct NoiseCase {");
        p("    i64 seed;");
        p("    int octaves;");
        p("    u64 xBits;");
        p("    u64 yBits;");
        p("    u64 zBits;");
        p("    int xSize;");
        p("    int ySize;");
        p("    int zSize;");
        p("    u64 xScaleBits;");
        p("    u64 yScaleBits;");
        p("    u64 zScaleBits;");
        p("    int count;");
        p("    const u64* expected;");
        p("};");
        p("");
        p("inline constexpr int kNoiseCaseCount = " + cases.length + ";");
        p("");
        p("inline constexpr NoiseCase kNoiseCases[kNoiseCaseCount] = {");
        System.out.print(table);
        p("};");
        p("");
    }

    // ---------------------------------------------------------------------
    // One generator at a time, against a real World
    // ---------------------------------------------------------------------
    //
    // Population's nine generators can only be compared as a group once all of
    // them exist -- a partially populated chunk matches nothing. This mode gets
    // ground truth for **one** generator so they can be built and verified one
    // at a time.
    //
    // The trick is to make the input state trivial to describe. A real World is
    // constructed and a box around the target is filled with stone, so the
    // fixture does not have to carry terrain: our side fills the same box the
    // same way and runs the same generator with the same seed. The box is far
    // larger than an ore vein can reach (a vein spans at most about seven
    // blocks from its centre), so nothing escapes it and the comparison is
    // exact.
    //
    // Everything else is the real thing: real World.setBlock, which refuses
    // y outside [0,128) and writes across chunk boundaries by generating the
    // neighbour, and a real java.util.Random.

    private static final int ORE_BOX = 24;   // half-extent, comfortably > any vein
    // Not a block id -- flags a clay case, which needs a different box.
    private static final int CLAY_MARKER = -1;

    // ---------------------------------------------------------------------
    // WorldGenLiquids (`nn`) -- the water and lava springs
    // ---------------------------------------------------------------------
    //
    // **This one gets an exhaustive oracle rather than sampled cases, because
    // it can have one.** `nn.a` draws no random numbers at all -- the Random it
    // is handed is never touched -- and it reads exactly seven blocks: the one
    // above, the one below, the centre, and the four horizontal neighbours. So
    // its behaviour is a pure function of seven block ids, and the whole truth
    // table is small enough to enumerate and check.
    //
    // The alphabet is {air, stone, dirt}: air and stone because the predicate
    // counts them, and dirt to stand for "some third thing", which is the case
    // a hand-picked fixture is most likely to leave out. Centre and the four
    // horizontals give 3^5 = 243 combinations, and the above/below pair gets
    // its own four, for 247 cases. Each is one row of seven inputs and the id
    // that ended up at the centre.
    //
    // Every case is run at its own coordinate so the previous one cannot leak
    // into it, and only the seven cells the generator reads are written -- what
    // the terrain generator left around them is irrelevant, because nothing
    // else is looked at.

    private static void emitLiquid(String jarPath, String scratchDir) {
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> worldClass = loader.loadClass(WORLD);
            Class<?> liquids = loader.loadClass("nn");

            java.lang.reflect.Constructor<?> worldCtor =
                worldClass.getConstructor(java.io.File.class, String.class, long.class);
            java.lang.reflect.Field snowField = worldClass.getField(SNOW_FIELD);
            java.lang.reflect.Method getChunk = worldClass.getMethod("b", int.class, int.class);
            java.lang.reflect.Method getBlock =
                worldClass.getMethod("a", int.class, int.class, int.class);
            java.lang.reflect.Method setBlock =
                worldClass.getMethod("a", int.class, int.class, int.class, int.class);

            java.lang.reflect.Constructor<?> liquidCtor = liquids.getConstructor(int.class);
            java.lang.reflect.Method generate =
                liquids.getMethod("a", worldClass, Random.class, int.class, int.class, int.class);

            final int AIR = 0, STONE = 1, DIRT = 3;
            final int WATER = 8, LAVA = 10;
            final int[] ALPHABET = {AIR, STONE, DIRT};

            // {above, below, centre, west, east, north, south, placedId}
            java.util.List<int[]> patterns = new java.util.ArrayList<>();

            // The above/below early returns, with a horizontal pattern that
            // would otherwise place: three stone sides and one air side.
            for (int above : new int[]{STONE, DIRT}) {
                for (int below : new int[]{STONE, DIRT}) {
                    patterns.add(new int[]{above, below, AIR, STONE, STONE, STONE, AIR, 0});
                }
            }

            // Centre and the four horizontals, exhaustively.
            for (int centre : ALPHABET) {
                for (int west : ALPHABET) {
                    for (int east : ALPHABET) {
                        for (int north : ALPHABET) {
                            for (int south : ALPHABET) {
                                patterns.add(new int[]{STONE, STONE, centre, west, east, north,
                                                       south, 0});
                            }
                        }
                    }
                }
            }

            java.io.File dir = new java.io.File(scratchDir, "liquid");
            deleteTree(dir);
            dir.mkdirs();
            Object world = worldCtor.newInstance(dir, "genref", 555000111L);
            snowField.setBoolean(world, false);

            // Two liquids over the same patterns proves the id is written
            // rather than assumed, and costs nothing.
            int[] liquidIds = {WATER, LAVA};

            StringBuilder table = new StringBuilder();
            int emitted = 0;
            int placedCount = 0;
            int slot = 0;
            for (int liquidId : liquidIds) {
                for (int[] pattern : patterns) {
                    // Far from spawn, and spaced so no two cases share a cell.
                    final int x = 600 + (slot % 64) * 4;
                    final int z = 600 + (slot / 64) * 4;
                    final int y = 40;
                    slot++;

                    getChunk.invoke(world, x >> 4, z >> 4);

                    setBlock.invoke(world, x, y + 1, z, pattern[0]);
                    setBlock.invoke(world, x, y - 1, z, pattern[1]);
                    setBlock.invoke(world, x, y, z, pattern[2]);
                    setBlock.invoke(world, x - 1, y, z, pattern[3]);
                    setBlock.invoke(world, x + 1, y, z, pattern[4]);
                    setBlock.invoke(world, x, y, z - 1, pattern[5]);
                    setBlock.invoke(world, x, y, z + 1, pattern[6]);

                    Object gen = liquidCtor.newInstance(liquidId);
                    // The Random is required by the signature and never read.
                    // Passing a fresh one per case and checking afterwards that
                    // it is untouched is what proves that claim, below.
                    Random unused = new Random(12345L);
                    generate.invoke(gen, world, unused, x, y, z);

                    final int got = ((Integer) getBlock.invoke(world, x, y, z)).intValue();
                    if (got == liquidId) {
                        placedCount++;
                    }

                    // The stream must be exactly where it started. If the
                    // generator ever drew from it, this catches it here rather
                    // than as a mystery desync three generators later.
                    if (unused.nextLong() != new Random(12345L).nextLong()) {
                        throw new IllegalStateException("nn consumed the random stream");
                    }

                    table.append("    {")
                         .append(pattern[0]).append(", ").append(pattern[1]).append(", ")
                         .append(pattern[2]).append(", ").append(pattern[3]).append(", ")
                         .append(pattern[4]).append(", ").append(pattern[5]).append(", ")
                         .append(pattern[6]).append(", ")
                         .append(liquidId).append(", ").append(got).append("},\n");
                    emitted++;
                }
            }

            p("// Generated by tools/genref.java --jar <client.jar> --liquid <scratch-dir>.");
            p("// Do not edit by hand.");
            p("//");
            p("// WorldGenLiquids (`nn`) against a **real a1.1.2 World**, exhaustively: the");
            p("// generator reads exactly seven blocks and draws no random numbers, so this is");
            p("// its whole truth table over {air, stone, dirt} rather than a sample.");
            p("//");
            p("// The harness also asserts, per case, that the Random it hands the generator");
            p("// comes back untouched.");
            p("//");
            p("// Regenerate with:");
            p("//   java tools/genref.java --jar <client.jar> --liquid /tmp/genref-scratch");
            p("//     redirected to tests/liquid_vectors.hpp");
            p("");
            p("#pragma once");
            p("");
            p("#include \"core/util/types.hpp\"");
            p("");
            p("namespace mc::test {");
            p("");
            p("struct LiquidCase {");
            p("    u8 above;");
            p("    u8 below;");
            p("    u8 centre;");
            p("    u8 west;");
            p("    u8 east;");
            p("    u8 north;");
            p("    u8 south;");
            p("    u8 liquidId;");
            p("    u8 placed;    // what ended up at the centre: the liquid, or what was there");
            p("};");
            p("");
            p("inline constexpr int kLiquidCaseCount = " + emitted + ";");
            p("");
            p("// How many of them actually placed the spring. A fixture where nothing ever");
            p("// places would pass a generator that always refuses.");
            p("inline constexpr int kLiquidPlacedCount = " + placedCount + ";");
            p("");
            p("inline constexpr LiquidCase kLiquidCases[kLiquidCaseCount] = {");
            System.out.print(table);
            p("};");
            p("");
            p("}  // namespace mc::test");
        } catch (Exception e) {
            System.err.println("genref --liquid failed: " + e);
            e.printStackTrace();
            System.exit(1);
        }
    }

    // ---------------------------------------------------------------------
    // WorldGenReed (`es`) and WorldGenCactus (`da`)
    // ---------------------------------------------------------------------
    //
    // Unlike the liquids, **these two draw from the shared Random, and how much
    // they draw depends on what they find.** Reeds spend two draws per try
    // choosing a spot and then two more on a height, but only when the spot is
    // air; cactus does the same with a third draw for its own y. So a
    // transcription that gets the placement right and the draw *count* wrong
    // produces a correct-looking plant and then moves every generator that runs
    // after it.
    //
    // That is why each case records a **stream fingerprint**: after the
    // generator returns, the harness draws one more long and stores it. Our
    // side does the same, and a mismatch says "you consumed a different number
    // of random numbers" in one comparison, without needing to know which draw
    // went missing.
    //
    // The scene is built procedurally from a small enum so the fixture does not
    // have to carry terrain. **Both sides implement the same four scenes**, and
    // they have to agree exactly -- see buildPlantScene here and Scene::build in
    // tests/plant_test.cpp.

    // Reeds reach three blocks from the origin and cactus seven, so twelve
    // covers both with room to spare and keeps the site probe below cheap.
    private static final int PLANT_BOX = 12;

    // **The scene is built underground, in solid rock, and the site is probed
    // for rather than picked. Both of those are the fix for the same bug.**
    //
    // The first attempt built at sea level and the second at y = 100, and both
    // died the same way: a StackOverflowError thousands of frames deep, cycling
    // through `cn.e` and `kn.a`. `kn` is not a block -- it is a light-update
    // record, holding a light type and a bounding box -- so what was recursing
    // was **a1.1.2's light propagation**, not the world generator and not, as it
    // first looked, flowing water.
    //
    // Writing a 41x41 slab of solid blocks into open sky is about the worst
    // possible input to that: every column below it loses direct skylight, and
    // the relight walks the lot recursively. Underground it is a non-event,
    // because skylight there is already zero and blocklight only travels as far
    // as the chamber we carve.
    //
    // The probe then rules out the other half of the problem. A box that
    // happens to contain water, lava or a cave brings back both cascades -- a
    // fluid re-evaluating as its neighbours are replaced, and light flooding
    // into or out of an opening -- so a site is accepted only if every block in
    // it, plus a two-block margin, is already solid and dry.
    private static final int PLANT_GROUND_TOP = 40;

    // How far below the surface to fill. Cactus reaches three blocks down from
    // the origin and reads one below that, so four layers is enough.
    private static final int PLANT_GROUND_DEPTH = 4;

    // How far above the surface the scene is cleared to air.
    private static final int PLANT_HEADROOM = 8;

    private static final int SCENE_SAND = 0;         // sand up to the surface
    private static final int SCENE_GRASS = 1;        // dirt below, grass on top
    private static final int SCENE_GRASS_WATER = 2;  // grass, plus a small still-water pool
    private static final int SCENE_SAND_SCATTER = 3; // sand, plus scattered stone on top

    // A small pool rather than a channel across the whole box: reeds only reach
    // three blocks from the origin, so this is all the shoreline they can find,
    // and a pool well inside the box cannot touch the world outside it.
    private static final int WATER_X0 = -3, WATER_X1 = 3;
    private static final int WATER_Z0 = 2, WATER_Z1 = 3;

    // Is every block of the box, plus a margin, solid rock? See the note above
    // on why anything else is unusable.
    private static boolean siteIsSolidAndDry(java.lang.reflect.Method getBlock, Object world,
                                             int x, int z) throws Exception {
        final int margin = 2;
        for (int bx = x - PLANT_BOX - margin; bx <= x + PLANT_BOX + margin; bx++) {
            for (int bz = z - PLANT_BOX - margin; bz <= z + PLANT_BOX + margin; bz++) {
                for (int by = PLANT_GROUND_TOP - PLANT_GROUND_DEPTH - margin;
                     by <= PLANT_GROUND_TOP + PLANT_HEADROOM + margin; by++) {
                    final int id = ((Integer) getBlock.invoke(world, bx, by, bz)).intValue();
                    if (id == 0 || id == 8 || id == 9 || id == 10 || id == 11) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    // **Two passes, and the order matters.** The solid ground goes down first
    // and the pool second, so that when the water is written it is already
    // enclosed on every side by blocks we placed.
    private static void buildPlantScene(java.lang.reflect.Method setBlock, Object world, int scene,
                                        int x, int z) throws Exception {
        final int top = PLANT_GROUND_TOP;
        final boolean sandy = scene == SCENE_SAND || scene == SCENE_SAND_SCATTER;

        for (int bx = x - PLANT_BOX; bx <= x + PLANT_BOX; bx++) {
            for (int bz = z - PLANT_BOX; bz <= z + PLANT_BOX; bz++) {
                for (int by = top - PLANT_GROUND_DEPTH; by < top; by++) {
                    setBlock.invoke(world, bx, by, bz, sandy ? 12 : 3);
                }
                setBlock.invoke(world, bx, top, bz, sandy ? 12 : 2);
                for (int by = top + 1; by <= top + PLANT_HEADROOM; by++) {
                    setBlock.invoke(world, bx, by, bz, 0);
                }
                if (scene == SCENE_SAND_SCATTER && ((bx + bz) % 3 == 0)) {
                    setBlock.invoke(world, bx, top + 1, bz, 1);   // stone
                }
            }
        }

        if (scene == SCENE_GRASS_WATER) {
            for (int bx = x + WATER_X0; bx <= x + WATER_X1; bx++) {
                for (int bz = z + WATER_Z0; bz <= z + WATER_Z1; bz++) {
                    setBlock.invoke(world, bx, top, bz, 9);   // still water
                }
            }
        }
    }

    private static void emitPlant(String jarPath, String scratchDir) {
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> worldClass = loader.loadClass(WORLD);
            Class<?> reedClass = loader.loadClass("es");
            Class<?> cactusClass = loader.loadClass("da");

            java.lang.reflect.Constructor<?> worldCtor =
                worldClass.getConstructor(java.io.File.class, String.class, long.class);
            java.lang.reflect.Field snowField = worldClass.getField(SNOW_FIELD);
            java.lang.reflect.Method getChunk = worldClass.getMethod("b", int.class, int.class);
            java.lang.reflect.Method getBlock =
                worldClass.getMethod("a", int.class, int.class, int.class);
            java.lang.reflect.Method setBlock =
                worldClass.getMethod("a", int.class, int.class, int.class, int.class);

            java.lang.reflect.Method reedRun = reedClass.getMethod(
                "a", worldClass, Random.class, int.class, int.class, int.class);
            java.lang.reflect.Method cactusRun = cactusClass.getMethod(
                "a", worldClass, Random.class, int.class, int.class, int.class);

            // {kind, scene, rngSeed}   kind: 0 = reed (83), 1 = cactus (81)
            long[][] cases = {
                {0, SCENE_GRASS_WATER, 101},   // reeds on a shoreline: the working case
                {0, SCENE_GRASS_WATER, 102},
                {0, SCENE_GRASS_WATER, 103},
                {0, SCENE_GRASS, 104},         // no water anywhere: every try refuses
                {0, SCENE_SAND, 105},          // wrong block below, water or not
                {0, SCENE_SAND_SCATTER, 106},
                {1, SCENE_SAND, 201},          // cactus on flat sand: the working case
                {1, SCENE_SAND, 202},
                {1, SCENE_SAND, 203},
                {1, SCENE_SAND_SCATTER, 204},  // solid neighbours block most of them
                {1, SCENE_GRASS, 205},         // wrong block below
                {1, SCENE_GRASS_WATER, 206},
            };

            java.io.File dir = new java.io.File(scratchDir, "plant");
            deleteTree(dir);
            dir.mkdirs();
            Object world = worldCtor.newInstance(dir, "genref", 246813579L);
            snowField.setBoolean(world, false);

            StringBuilder table = new StringBuilder();
            StringBuilder blocks = new StringBuilder();
            int totalPlacements = 0;

            for (int c = 0; c < cases.length; c++) {
                final int kind = (int) cases[c][0];
                final int scene = (int) cases[c][1];
                final long rngSeed = cases[c][2];

                // Walk outward until a site passes the probe. Sites are spaced
                // far enough apart that no two cases can reach each other.
                int x = 0;
                int z = 800 + c * 64;
                boolean found = false;
                for (int attempt = 0; attempt < 400 && !found; attempt++) {
                    x = 800 + attempt * 48;
                    for (int cx = (x - PLANT_BOX - 2) >> 4; cx <= (x + PLANT_BOX + 2) >> 4; cx++) {
                        for (int cz = (z - PLANT_BOX - 2) >> 4;
                             cz <= (z + PLANT_BOX + 2) >> 4; cz++) {
                            getChunk.invoke(world, cx, cz);
                        }
                    }
                    found = siteIsSolidAndDry(getBlock, world, x, z);
                }
                if (!found) {
                    throw new IllegalStateException("no solid dry site for plant case " + c);
                }

                final int y = PLANT_GROUND_TOP + 1;
                buildPlantScene(setBlock, world, scene, x, z);

                Random random = new Random(rngSeed);
                Object gen = (kind == 0) ? reedClass.getConstructor().newInstance()
                                         : cactusClass.getConstructor().newInstance();
                java.lang.reflect.Method run = (kind == 0) ? reedRun : cactusRun;
                run.invoke(gen, world, random, x, y, z);

                // The stream fingerprint: one more draw, after the generator is
                // done. Equal only if the same number of draws happened.
                final long afterDraw = random.nextLong();

                final int plantId = (kind == 0) ? 83 : 81;
                StringBuilder placements = new StringBuilder();
                int count = 0;
                for (int bx = x - PLANT_BOX; bx <= x + PLANT_BOX; bx++) {
                    for (int by = PLANT_GROUND_TOP;
                         by <= PLANT_GROUND_TOP + PLANT_HEADROOM; by++) {
                        for (int bz = z - PLANT_BOX; bz <= z + PLANT_BOX; bz++) {
                            final int id =
                                ((Integer) getBlock.invoke(world, bx, by, bz)).intValue();
                            if (id == plantId) {
                                placements.append("    {").append(bx - x).append(", ")
                                          .append(by).append(", ").append(bz - z).append("},\n");
                                count++;
                            }
                        }
                    }
                }
                totalPlacements += count;

                blocks.append("inline constexpr PlantPlacement kPlant").append(c)
                      .append("[").append(Math.max(count, 1)).append("] = {\n")
                      .append(count == 0 ? "    {0, 0, 0},\n" : placements.toString())
                      .append("};\n");

                table.append("    {").append(kind).append(", ").append(scene).append(", ")
                     .append(lit(rngSeed)).append(", ").append(x).append(", ").append(y)
                     .append(", ").append(z).append(", ").append(lit(afterDraw)).append(", ")
                     .append(count).append(", kPlant").append(c).append("},\n");
            }

            p("// Generated by tools/genref.java --jar <client.jar> --plant <scratch-dir>.");
            p("// Do not edit by hand.");
            p("//");
            p("// WorldGenReed (`es`) and WorldGenCactus (`da`) against a **real a1.1.2 World**");
            p("// on a procedurally built scene -- see tools/genref.java for the four scenes,");
            p("// which tests/plant_test.cpp has to build identically.");
            p("//");
            p("// `afterDraw` is a stream fingerprint: the next long drawn once the generator");
            p("// has finished. It matches only if the same number of random numbers were");
            p("// consumed, which is the failure this pair is most likely to have.");
            p("//");
            p("// Regenerate with:");
            p("//   java tools/genref.java --jar <client.jar> --plant /tmp/genref-scratch");
            p("//     redirected to tests/plant_vectors.hpp");
            p("");
            p("#pragma once");
            p("");
            p("#include \"core/util/types.hpp\"");
            p("");
            p("namespace mc::test {");
            p("");
            p("inline constexpr int kPlantBox = " + PLANT_BOX + ";");
            p("inline constexpr int kPlantGroundTop = " + PLANT_GROUND_TOP + ";");
            p("inline constexpr int kPlantGroundDepth = " + PLANT_GROUND_DEPTH + ";");
            p("inline constexpr int kPlantHeadroom = " + PLANT_HEADROOM + ";");
            p("");
            p("// The still-water pool in kSceneGrassWater, as offsets from the case origin.");
            p("inline constexpr int kWaterX0 = " + WATER_X0 + ";");
            p("inline constexpr int kWaterX1 = " + WATER_X1 + ";");
            p("inline constexpr int kWaterZ0 = " + WATER_Z0 + ";");
            p("inline constexpr int kWaterZ1 = " + WATER_Z1 + ";");
            p("");
            p("enum PlantScene {");
            p("    kSceneSand = " + SCENE_SAND + ",");
            p("    kSceneGrass = " + SCENE_GRASS + ",");
            p("    kSceneGrassWater = " + SCENE_GRASS_WATER + ",");
            p("    kSceneSandScatter = " + SCENE_SAND_SCATTER + ",");
            p("};");
            p("");
            p("// Offsets from the case's origin in x and z; y is absolute.");
            p("struct PlantPlacement {");
            p("    i32 dx;");
            p("    i32 y;");
            p("    i32 dz;");
            p("};");
            p("");
            p("struct PlantCase {");
            p("    int kind;         // 0 = reed, 1 = cactus");
            p("    int scene;");
            p("    i64 rngSeed;");
            p("    i32 x;");
            p("    i32 y;");
            p("    i32 z;");
            p("    i64 afterDraw;    // stream fingerprint");
            p("    int placementCount;");
            p("    const PlantPlacement* placements;");
            p("};");
            p("");
            System.out.print(blocks);
            p("");
            p("inline constexpr int kPlantCaseCount = " + cases.length + ";");
            p("");
            p("// Total blocks placed across every case. A fixture that placed nothing would");
            p("// be satisfied by a generator that refuses everything.");
            p("inline constexpr int kPlantTotalPlacements = " + totalPlacements + ";");
            p("");
            p("inline constexpr PlantCase kPlantCases[kPlantCaseCount] = {");
            System.out.print(table);
            p("};");
            p("");
            p("}  // namespace mc::test");
        } catch (Exception e) {
            System.err.println("genref --plant failed: " + e);
            e.printStackTrace();
            System.exit(1);
        }
    }

    // ---------------------------------------------------------------------
    // The light engine
    // ---------------------------------------------------------------------
    //
    // a1.1.2 does not light a chunk in one pass. `Chunk.generateSkylightMap`
    // fills each column downward from its own height map, and everything else
    // -- light crossing chunk boundaries, block light, the corrections needed
    // once a neighbour finally loads -- happens through a **queue of bounding
    // boxes** (`kn`) that `World.updateLights` drains a thousand at a time,
    // from the end, re-scanning each box and rescheduling its neighbours until
    // nothing changes.
    //
    // That is a fixed-point iteration, and this mode's whole job is to run it
    // **to completion** so the fixed point can be compared against. `cn.e()`
    // returns true when it stopped at its own 1000-entry budget with work
    // left, so draining is a loop on that.
    //
    // The fixture carries the **3x3 of block columns** around the target and
    // the **centre's** converged skylight, blocklight and height map. 3x3 is
    // sufficient and not a guess: light travels at most 15 blocks, the ring
    // extends 16, and a path that leaves the ring and comes back cannot be
    // shorter than the direct one, so no source or route outside it can change
    // a single value inside the centre.

    // ---------------------------------------------------------------------
    // The sky: its three colours over a day, and the star field's seed stream
    // ---------------------------------------------------------------------
    //
    // The colours come off a **real World**, the same way the light vectors do:
    // `cn.b(F)` getSkyColor, `cn.e(F)` getFogColor and `cn.f(F)` getStarBrightness
    // are instance methods that read the world's own clock, so the clock (public
    // field `cn.c`) is set and the three are asked. Nothing here needs a GL
    // context; RenderGlobal is not touched.
    //
    // The stars are the one part that cannot be called: `e.f()` builds a display
    // list, so the loop below is a transcription of its bytecode rather than a
    // call into it. What it emits is the part a transcription gets wrong -- how
    // many of the 1500 candidates survive the rejection test, and in what order
    // the random stream is drawn -- and not the quad corners, which
    // tests/sky_test.cpp checks as geometry instead.
    private static void emitSky(String jarPath, String scratchDir) {
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> worldClass = loader.loadClass(WORLD);
            java.lang.reflect.Constructor<?> worldCtor =
                worldClass.getConstructor(java.io.File.class, String.class, long.class);
            // `cn.c` is the world clock and it is public: setting it is what
            // getCelestialAngle reads a line later.
            java.lang.reflect.Field timeField = worldClass.getField("c");
            java.lang.reflect.Method skyColor = worldClass.getMethod("b", float.class);
            java.lang.reflect.Method fogColor = worldClass.getMethod("e", float.class);
            java.lang.reflect.Method starBrightness = worldClass.getMethod("f", float.class);
            Class<?> vecClass = loader.loadClass("aj");
            java.lang.reflect.Field vx = vecClass.getField("a");
            java.lang.reflect.Field vy = vecClass.getField("b");
            java.lang.reflect.Field vz = vecClass.getField("c");

            // Dawn, the test world's own Time=412, noon, the whole of dusk one
            // tick either side of where the steps land, midnight, and dawn
            // again. Partial ticks on two of them, because the renderer always
            // asks with one and a transcription that dropped it would still
            // pass on whole ticks.
            long[] times = {0L, 412L, 6000L, 11000L, 12000L, 12500L, 12800L, 13000L,
                            13500L, 15000L, 18000L, 22000L, 23000L, 23500L};
            float[] partials = {0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.25f,
                                0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

            java.io.File dir = new java.io.File(scratchDir, "sky");
            deleteTree(dir);
            dir.mkdirs();
            Object world = worldCtor.newInstance(dir, "genref", 1234567890L);

            p("// Generated by tools/genref.java --jar <client.jar> --sky <scratch-dir>.");
            p("// Do not edit by hand.");
            p("//");
            p("// a1.1.2's sky, from a **real World**: getSkyColor, getFogColor and");
            p("// getStarBrightness asked of the original at fourteen times of day, plus the");
            p("// star field's rejection stream from a transcription of RenderGlobal's own");
            p("// loop. See tools/genref.java --sky.");
            p("//");
            p("// Regenerate with:");
            p("//   java tools/genref.java --jar <client.jar> --sky /tmp/genref-scratch");
            p("//     redirected to tests/sky_vectors.hpp");
            p("");
            p("#pragma once");
            p("");
            p("#include \"core/util/types.hpp\"");
            p("");
            p("namespace mc::test {");
            p("");
            p("struct SkyCase {");
            p("    i64 time;");
            p("    float partial;");
            p("    double skyR, skyG, skyB;");
            p("    double fogR, fogG, fogB;");
            p("    double stars;");
            p("};");
            p("");
            p("inline constexpr int kSkyCaseCount = " + times.length + ";");
            p("");
            p("inline constexpr SkyCase kSkyCases[kSkyCaseCount] = {");
            for (int i = 0; i < times.length; i++) {
                timeField.setLong(world, times[i]);
                Object sky = skyColor.invoke(world, partials[i]);
                Object fog = fogColor.invoke(world, partials[i]);
                float stars = (Float) starBrightness.invoke(world, partials[i]);
                p("    {" + times[i] + ", " + flt(partials[i]) + ", "
                  + dbl(vx.getDouble(sky)) + ", " + dbl(vy.getDouble(sky)) + ", "
                  + dbl(vz.getDouble(sky)) + ", "
                  + dbl(vx.getDouble(fog)) + ", " + dbl(vy.getDouble(fog)) + ", "
                  + dbl(vz.getDouble(fog)) + ", " + dbl(stars) + "},");
            }
            p("};");
            p("");

            // e.f(), the star list, for its random stream only.
            java.util.Random random = new java.util.Random(10842L);
            java.util.ArrayList<double[]> stars = new java.util.ArrayList<>();
            for (int i = 0; i < 1500; i++) {
                double d = random.nextFloat() * 2.0F - 1.0F;
                double d1 = random.nextFloat() * 2.0F - 1.0F;
                double d2 = random.nextFloat() * 2.0F - 1.0F;
                double d3 = 0.25F + random.nextFloat() * 0.25F;
                double d4 = d * d + d1 * d1 + d2 * d2;
                if (d4 < 1.0D && d4 > 0.01D) {
                    d4 = 1.0D / Math.sqrt(d4);
                    d *= d4;
                    d1 *= d4;
                    d2 *= d4;
                    stars.add(new double[]{i, d * 100.0D, d1 * 100.0D, d2 * 100.0D, d3,
                                           random.nextDouble() * Math.PI * 2.0D});
                }
            }

            p("// How many of the 1500 candidates land inside the unit sphere and outside its");
            p("// 0.01 core. The rejected ones still draw four floats each, so a transcription");
            p("// that drew them in the wrong order would put every star somewhere else.");
            p("inline constexpr int kStarCount = " + stars.size() + ";");
            p("");
            p("struct StarSample {");
            p("    int index;       // which surviving star this is, counted in order");
            p("    int candidate;   // which of the 1500 turns of the loop this was");
            p("    double x, y, z;  // centre, on the sphere of radius 100");
            p("    double size;     // half-width of the quad, 0.25 to 0.5");
            p("    double roll;     // the billboard's own rotation, 0 to 2pi");
            p("};");
            p("");
            // The first few, the last, and a spread through the middle: enough
            // that a stream drawn one number out of step cannot match.
            java.util.ArrayList<Integer> pick = new java.util.ArrayList<>();
            for (int i = 0; i < 4; i++) {
                pick.add(i);
            }
            for (int i = 1; i <= 6; i++) {
                pick.add(i * stars.size() / 7);
            }
            pick.add(stars.size() - 1);
            p("inline constexpr int kStarSampleCount = " + pick.size() + ";");
            p("");
            p("inline constexpr StarSample kStarSamples[kStarSampleCount] = {");
            for (int index : pick) {
                double[] s = stars.get(index);
                p("    {" + index + ", " + (int) s[0] + ", " + dbl(s[1]) + ", " + dbl(s[2])
                  + ", " + dbl(s[3]) + ", " + dbl(s[4]) + ", " + dbl(s[5]) + "},");
            }
            p("};");
            p("");
            p("}  // namespace mc::test");
        } catch (Exception e) {
            throw new RuntimeException(e);
        }
    }

    // The partial-tick column, which is a float in the original's signature.
    // Doubles go through the exact round-tripping `dbl` further down the file.
    private static String flt(float value) {
        return String.format("%.9gf", value);
    }

    private static void emitLight(String jarPath, String scratchDir) {
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> worldClass = loader.loadClass(WORLD);
            Class<?> chunkClass = loader.loadClass("ga");
            Class<?> nibbleClass = loader.loadClass("mu");
            Class<?> skyBlockClass = loader.loadClass("by");

            java.lang.reflect.Constructor<?> worldCtor =
                worldClass.getConstructor(java.io.File.class, String.class, long.class);
            java.lang.reflect.Field snowField = worldClass.getField(SNOW_FIELD);
            java.lang.reflect.Method getChunk = worldClass.getMethod("b", int.class, int.class);
            // `cn.e()` -- updateLights. True means it hit its budget with more
            // to do, so looping on it is what runs the queue dry.
            java.lang.reflect.Method updateLights = worldClass.getMethod("e");
            java.lang.reflect.Method heightAt = worldClass.getMethod("c", int.class, int.class);
            java.lang.reflect.Method setBlockAt =
                worldClass.getMethod("a", int.class, int.class, int.class, int.class);

            java.lang.reflect.Field chunkBlocks = chunkClass.getField("b");       // byte[]
            java.lang.reflect.Field chunkHeight = chunkClass.getField("h");       // byte[]
            java.lang.reflect.Field chunkSky = chunkClass.getField("f");          // mu
            java.lang.reflect.Field chunkBlockLight = chunkClass.getField("g");   // mu
            java.lang.reflect.Field nibbleData = nibbleClass.getDeclaredField("a");
            nibbleData.setAccessible(true);
            java.lang.reflect.Field skyEnum = skyBlockClass.getField("a");
            java.lang.reflect.Field blockEnum = skyBlockClass.getField("b");
            // Touch them so a rename shows up here rather than as wrong numbers.
            if (skyEnum.get(null) == null || blockEnum.get(null) == null) {
                throw new IllegalStateException("EnumSkyBlock constants missing");
            }

            // {seed, chunkX, chunkZ, snowCovered, edgeEdits}
            //
            // The third case exists because **generated terrain never reaches
            // either end of the world**, so the first two leave the y = 0 and
            // y = 127 boundaries of the sky rule completely untested -- both
            // were confirmed as surviving mutants before this case was added.
            // It carves the bedrock out of one column and caps another with
            // leaves, then lets the original's own light queue work out what
            // that means, rather than asserting what the bytecode looked like
            // it should mean.
            long[][] cases = {
                {1234567890L, 37, -14, 0, 0},
                {-8675309L, 1875, -2048, 0, 0},
                {1234567890L, 37, -14, 0, 1},
            };

            p("// Generated by tools/genref.java --jar <client.jar> --light <scratch-dir>.");
            p("// Do not edit by hand.");
            p("//");
            p("// Converged lighting from a **real a1.1.2 World**: chunks generated and");
            p("// populated, then World.updateLights drained until its queue is empty, so this");
            p("// is the fixed point the original settles on rather than a snapshot part way");
            p("// through. Each case carries the 3x3 of block columns around the target and the");
            p("// centre chunk's skylight, blocklight and height map.");
            p("//");
            p("// Columns are run-length encoded as (count << 8) | value; light nibbles the");
            p("// same way, indexed x << 11 | z << 7 | y like everything else in this format.");
            p("//");
            p("// Regenerate with:");
            p("//   java tools/genref.java --jar <client.jar> --light /tmp/genref-scratch");
            p("//     redirected to tests/light_vectors.hpp");
            p("");
            p("#pragma once");
            p("");
            p("#include \"core/util/types.hpp\"");
            p("");
            p("namespace mc::test {");
            p("");
            p("struct LightCase {");
            p("    i64 seed;");
            p("    i32 chunkX;");
            p("    i32 chunkZ;");
            p("    bool snowCovered;");
            p("    // Nine columns, x slowest: column (ix, iz) covers chunk");
            p("    // (chunkX - 1 + ix, chunkZ - 1 + iz).");
            p("    int blockRuns[9];");
            p("    const u32* blocks[9];");
            p("    int skyRuns;");
            p("    const u32* sky;");
            p("    int blockLightRuns;");
            p("    const u32* blockLight;");
            p("    const u8* heightMap;   // 256 entries, z << 4 | x");
            p("};");
            p("");

            java.io.File dir = new java.io.File(scratchDir, "light");
            deleteTree(dir);
            dir.mkdirs();

            StringBuilder table = new StringBuilder();
            for (int c = 0; c < cases.length; c++) {
                final long seed = cases[c][0];
                final int cx = (int) cases[c][1];
                final int cz = (int) cases[c][2];
                final boolean snowy = cases[c][3] != 0;
                @SuppressWarnings("unused") final boolean edited = cases[c][4] != 0;

                java.io.File caseDir = new java.io.File(dir, "case" + c);
                caseDir.mkdirs();
                Object world = worldCtor.newInstance(caseDir, "genref", seed);
                snowField.setBoolean(world, snowy);

                // 5x5, so every column of the 3x3 the fixture carries has its
                // own neighbours present and is itself fully populated -- a
                // chunk is only populated once its neighbours exist.
                for (int x = cx - 2; x <= cx + 2; x++) {
                    for (int z = cz - 2; z <= cz + 2; z++) {
                        getChunk.invoke(world, x, z);
                    }
                }

                if (cases[c][4] != 0) {
                    final int baseX = cx * 16 + 4;
                    final int baseZ = cz * 16 + 4;
                    // A shaft down to and through the world floor. The cell at
                    // y = 0 reads its own floor -- outside the world, where
                    // getSavedLightValue answers with Sky's default of 15 --
                    // so it must come out at 14 rather than 0.
                    for (int y = 0; y <= 6; y++) {
                        setBlockAt.invoke(world, baseX, y, baseZ, 0);
                    }
                    // And a patch capped at the very top by something that
                    // dims rather than blocks. Those columns have a height map
                    // of 128, so they "cannot see the sky", yet the ceiling
                    // above them is a source of 15 and leaves cost 1.
                    //
                    // **It has to be 3x3 rather than a single block**, and
                    // that was found by mutation rather than by design: one
                    // leaf is surrounded by open sky at its own height, so
                    // 15 from above and 15 - 1 from the side give the same 14
                    // and the case proves nothing. The middle of a patch has
                    // no lit neighbour at its own level, so 14 from above and
                    // 13 from the side are finally distinguishable.
                    for (int dx = -1; dx <= 1; dx++) {
                        for (int dz = -1; dz <= 1; dz++) {
                            setBlockAt.invoke(world, baseX + 6 + dx, 127, baseZ + 6 + dz, 18);
                            for (int y = 120; y < 127; y++) {
                                setBlockAt.invoke(world, baseX + 6 + dx, y, baseZ + 6 + dz, 0);
                            }
                        }
                    }
                }

                // Run the queue dry. The bound is a safety net, not a budget:
                // if it is ever reached the fixture would be a snapshot rather
                // than a fixed point, which is exactly the thing that must not
                // be published quietly.
                int drains = 0;
                while (((Boolean) updateLights.invoke(world)).booleanValue()) {
                    if (++drains > 100000) {
                        throw new IllegalStateException("light queue did not converge");
                    }
                }

                StringBuilder columns = new StringBuilder();
                for (int ix = 0; ix < 3; ix++) {
                    for (int iz = 0; iz < 3; iz++) {
                        Object nb = getChunk.invoke(world, cx - 1 + ix, cz - 1 + iz);
                        byte[] blocks = (byte[]) chunkBlocks.get(nb);
                        int runs = emitRuns("kLightBlocks" + c + "_" + (ix * 3 + iz), blocks);
                        columns.append(runs).append(", ");
                    }
                }

                Object chunk = getChunk.invoke(world, cx, cz);
                byte[] skyPacked = (byte[]) nibbleData.get(chunkSky.get(chunk));
                byte[] blockPacked = (byte[]) nibbleData.get(chunkBlockLight.get(chunk));
                int skyRuns = emitNibbleRuns("kLightSky" + c, skyPacked);
                int blockRuns = emitNibbleRuns("kLightBlock" + c, blockPacked);

                byte[] heights = (byte[]) chunkHeight.get(chunk);
                p("inline constexpr u8 kLightHeight" + c + "[256] = {");
                StringBuilder hs = new StringBuilder("   ");
                for (int i = 0; i < 256; i++) {
                    hs.append(' ').append(heights[i] & 255).append(',');
                    if (i % 16 == 15) { p(hs.toString()); hs = new StringBuilder("   "); }
                }
                p("};");
                p("");

                table.append("    {").append(lit(seed)).append(", ").append(cx).append(", ")
                     .append(cz).append(", ").append(snowy).append(",\n     {");
                table.append(columns.substring(0, columns.length() - 2)).append("},\n     {");
                for (int i = 0; i < 9; i++) {
                    table.append("kLightBlocks").append(c).append("_").append(i)
                         .append(i == 8 ? "" : ", ");
                }
                table.append("},\n     ").append(skyRuns).append(", kLightSky").append(c)
                     .append(", ").append(blockRuns).append(", kLightBlock").append(c)
                     .append(", kLightHeight").append(c).append("},\n");
            }

            p("inline constexpr int kLightCaseCount = " + cases.length + ";");
            p("");
            p("inline constexpr LightCase kLightCases[kLightCaseCount] = {");
            System.out.print(table);
            p("};");
            p("");
            p("}  // namespace mc::test");
        } catch (Exception e) {
            System.err.println("genref --light failed: " + e);
            e.printStackTrace();
            System.exit(1);
        }
    }

    // Run-length encodes an already-packed nibble array by unpacking it first,
    // so the fixture is one entry per block rather than per byte pair and the
    // C++ side never has to know the packing.
    private static int emitNibbleRuns(String name, byte[] packed) {
        int[] values = new int[packed.length * 2];
        for (int i = 0; i < values.length; i++) {
            final int b = packed[i >> 1] & 255;
            values[i] = (i & 1) == 0 ? (b & 15) : (b >> 4);
        }
        return emitIntRuns(name, values);
    }

    // ---------------------------------------------------------------------
    // WorldGenFlowers (`ae`) -- dandelions, roses and both mushrooms
    // ---------------------------------------------------------------------
    //
    // One generator class handed four different block ids, and the id decides
    // which `canBlockStay` answers: `mq` for the flowers, which wants light of
    // at least 8 or a view of the sky, and `ky` for the mushrooms, which wants
    // 13 or less. So a fixture that only ever plants in the open would test
    // half of it and would pass with the two predicates swapped.
    //
    // The scenes therefore include a **canopy**: a slab of leaves floating a
    // few blocks above the ground, which drops the height map and leaves a
    // band of partial light beneath it. That is the only place a flower and a
    // mushroom can both legally stand, and the only place the difference
    // between "light >= 8" and "can see the sky" is visible at all.
    //
    // Like the plants oracle, each case records a stream fingerprint -- though
    // this generator draws a fixed 192 numbers whatever it finds, so the
    // fingerprint is checking that claim rather than guarding a branch.

    private static final int FLOWER_BOX = 9;

    // Half-width of the block patch the fixture carries. **All ten cases share
    // one site** -- same seed, same deterministic probe -- and differ only in
    // which scene is laid over it, so the terrain is emitted once and the C++
    // side applies the scene rules itself. Emitting it per case would have
    // meant ten copies of the same half-megabyte.
    //
    // Ten is comfortably past the generator's reach: it perturbs x and z by
    // `nextInt(8) - nextInt(8)`, so seven blocks, and reads one below that.
    private static final int FLOWER_PATCH = 11;

    // How far above the natural surface the two covered scenes put their roof.
    private static final int FLOWER_ROOF = 5;

    private static final int FSCENE_OPEN = 0;      // the terrain as generated
    private static final int FSCENE_CANOPY = 1;    // a leaf roof five blocks up
    private static final int FSCENE_STONE = 2;     // the surface replaced with stone
    private static final int FSCENE_ROOFED = 3;    // a stone roof instead of leaves
    // Dark *and* standing on stone. Added because "mushrooms grow on any
    // opaque cube" and "mushrooms grow on grass or dirt" were indistinguishable
    // without it -- every other shaded scene has grass underfoot.
    private static final int FSCENE_STONE_ROOFED = 4;
    // Leaves one block above the ground, leaving a single air gap. That gap is
    // the **only** place in a1.1.2 where a plantable cell reads exactly 13:
    // the height map stops at the leaves, the leaves themselves cost 1 leaving
    // 14, and the air below them costs another 1. Without it, "light <= 13" and
    // "light <= 12" are indistinguishable.
    private static final int FSCENE_LOW_CANOPY = 5;
    // Leaves as the *floor*, under a stone roof. The only scene in which the
    // two opaque-cube columns can be told apart: `Block.opaqueCubeLookup` says
    // leaves are solid and a live `isOpaqueCube()` under fancy graphics says
    // they are not, and a mushroom's ground test reads the former.
    private static final int FSCENE_LEAF_FLOOR = 6;

    // **These scenes edit the world as little as possible, and that is the
    // point.** The first attempt carved a room out of solid rock underground,
    // which produced ten cases in which no flower could possibly be planted --
    // `mq.canBlockStay` wants light of at least 8 or a view of the sky, and a
    // buried room has neither. Every flower case came back empty and every
    // mushroom case came back full, which is correct behaviour and a useless
    // fixture.
    //
    // So the scenes sit on the terrain's own grass, in the open, and add at
    // most one layer of blocks. That also keeps the light cascade small: each
    // roof block is placed above its column's height map, so it costs one
    // relight of that column and nothing more.
    private static void buildFlowerScene(java.lang.reflect.Method setBlock,
                                         java.lang.reflect.Method height, Object world,
                                         int scene, int x, int z) throws Exception {
        if (scene == FSCENE_OPEN) {
            return;
        }
        for (int bx = x - FLOWER_BOX; bx <= x + FLOWER_BOX; bx++) {
            for (int bz = z - FLOWER_BOX; bz <= z + FLOWER_BOX; bz++) {
                final int top = ((Integer) height.invoke(world, bx, bz)).intValue();
                if (scene == FSCENE_STONE || scene == FSCENE_STONE_ROOFED) {
                    setBlock.invoke(world, bx, top - 1, bz, 1);
                } else if (scene == FSCENE_LEAF_FLOOR) {
                    setBlock.invoke(world, bx, top - 1, bz, 18);
                }
                if (scene == FSCENE_LOW_CANOPY) {
                    setBlock.invoke(world, bx, top + 1, bz, 18);
                } else if (scene != FSCENE_STONE) {   // includes FSCENE_LEAF_FLOOR
                    setBlock.invoke(world, bx, top + FLOWER_ROOF, bz,
                                    scene == FSCENE_CANOPY ? 18 : 1);
                }
            }
        }
    }

    // A patch of open, flat-ish grass with nothing wet in it. Flat matters
    // because the generator perturbs y by only +-3: on a cliff most of its
    // tries would land inside the hillside and the case would say little.
    private static boolean flowerSiteIsOpenGrass(java.lang.reflect.Method getBlock,
                                                 java.lang.reflect.Method height, Object world,
                                                 int x, int z) throws Exception {
        final int centre = ((Integer) height.invoke(world, x, z)).intValue();
        if (centre < 60 || centre > 100) {
            return false;
        }
        for (int bx = x - FLOWER_BOX - 1; bx <= x + FLOWER_BOX + 1; bx++) {
            for (int bz = z - FLOWER_BOX - 1; bz <= z + FLOWER_BOX + 1; bz++) {
                final int top = ((Integer) height.invoke(world, bx, bz)).intValue();
                if (Math.abs(top - centre) > 2) {
                    return false;
                }
                final int surface = ((Integer) getBlock.invoke(world, bx, top - 1, bz)).intValue();
                if (surface != 2) {   // grass, so flowers have somewhere legal to go
                    return false;
                }
                // Nothing that flows, and nothing already standing here.
                for (int by = top; by <= top + FLOWER_ROOF + 2; by++) {
                    final int id = ((Integer) getBlock.invoke(world, bx, by, bz)).intValue();
                    if (id != 0) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    // Runs World.updateLights until its queue is empty. It returns true when it
    // stopped at its own 1000-entry budget with work left, so looping on it is
    // what runs the queue dry.
    private static void drainLights(java.lang.reflect.Method updateLights, Object world)
            throws Exception {
        int guard = 0;
        while (((Boolean) updateLights.invoke(world)).booleanValue()) {
            if (++guard > 100000) {
                throw new IllegalStateException("light queue did not converge");
            }
        }
    }

    private static void emitFlower(String jarPath, String scratchDir) {
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> worldClass = loader.loadClass(WORLD);
            Class<?> flowerClass = loader.loadClass("ae");

            java.lang.reflect.Constructor<?> worldCtor =
                worldClass.getConstructor(java.io.File.class, String.class, long.class);
            java.lang.reflect.Field snowField = worldClass.getField(SNOW_FIELD);
            java.lang.reflect.Method getChunk = worldClass.getMethod("b", int.class, int.class);
            java.lang.reflect.Method getBlock =
                worldClass.getMethod("a", int.class, int.class, int.class);
            java.lang.reflect.Method setBlock =
                worldClass.getMethod("a", int.class, int.class, int.class, int.class);
            java.lang.reflect.Method run = flowerClass.getMethod(
                "a", worldClass, Random.class, int.class, int.class, int.class);
            java.lang.reflect.Constructor<?> flowerCtor = flowerClass.getConstructor(int.class);

            // {plantId, scene, rngSeed}
            long[][] cases = {
                {37, FSCENE_OPEN, 301},        // dandelions in the open
                {37, FSCENE_CANOPY, 302},      // and in the shade under leaves
                {38, FSCENE_OPEN, 303},        // roses
                {38, FSCENE_STONE, 304},       // wrong ground: nothing at all
                {38, FSCENE_ROOFED, 305},      // dark: only the sky clause could save it
                {39, FSCENE_OPEN, 306},        // brown mushroom, too bright
                {39, FSCENE_CANOPY, 307},      // brown mushroom in the shade
                {39, FSCENE_ROOFED, 308},      // and in the dark on stone
                {40, FSCENE_CANOPY, 309},      // red mushroom
                {40, FSCENE_STONE, 310},       // opaque ground, but open sky
                {40, FSCENE_STONE_ROOFED, 311},// dark *and* stone: mushrooms only
                {38, FSCENE_STONE_ROOFED, 312},// and a flower refuses it outright
                {39, FSCENE_LOW_CANOPY, 313},  // the one place light reads exactly 13
                {37, FSCENE_LOW_CANOPY, 314},  // where a flower is still happy at 13
                {39, FSCENE_LEAF_FLOOR, 315},  // a mushroom standing on leaves
                {38, FSCENE_LEAF_FLOOR, 316},  // where a flower will not
            };

            java.lang.reflect.Method updateLights = worldClass.getMethod("e");
            java.lang.reflect.Method heightAt = worldClass.getMethod("c", int.class, int.class);

            java.io.File dir = new java.io.File(scratchDir, "flower");
            deleteTree(dir);
            dir.mkdirs();

            StringBuilder table = new StringBuilder();
            StringBuilder blocks = new StringBuilder();
            int totalPlacements = 0;
            int patchRuns = 0;

            p("// Generated by tools/genref.java --jar <client.jar> --flower <scratch-dir>.");
            p("// Do not edit by hand.");
            p("//");
            p("// WorldGenFlowers (`ae`) against a **real a1.1.2 World**, for all four ids it");
            p("// plants -- dandelion, rose, and both mushrooms -- over four scenes. The");
            p("// flowers want light of at least 8 or a view of the sky; the mushrooms want 13");
            p("// or less. So the fixture needs open ground *and* shade, or it would pass with");
            p("// the two predicates swapped.");
            p("//");
            p("// All ten cases share one site and one seed, so the terrain is carried once and");
            p("// the scene -- a leaf roof, a stone roof, or a stone surface -- is applied by");
            p("// tests/flower_test.cpp using the same rules as buildFlowerScene here.");
            p("//");
            p("// Regenerate with:");
            p("//   java tools/genref.java --jar <client.jar> --flower /tmp/genref-scratch");
            p("//     redirected to tests/flower_vectors.hpp");
            p("");
            p("#pragma once");
            p("");
            p("#include \"core/util/types.hpp\"");
            p("");
            p("namespace mc::test {");
            p("");
            p("inline constexpr int kFlowerBox = " + FLOWER_BOX + ";");
            p("inline constexpr int kFlowerPatch = " + FLOWER_PATCH + ";");
            p("inline constexpr int kFlowerRoof = " + FLOWER_ROOF + ";");
            p("");
            p("enum FlowerScene {");
            p("    kFSceneOpen = " + FSCENE_OPEN + ",");
            p("    kFSceneCanopy = " + FSCENE_CANOPY + ",");
            p("    kFSceneStone = " + FSCENE_STONE + ",");
            p("    kFSceneRoofed = " + FSCENE_ROOFED + ",");
            p("    kFSceneStoneRoofed = " + FSCENE_STONE_ROOFED + ",");
            p("    kFSceneLowCanopy = " + FSCENE_LOW_CANOPY + ",");
            p("    kFSceneLeafFloor = " + FSCENE_LEAF_FLOOR + ",");
            p("};");
            p("");
            p("// Offsets from the case origin in x and z; y is absolute.");
            p("struct FlowerPlacement {");
            p("    i32 dx;");
            p("    i32 y;");
            p("    i32 dz;");
            p("};");
            p("");
            p("struct FlowerCase {");
            p("    int plantId;");
            p("    int scene;");
            p("    i64 rngSeed;");
            p("    i32 x;");
            p("    i32 y;        // the ground height, read before any scene was built");
            p("    i32 z;");
            p("    i64 afterDraw;");
            p("    int placementCount;");
            p("    const FlowerPlacement* placements;");
            p("};");
            p("");
            p("// The shared terrain, as (count << 8) | blockId over a");
            p("// (2*kFlowerPatch+1)^2 patch of full-height columns, x slowest then z then y.");

            for (int c = 0; c < cases.length; c++) {
                final int plantId = (int) cases[c][0];
                final int scene = (int) cases[c][1];
                final long rngSeed = cases[c][2];

                // **A fresh World per case, and the light queue drained between
                // every step.** Generating a long strip of chunks in one World
                // was enough to push `scheduleLightingUpdate` past its own
                // 100,000-entry threshold, at which point it drains the queue
                // *from inside itself* -- and that re-entry, queue processing
                // scheduling more queue processing, overflowed the JVM stack
                // during plain chunk generation. Emptying the queue from the
                // top level keeps it far below the threshold, so it never
                // re-enters.
                java.io.File caseDir = new java.io.File(dir, "case" + c);
                caseDir.mkdirs();
                Object world = worldCtor.newInstance(caseDir, "genref", 13571357L);
                snowField.setBoolean(world, false);

                int x = 0;
                final int z = 2000;
                boolean found = false;
                for (int attempt = 0; attempt < 200 && !found; attempt++) {
                    x = 2000 + attempt * 40;
                    for (int cx = (x - FLOWER_BOX - 2) >> 4; cx <= (x + FLOWER_BOX + 2) >> 4; cx++) {
                        for (int cz = (z - FLOWER_BOX - 2) >> 4;
                             cz <= (z + FLOWER_BOX + 2) >> 4; cz++) {
                            getChunk.invoke(world, cx, cz);
                        }
                    }
                    drainLights(updateLights, world);
                    found = flowerSiteIsOpenGrass(getBlock, heightAt, world, x, z);
                }
                if (!found) {
                    throw new IllegalStateException("no site for flower case " + c);
                }

                // **Before the scene is built, not after.** A roof raises the
                // height map by five, so reading it afterwards would start the
                // generator up on the roof -- and since it perturbs y by only
                // +-3, it could never reach the ground it is supposed to plant
                // on. Every covered case came back empty until this moved.
                final int surface = ((Integer) heightAt.invoke(world, x, z)).intValue();

                if (c == 0) {
                    // Case 0's scene is the open one, which edits nothing, so
                    // this is the pristine terrain every case starts from.
                    StringBuilder patch = new StringBuilder();
                    int[] cells = new int[(2 * FLOWER_PATCH + 1) * (2 * FLOWER_PATCH + 1) * 128];
                    int at = 0;
                    for (int dx = -FLOWER_PATCH; dx <= FLOWER_PATCH; dx++) {
                        for (int dz = -FLOWER_PATCH; dz <= FLOWER_PATCH; dz++) {
                            for (int by = 0; by < 128; by++) {
                                cells[at++] = ((Integer) getBlock.invoke(world, x + dx, by, z + dz))
                                                  .intValue();
                            }
                        }
                    }
                    patchRuns = emitIntRuns("kFlowerTerrain", cells);
                }

                // **Deliberately not drained after this.** The drain during the
                // site probe keeps chunk generation from overflowing its own
                // queue, but running it again here would be a lie about what
                // population sees: in the real game `updateLights` only runs
                // from the game tick, and no tick happens while a chunk is
                // being generated.
                //
                // The difference is not subtle. With the queue drained, sky
                // light spreads sideways under a roof, and roses appeared nine
                // blocks in from its edge where the light had settled at
                // exactly 8. Undrained -- which is what the original actually
                // does -- that whole area reads 0 and nothing grows there.
                buildFlowerScene(setBlock, heightAt, world, scene, x, z);

                final int y = surface;
                Random random = new Random(rngSeed);
                Object gen = flowerCtor.newInstance(plantId);
                run.invoke(gen, world, random, x, y, z);
                final long afterDraw = random.nextLong();

                StringBuilder placements = new StringBuilder();
                int count = 0;
                for (int bx = x - FLOWER_BOX; bx <= x + FLOWER_BOX; bx++) {
                    for (int by = surface - 8; by <= surface + 8; by++) {
                        for (int bz = z - FLOWER_BOX; bz <= z + FLOWER_BOX; bz++) {
                            final int id =
                                ((Integer) getBlock.invoke(world, bx, by, bz)).intValue();
                            if (id == plantId) {
                                placements.append("    {").append(bx - x).append(", ")
                                          .append(by).append(", ").append(bz - z).append("},\n");
                                count++;
                            }
                        }
                    }
                }
                totalPlacements += count;

                blocks.append("inline constexpr FlowerPlacement kFlower").append(c)
                      .append("[").append(Math.max(count, 1)).append("] = {\n")
                      .append(count == 0 ? "    {0, 0, 0},\n" : placements.toString())
                      .append("};\n");

                table.append("    {").append(plantId).append(", ").append(scene).append(", ")
                     .append(lit(rngSeed)).append(", ").append(x).append(", ").append(y)
                     .append(", ").append(z).append(", ").append(lit(afterDraw)).append(", ")
                     .append(count).append(", kFlower").append(c).append("},\n");
            }

            p("");
            p("inline constexpr int kFlowerTerrainRuns = " + patchRuns + ";");
            p("");
            System.out.print(blocks);
            p("");
            p("inline constexpr int kFlowerCaseCount = " + cases.length + ";");
            p("inline constexpr int kFlowerTotalPlacements = " + totalPlacements + ";");
            p("");
            p("inline constexpr FlowerCase kFlowerCases[kFlowerCaseCount] = {");
            System.out.print(table);
            p("};");
            p("");
            p("}  // namespace mc::test");
        } catch (Exception e) {
            System.err.println("genref --flower failed: " + e);
            e.printStackTrace();
            System.exit(1);
        }
    }

    // ---------------------------------------------------------------------
    // Block.opaqueCubeLookup
    // ---------------------------------------------------------------------
    //
    // **This is not the same question as `isOpaqueCube()`, and the difference
    // is a graphics setting.** The array is filled in the Block constructor,
    // once, at class-init. `BlockLeaves.isOpaqueCube` returns `!fancyGraphics`,
    // and the client only calls `setGraphicsLevel` later, from the video
    // options -- which default to fancy (`fr.i` is initialised to true). So a
    // live call says leaves are see-through while the cached array says they
    // are solid, and both are correct answers to different questions.
    //
    // World generation reads the array: `BlockMushroom.canThisPlantGrowOn` is
    // a straight `opaqueCubeLookup[id]` lookup, so a1.1.2 will grow a mushroom
    // on a leaf block. Our block table records the live, fancy-graphics answer
    // because that is what face culling in the mesher wants, so the two need
    // separate columns and this mode is what pins the second one.
    //
    // Read by reflection rather than interpreted, because the array is a plain
    // static that the JVM has already filled in by the time it is asked for.

    private static void emitOpaqueCube(String jarPath) {
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> blockClass = loader.loadClass("ly");
            boolean[] lookup = (boolean[]) blockClass.getField("p").get(null);
            Object[] blocks = (Object[]) blockClass.getField("n").get(null);
            java.lang.reflect.Method isOpaque = blockClass.getMethod("b");

            p("// Generated by tools/genref.java --jar <client.jar> --opaquecube.");
            p("// Do not edit by hand.");
            p("//");
            p("// `Block.opaqueCubeLookup`, straight out of a loaded jar. This is the array");
            p("// world generation reads -- BlockMushroom's ground test is a lookup into it --");
            p("// and it is filled in the Block constructor, before any graphics setting is");
            p("// applied. `live` is what isOpaqueCube() answers right now, for comparison:");
            p("// they differ only for leaves, and only because the video options default to");
            p("// fancy. See docs/worldgen-a1.1.2.md.");
            p("//");
            p("// Regenerate with:");
            p("//   java tools/genref.java --jar <client.jar> --opaquecube");
            p("//     redirected to tests/opaque_cube_vectors.hpp");
            p("");
            p("#pragma once");
            p("");
            p("#include \"core/util/types.hpp\"");
            p("");
            p("namespace mc::test {");
            p("");
            p("struct OpaqueCubeEntry {");
            p("    bool cached;   // Block.opaqueCubeLookup[id]");
            p("    bool live;     // isOpaqueCube() as the class stands now");
            p("    bool defined;  // whether this version constructs the block at all");
            p("};");
            p("");
            p("inline constexpr OpaqueCubeEntry kOpaqueCube[256] = {");
            for (int id = 0; id < 256; id++) {
                final boolean defined = id < blocks.length && blocks[id] != null;
                boolean live = false;
                if (defined) {
                    live = ((Boolean) isOpaque.invoke(blocks[id])).booleanValue();
                }
                p("    {" + (id < lookup.length && lookup[id]) + ", " + live + ", " + defined
                  + "},   // " + id);
            }
            p("};");
            p("");
            p("}  // namespace mc::test");
        } catch (Exception e) {
            System.err.println("genref --opaquecube failed: " + e);
            e.printStackTrace();
            System.exit(1);
        }
    }

    // ---------------------------------------------------------------------
    // WorldGenTrees (`oa`)
    // ---------------------------------------------------------------------
    //
    // Reuses the flower oracle's site machinery -- the same open flat grass,
    // the same shared terrain patch -- because a tree needs exactly the same
    // thing: real ground with sky above it. The scenes differ, though. A tree
    // is much larger than a flower, so what is worth varying is what is *in
    // the way*: nothing, a low stone ceiling that refuses it outright, a
    // neighbouring tree's leaves that it is allowed to grow through, and a
    // hillside that clips the canopy.
    //
    // Every case records a stream fingerprint. It matters more here than
    // anywhere else so far, because the number of random draws a tree makes
    // depends on its height *and* on how many canopy corners it evaluates --
    // and a tree that fails its clearance check draws exactly one number and
    // stops.

    private static final int TREE_PATCH = 12;

    private static final int TSCENE_OPEN = 0;       // clear ground and sky
    private static final int TSCENE_CEILING = 1;    // stone four blocks up: no room
    private static final int TSCENE_LEAVES = 2;     // a slab of leaves it may grow through
    // A short stone pillar two blocks from the trunk, low down. **Chosen to sit
    // in the gap between the two radii**: the clearance check uses radius 1
    // below the top layers, so it never looks here, while the canopy's lower
    // layers use radius 2 and do. The tree therefore plants successfully and
    // comes out with a bite taken out of it -- which is what exercises the
    // opaque-cube test inside the leaf loop. A full wall, tried first, was
    // caught by the clearance check instead and simply refused.
    private static final int TSCENE_CLIP = 3;

    private static void buildTreeScene(java.lang.reflect.Method setBlock,
                                       java.lang.reflect.Method height, Object world,
                                       int scene, int x, int z) throws Exception {
        if (scene == TSCENE_OPEN) {
            return;
        }
        for (int bx = x - TREE_PATCH; bx <= x + TREE_PATCH; bx++) {
            for (int bz = z - TREE_PATCH; bz <= z + TREE_PATCH; bz++) {
                final int top = ((Integer) height.invoke(world, bx, bz)).intValue();
                if (scene == TSCENE_CEILING) {
                    setBlock.invoke(world, bx, top + 4, bz, 1);
                } else if (scene == TSCENE_LEAVES) {
                    // A layer the trunk and canopy are explicitly allowed to
                    // pass through -- the clearance check accepts leaves.
                    setBlock.invoke(world, bx, top + 5, bz, 18);
                } else if (scene == TSCENE_CLIP && bx == x + 2 && bz == z) {
                    // y + 1 .. y + 3 covers `y + height - 3` for every height
                    // the generator can roll, which is four to six.
                    for (int by = top + 1; by <= top + 3; by++) {
                        setBlock.invoke(world, bx, by, bz, 1);
                    }
                }
            }
        }
    }

    private static void emitTree(String jarPath, String scratchDir) {
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> worldClass = loader.loadClass(WORLD);
            Class<?> treeClass = loader.loadClass("oa");

            java.lang.reflect.Constructor<?> worldCtor =
                worldClass.getConstructor(java.io.File.class, String.class, long.class);
            java.lang.reflect.Field snowField = worldClass.getField(SNOW_FIELD);
            java.lang.reflect.Method getChunk = worldClass.getMethod("b", int.class, int.class);
            java.lang.reflect.Method getBlock =
                worldClass.getMethod("a", int.class, int.class, int.class);
            java.lang.reflect.Method setBlock =
                worldClass.getMethod("a", int.class, int.class, int.class, int.class);
            java.lang.reflect.Method updateLights = worldClass.getMethod("e");
            java.lang.reflect.Method heightAt = worldClass.getMethod("c", int.class, int.class);
            java.lang.reflect.Method run = treeClass.getMethod(
                "a", worldClass, Random.class, int.class, int.class, int.class);

            // The tree *count* comes from the chunk provider's eighth octave
            // generator, so it is reached through a provider rather than
            // through the World. `nw.c` is the field; `lp.a(DD)D` is the
            // sample. Unsafe-allocated, because constructing a provider needs
            // a World and none of the noise depends on one.
            Class<?> providerClass = loader.loadClass("nw");
            Class<?> octaveClass = loader.loadClass("lp");
            java.lang.reflect.Method sample2d =
                octaveClass.getMethod("a", double.class, double.class);
            java.lang.reflect.Field densityField = providerClass.getDeclaredField("c");
            densityField.setAccessible(true);
            java.lang.reflect.Constructor<?> providerCtor =
                providerClass.getConstructor(worldClass, long.class);

            // {scene, rngSeed}
            long[][] cases = {
                {TSCENE_OPEN, 401}, {TSCENE_OPEN, 402}, {TSCENE_OPEN, 403}, {TSCENE_OPEN, 404},
                {TSCENE_CEILING, 405},
                {TSCENE_LEAVES, 406}, {TSCENE_LEAVES, 407},
                {TSCENE_CLIP, 408}, {TSCENE_CLIP, 409}, {TSCENE_CLIP, 410},
            };

            java.io.File dir = new java.io.File(scratchDir, "tree");
            deleteTree(dir);
            dir.mkdirs();

            StringBuilder table = new StringBuilder();
            StringBuilder blocks = new StringBuilder();
            int patchRuns = 0;
            int totalPlacements = 0;

            p("// Generated by tools/genref.java --jar <client.jar> --tree <scratch-dir>.");
            p("// Do not edit by hand.");
            p("//");
            p("// WorldGenTrees (`oa`) against a **real a1.1.2 World**, on four scenes: open");
            p("// ground, a ceiling too low to grow under, a leaf layer it is allowed to grow");
            p("// through, and a wall that clips the canopy.");
            p("//");
            p("// All cases share one site and one seed, so the terrain is carried once and the");
            p("// scene is applied by tests/tree_test.cpp with the same rules used here.");
            p("//");
            p("// Regenerate with:");
            p("//   java tools/genref.java --jar <client.jar> --tree /tmp/genref-scratch");
            p("//     redirected to tests/tree_vectors.hpp");
            p("");
            p("#pragma once");
            p("");
            p("#include \"core/util/types.hpp\"");
            p("");
            p("namespace mc::test {");
            p("");
            p("inline constexpr int kTreePatch = " + TREE_PATCH + ";");
            p("");
            p("enum TreeScene {");
            p("    kTSceneOpen = " + TSCENE_OPEN + ",");
            p("    kTSceneCeiling = " + TSCENE_CEILING + ",");
            p("    kTSceneLeaves = " + TSCENE_LEAVES + ",");
            p("    kTSceneClip = " + TSCENE_CLIP + ",");
            p("};");
            p("");
            p("// Offsets from the case origin in x and z; y is absolute.");
            p("struct TreeBlock {");
            p("    i32 dx;");
            p("    i32 y;");
            p("    i32 dz;");
            p("    u8 id;");
            p("};");
            p("");
            p("// {seed, blockX, blockZ, count, big} -- what `populate` works out before it");
            p("// plants anything. The two nextInt(10) rolls happen whatever the count is.");
            p("struct TreeBatchCase {");
            p("    i64 seed;");
            p("    i64 rngSeed;");
            p("    i32 blockX;");
            p("    i32 blockZ;");
            p("    int count;");
            p("    bool big;");
            p("    i64 afterDraw;");
            p("};");
            p("");
            p("struct TreeCase {");
            p("    int scene;");
            p("    i64 rngSeed;");
            p("    i32 x;");
            p("    i32 y;");
            p("    i32 z;");
            p("    bool planted;     // what oa.a returned");
            p("    i64 afterDraw;    // stream fingerprint");
            p("    int changeCount;");
            p("    const TreeBlock* changes;");
            p("};");
            p("");
            p("// The shared terrain, as (count << 8) | blockId over a");
            p("// (2*kTreePatch+1)^2 patch of full-height columns, x slowest then z then y.");

            for (int c = 0; c < cases.length; c++) {
                final int scene = (int) cases[c][0];
                final long rngSeed = cases[c][1];

                java.io.File caseDir = new java.io.File(dir, "case" + c);
                caseDir.mkdirs();
                Object world = worldCtor.newInstance(caseDir, "genref", 13571357L);
                snowField.setBoolean(world, false);

                int x = 0;
                final int z = 2000;
                boolean found = false;
                for (int attempt = 0; attempt < 200 && !found; attempt++) {
                    x = 2000 + attempt * 40;
                    for (int cx = (x - TREE_PATCH - 2) >> 4; cx <= (x + TREE_PATCH + 2) >> 4; cx++) {
                        for (int cz = (z - TREE_PATCH - 2) >> 4;
                             cz <= (z + TREE_PATCH + 2) >> 4; cz++) {
                            getChunk.invoke(world, cx, cz);
                        }
                    }
                    drainLights(updateLights, world);
                    found = treeSiteIsOpenGrass(getBlock, heightAt, world, x, z);
                }
                if (!found) {
                    throw new IllegalStateException("no site for tree case " + c);
                }

                final int surface = ((Integer) heightAt.invoke(world, x, z)).intValue();

                // Snapshot before, so the fixture can carry only what changed.
                final int span = 2 * TREE_PATCH + 1;
                int[] before = new int[span * span * 128];
                int at = 0;
                for (int dx = -TREE_PATCH; dx <= TREE_PATCH; dx++) {
                    for (int dz = -TREE_PATCH; dz <= TREE_PATCH; dz++) {
                        for (int by = 0; by < 128; by++) {
                            before[at++] = ((Integer) getBlock.invoke(world, x + dx, by, z + dz))
                                               .intValue();
                        }
                    }
                }
                if (c == 0) {
                    patchRuns = emitIntRuns("kTreeTerrain", before);
                }

                buildTreeScene(setBlock, heightAt, world, scene, x, z);

                // Re-snapshot after the scene so the recorded changes are the
                // tree's alone -- **not drained**, because population never is.
                at = 0;
                for (int dx = -TREE_PATCH; dx <= TREE_PATCH; dx++) {
                    for (int dz = -TREE_PATCH; dz <= TREE_PATCH; dz++) {
                        for (int by = 0; by < 128; by++) {
                            before[at++] = ((Integer) getBlock.invoke(world, x + dx, by, z + dz))
                                               .intValue();
                        }
                    }
                }

                Random random = new Random(rngSeed);
                Object gen = treeClass.getConstructor().newInstance();
                final boolean planted = ((Boolean) run.invoke(gen, world, random, x, surface, z))
                                            .booleanValue();
                final long afterDraw = random.nextLong();

                StringBuilder changes = new StringBuilder();
                int count = 0;
                at = 0;
                for (int dx = -TREE_PATCH; dx <= TREE_PATCH; dx++) {
                    for (int dz = -TREE_PATCH; dz <= TREE_PATCH; dz++) {
                        for (int by = 0; by < 128; by++) {
                            final int now =
                                ((Integer) getBlock.invoke(world, x + dx, by, z + dz)).intValue();
                            if (now != before[at]) {
                                changes.append("    {").append(dx).append(", ").append(by)
                                       .append(", ").append(dz).append(", ").append(now)
                                       .append("},\n");
                                count++;
                            }
                            at++;
                        }
                    }
                }
                totalPlacements += count;

                blocks.append("inline constexpr TreeBlock kTree").append(c)
                      .append("[").append(Math.max(count, 1)).append("] = {\n")
                      .append(count == 0 ? "    {0, 0, 0, 0},\n" : changes.toString())
                      .append("};\n");

                table.append("    {").append(scene).append(", ").append(lit(rngSeed)).append(", ")
                     .append(x).append(", ").append(surface).append(", ").append(z).append(", ")
                     .append(planted).append(", ").append(lit(afterDraw)).append(", ")
                     .append(count).append(", kTree").append(c).append("},\n");
            }

            p("");
            p("inline constexpr int kTreeTerrainRuns = " + patchRuns + ";");
            p("");
            System.out.print(blocks);
            p("");
            p("inline constexpr int kTreeCaseCount = " + cases.length + ";");
            p("inline constexpr int kTreeTotalChanges = " + totalPlacements + ";");
            p("");
            p("inline constexpr TreeCase kTreeCases[kTreeCaseCount] = {");
            System.out.print(table);
            p("};");
            p("");

            // The batch counts, over a spread of seeds and chunks.
            long[][] batches = {
                {1234567890L, 11, 0, 0}, {1234567890L, 12, 37, -14},
                {1234567890L, 13, 100, 100}, {-8675309L, 14, 30000, -32768},
                {-8675309L, 15, 0, 0}, {42L, 16, -1600, 3200},
                {42L, 17, 16, 16}, {7L, 18, -48, -48},
            };
            StringBuilder batchTable = new StringBuilder();
            for (long[] row : batches) {
                // The real constructor, over an uninitialised World -- the same
                // trick the terrain oracle uses. None of the noise depends on
                // the World, but the *order* the eight generators are built in
                // is the seed, so they have to be built by the constructor
                // rather than one at a time.
                Object worldInstance = allocateUninitialised(worldClass);
                Object provider = providerCtor.newInstance(worldInstance, row[0]);
                Object density = densityField.get(provider);
                Random rand = new Random(row[1]);
                final double n = ((Double) sample2d.invoke(density, row[2] * 0.5, row[3] * 0.5))
                                     .doubleValue();
                int count = (int) ((n / 8.0 + rand.nextDouble() * 4.0 + 4.0) / 3.0);
                if (count < 0) {
                    count = 0;
                }
                if (rand.nextInt(10) == 0) {
                    count++;
                }
                final boolean big = rand.nextInt(10) == 0;
                batchTable.append("    {").append(lit(row[0])).append(", ").append(lit(row[1]))
                          .append(", ").append(row[2]).append(", ").append(row[3]).append(", ")
                          .append(count).append(", ").append(big).append(", ")
                          .append(lit(rand.nextLong())).append("},\n");
            }
            p("inline constexpr int kTreeBatchCaseCount = " + batches.length + ";");
            p("");
            p("inline constexpr TreeBatchCase kTreeBatchCases[kTreeBatchCaseCount] = {");
            System.out.print(batchTable);
            p("};");
            p("");
            p("}  // namespace mc::test");
        } catch (Exception e) {
            System.err.println("genref --tree failed: " + e);
            e.printStackTrace();
            System.exit(1);
        }
    }

    // The flower probe with a taller clearance requirement: a tree needs about
    // eight blocks of headroom where a flower needs one.
    private static boolean treeSiteIsOpenGrass(java.lang.reflect.Method getBlock,
                                               java.lang.reflect.Method height, Object world,
                                               int x, int z) throws Exception {
        final int centre = ((Integer) height.invoke(world, x, z)).intValue();
        if (centre < 60 || centre > 100) {
            return false;
        }
        for (int bx = x - TREE_PATCH - 1; bx <= x + TREE_PATCH + 1; bx++) {
            for (int bz = z - TREE_PATCH - 1; bz <= z + TREE_PATCH + 1; bz++) {
                final int top = ((Integer) height.invoke(world, bx, bz)).intValue();
                if (Math.abs(top - centre) > 2) {
                    return false;
                }
                if (((Integer) getBlock.invoke(world, bx, top - 1, bz)).intValue() != 2) {
                    return false;
                }
                for (int by = top; by <= top + 10; by++) {
                    if (((Integer) getBlock.invoke(world, bx, by, bz)).intValue() != 0) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    // ---------------------------------------------------------------------
    // WorldGenBigTree (`ej`)
    // ---------------------------------------------------------------------
    //
    // Same site machinery as the ordinary tree -- real terrain, open flat
    // grass, one shared patch -- but a bigger one: a big tree reaches up to
    // seventeen blocks and spreads its clusters several blocks out, so the
    // patch has to be wide enough that the whole tree is inside it.
    //
    // The fingerprint here checks something different from the other
    // generators. `ej` draws **one** nextLong from the shared stream and then
    // runs entirely on its own Random, so the fingerprint is verifying that
    // claim -- that no matter what the tree does internally, the chunk's
    // stream advanced by exactly one draw.

    private static final int BIG_PATCH = 20;

    private static void emitBigTree(String jarPath, String scratchDir) {
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> worldClass = loader.loadClass(WORLD);
            Class<?> bigClass = loader.loadClass("ej");

            java.lang.reflect.Constructor<?> worldCtor =
                worldClass.getConstructor(java.io.File.class, String.class, long.class);
            java.lang.reflect.Field snowField = worldClass.getField(SNOW_FIELD);
            java.lang.reflect.Method getChunk = worldClass.getMethod("b", int.class, int.class);
            java.lang.reflect.Method getBlock =
                worldClass.getMethod("a", int.class, int.class, int.class);
            java.lang.reflect.Method updateLights = worldClass.getMethod("e");
            java.lang.reflect.Method heightAt = worldClass.getMethod("c", int.class, int.class);
            java.lang.reflect.Method setBlockAt =
                worldClass.getMethod("a", int.class, int.class, int.class, int.class);
            java.lang.reflect.Method run = bigClass.getMethod(
                "a", worldClass, Random.class, int.class, int.class, int.class);
            // **`setScale` has to be called, and calling it is not optional.**
            // The driver does `gen.setScale(1.0, 1.0, 1.0)` before every big
            // tree, and 1.0 > 0.5 pushes `leafDistanceLimit` from 4 to 5 --
            // which changes the height of every leaf cluster and the y of every
            // node in the plan. The first version of this oracle skipped it and
            // produced a fixture for a tree the game never builds.
            java.lang.reflect.Method setScale = bigClass.getMethod(
                "a", double.class, double.class, double.class);

            // {rngSeed, obstructed}. **The obstructed cases exist to reach
            // clearLineLength.** On clear ground every check line runs through
            // air and returns -1 whatever rounding it uses, so the difference
            // between the checked line and the drawn line -- truncation versus
            // round-to-nearest, which disagree on about a third of steps -- is
            // invisible. A stone pillar inside the canopy's reach is what makes
            // the two distinguishable.
            long[][] seeds = {
                {501, 0}, {502, 0}, {503, 0}, {504, 0},
                {505, 0}, {506, 0}, {507, 0}, {508, 0},
                {511, 1}, {512, 1}, {513, 1}, {514, 1},
            };

            java.io.File dir = new java.io.File(scratchDir, "bigtree");
            deleteTree(dir);
            dir.mkdirs();

            StringBuilder table = new StringBuilder();
            StringBuilder blocks = new StringBuilder();
            int patchRuns = 0;
            int totalChanges = 0;

            p("// Generated by tools/genref.java --jar <client.jar> --bigtree <scratch-dir>.");
            p("// Do not edit by hand.");
            p("//");
            p("// WorldGenBigTree (`ej`) against a **real a1.1.2 World** on open grass. Every");
            p("// case shares one site and one world seed and differs only in the seed handed");
            p("// to the generator, so the terrain is carried once.");
            p("//");
            p("// `afterDraw` is taken after the call: `ej` draws exactly one nextLong from the");
            p("// stream it is given and runs on its own Random thereafter, so this pins that");
            p("// property rather than a draw count that varies with the tree.");
            p("//");
            p("// Regenerate with:");
            p("//   java tools/genref.java --jar <client.jar> --bigtree /tmp/genref-scratch");
            p("//     redirected to tests/big_tree_vectors.hpp");
            p("");
            p("#pragma once");
            p("");
            p("#include \"core/util/types.hpp\"");
            p("");
            p("namespace mc::test {");
            p("");
            p("inline constexpr int kBigPatch = " + BIG_PATCH + ";");
            p("");
            p("struct BigTreeBlock {");
            p("    i32 dx;");
            p("    i32 y;");
            p("    i32 dz;");
            p("    u8 id;");
            p("};");
            p("");
            p("struct BigTreeCase {");
            p("    i64 rngSeed;");
            p("    bool obstructed;   // pillars ringing the trunk, see genref");
            p("    i32 x;");
            p("    i32 y;");
            p("    i32 z;");
            p("    bool planted;");
            p("    i64 afterDraw;");
            p("    int changeCount;");
            p("    const BigTreeBlock* changes;");
            p("};");
            p("");
            p("// The shared terrain, as (count << 8) | blockId over a");
            p("// (2*kBigPatch+1)^2 patch of full-height columns, x slowest then z then y.");

            for (int c = 0; c < seeds.length; c++) {
                java.io.File caseDir = new java.io.File(dir, "case" + c);
                caseDir.mkdirs();
                Object world = worldCtor.newInstance(caseDir, "genref", 13571357L);
                snowField.setBoolean(world, false);

                int x = 0;
                final int z = 2000;
                boolean found = false;
                for (int attempt = 0; attempt < 400 && !found; attempt++) {
                    x = 2000 + attempt * 48;
                    for (int cx = (x - BIG_PATCH - 2) >> 4; cx <= (x + BIG_PATCH + 2) >> 4; cx++) {
                        for (int cz = (z - BIG_PATCH - 2) >> 4;
                             cz <= (z + BIG_PATCH + 2) >> 4; cz++) {
                            getChunk.invoke(world, cx, cz);
                        }
                    }
                    drainLights(updateLights, world);
                    found = bigTreeSiteIsOpenGrass(getBlock, heightAt, world, x, z);
                }
                if (!found) {
                    throw new IllegalStateException("no site for big tree case " + c);
                }

                final int surface = ((Integer) heightAt.invoke(world, x, z)).intValue();
                final int span = 2 * BIG_PATCH + 1;
                int[] before = new int[span * span * 128];
                int at = 0;
                for (int dx = -BIG_PATCH; dx <= BIG_PATCH; dx++) {
                    for (int dz = -BIG_PATCH; dz <= BIG_PATCH; dz++) {
                        for (int by = 0; by < 128; by++) {
                            before[at++] = ((Integer) getBlock.invoke(world, x + dx, by, z + dz))
                                               .intValue();
                        }
                    }
                }
                if (c == 0) {
                    patchRuns = emitIntRuns("kBigTreeTerrain", before);
                }

                if (seeds[c][1] != 0) {
                    // Four pillars ringing the trunk at the height the canopy
                    // spreads through. Placed before the snapshot below so the
                    // fixture treats them as terrain rather than as something
                    // the tree did.
                    // **A sparse ring, well out from the trunk and high up.**
                    //
                    // Two denser obstructions were tried first and both made
                    // the fixture *worse*: four pillars at radius 3, and then a
                    // block every third cell across the whole canopy. Both sit
                    // close enough to the trunk that `chooseHeight` clips the
                    // tree short -- the obstructed trees came back at 73 to 135
                    // blocks against 500 for a clear one -- and a short tree
                    // plants few clusters and therefore walks few check lines.
                    // Instrumenting it showed 5 to 25 line steps against 125 on
                    // clear ground.
                    //
                    // So the obstruction has to be *outside* the trunk's own
                    // clearance column but *inside* the canopy's horizontal
                    // reach. Radius 5 to 6 and above the trunk top does that.
                    for (int dx = -6; dx <= 6; dx++) {
                        for (int dz = -6; dz <= 6; dz++) {
                            final int reach = Math.max(Math.abs(dx), Math.abs(dz));
                            if (reach < 5) {
                                continue;
                            }
                            if (((dx + 6) % 2 != 0) || ((dz + 6) % 2 != 0)) {
                                continue;
                            }
                            for (int by = surface + 8; by <= surface + 14; by += 2) {
                                setBlockAt.invoke(world, x + dx, by, z + dz, 1);
                            }
                        }
                    }
                    at = 0;
                    for (int dx = -BIG_PATCH; dx <= BIG_PATCH; dx++) {
                        for (int dz = -BIG_PATCH; dz <= BIG_PATCH; dz++) {
                            for (int by = 0; by < 128; by++) {
                                before[at++] =
                                    ((Integer) getBlock.invoke(world, x + dx, by, z + dz))
                                        .intValue();
                            }
                        }
                    }
                }

                Random random = new Random(seeds[c][0]);
                // A fresh generator per tree, which is what the driver's own
                // `new ej()` per chunk gives -- and it matters, because the
                // class keeps its height between calls.
                Object gen = bigClass.getConstructor().newInstance();
                setScale.invoke(gen, 1.0, 1.0, 1.0);
                final boolean planted = ((Boolean) run.invoke(gen, world, random, x, surface, z))
                                            .booleanValue();
                final long afterDraw = random.nextLong();

                StringBuilder changes = new StringBuilder();
                int count = 0;
                at = 0;
                for (int dx = -BIG_PATCH; dx <= BIG_PATCH; dx++) {
                    for (int dz = -BIG_PATCH; dz <= BIG_PATCH; dz++) {
                        for (int by = 0; by < 128; by++) {
                            final int now =
                                ((Integer) getBlock.invoke(world, x + dx, by, z + dz)).intValue();
                            if (now != before[at]) {
                                changes.append("    {").append(dx).append(", ").append(by)
                                       .append(", ").append(dz).append(", ").append(now)
                                       .append("},\n");
                                count++;
                            }
                            at++;
                        }
                    }
                }
                totalChanges += count;

                blocks.append("inline constexpr BigTreeBlock kBigTree").append(c)
                      .append("[").append(Math.max(count, 1)).append("] = {\n")
                      .append(count == 0 ? "    {0, 0, 0, 0},\n" : changes.toString())
                      .append("};\n");

                table.append("    {").append(lit(seeds[c][0])).append(", ")
                     .append(seeds[c][1] != 0).append(", ").append(x).append(", ")
                     .append(surface).append(", ").append(z).append(", ").append(planted)
                     .append(", ").append(lit(afterDraw)).append(", ").append(count)
                     .append(", kBigTree").append(c).append("},\n");
            }

            p("");
            p("inline constexpr int kBigTreeTerrainRuns = " + patchRuns + ";");
            p("");
            System.out.print(blocks);
            p("");
            p("inline constexpr int kBigTreeCaseCount = " + seeds.length + ";");
            p("inline constexpr int kBigTreeTotalChanges = " + totalChanges + ";");
            p("");
            p("inline constexpr BigTreeCase kBigTreeCases[kBigTreeCaseCount] = {");
            System.out.print(table);
            p("};");
            p("");
            p("}  // namespace mc::test");
        } catch (Exception e) {
            System.err.println("genref --bigtree failed: " + e);
            e.printStackTrace();
            System.exit(1);
        }
    }

    // **The flatness requirement is deliberately narrower than the patch.**
    // Demanding 41x41 of grass flat to +-2 finds nothing in real a1.1.2
    // terrain -- the first version of this probe walked 400 sites and rejected
    // every one. A big tree only *stands* on the centre column and only
    // spreads a handful of blocks, so the strict test covers a radius of 8 and
    // the rest of the patch is recorded as-is, whatever it contains. Anything
    // the tree collides with out there is terrain the fixture carries, and our
    // side rebuilds it identically.
    private static final int BIG_SITE_RADIUS = 8;

    private static boolean bigTreeSiteIsOpenGrass(java.lang.reflect.Method getBlock,
                                                  java.lang.reflect.Method height, Object world,
                                                  int x, int z) throws Exception {
        final int centre = ((Integer) height.invoke(world, x, z)).intValue();
        if (centre < 60 || centre > 95) {
            return false;
        }
        for (int bx = x - BIG_SITE_RADIUS; bx <= x + BIG_SITE_RADIUS; bx++) {
            for (int bz = z - BIG_SITE_RADIUS; bz <= z + BIG_SITE_RADIUS; bz++) {
                final int top = ((Integer) height.invoke(world, bx, bz)).intValue();
                if (Math.abs(top - centre) > 2) {
                    return false;
                }
                if (((Integer) getBlock.invoke(world, bx, top - 1, bz)).intValue() != 2) {
                    return false;
                }
                // A big tree is up to seventeen tall; give it room plus slack.
                for (int by = top; by <= top + 22; by++) {
                    if (((Integer) getBlock.invoke(world, bx, by, bz)).intValue() != 0) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    // ---------------------------------------------------------------------
    // WorldGenDungeons (`cg`)
    // ---------------------------------------------------------------------
    //
    // The only generator that produces something other than blocks: a mob
    // spawner with a mob name, and up to two chests with contents. So this
    // fixture carries three things per case -- the block changes, the
    // spawner's mob, and every stack in every chest with its slot.
    //
    // **The scene is built rather than probed.** A dungeon needs a room-shaped
    // void inside solid rock with between one and five openings in its wall,
    // and terrain that happens to look like that is rare enough that a probe
    // would walk thousands of sites. Instead a solid block of stone is written
    // underground and a cavity carved in it, with a doorway punched through
    // one wall to give the opening count something to find.

    private static final int DUNGEON_PATCH = 14;
    private static final int DUNGEON_Y = 40;

    // **The cavity has to match the room, not merely contain it.**
    //
    // The first version carved 11x11 and every case was refused. The generator
    // inspects a shell at exactly `spanX + 1` and `spanZ + 1` from the centre,
    // which is at most 4 -- so a wider cavity puts nothing but air where it
    // looks for a wall, it counts zero openings, and zero is below the minimum
    // of one. A dungeon does not go in a big cave; it goes in a hole its own
    // size.
    //
    // `spanX` and `spanZ` are `nextInt(2) + 2`, so 2 or 3. Carving at 3 means
    // the shell lands on solid stone when the roll is 2 and one block inside
    // the carved air when it is 3 -- both are real cases and the fixture wants
    // both, so the cavity is fixed and the roll varies.
    private static final int CAVITY_X = 3;
    private static final int CAVITY_Z = 3;
    private static final int CAVITY_H = 3;

    private static void buildDungeonScene(java.lang.reflect.Method setBlock, Object world,
                                          int x, int z, int doorways) throws Exception {
        final int y = DUNGEON_Y;

        // Solid stone everywhere in the patch, top to bottom of the working
        // range. Written before the cavity so nothing borders open air while
        // it is being placed.
        for (int bx = x - DUNGEON_PATCH; bx <= x + DUNGEON_PATCH; bx++) {
            for (int bz = z - DUNGEON_PATCH; bz <= z + DUNGEON_PATCH; bz++) {
                for (int by = y - 6; by <= y + 10; by++) {
                    setBlock.invoke(world, bx, by, bz, 1);
                }
            }
        }

        // The cavity.
        for (int bx = x - CAVITY_X; bx <= x + CAVITY_X; bx++) {
            for (int bz = z - CAVITY_Z; bz <= z + CAVITY_Z; bz++) {
                for (int by = y; by <= y + CAVITY_H; by++) {
                    setBlock.invoke(world, bx, by, bz, 0);
                }
            }
        }

        // Doorways: two-block-tall holes punched out through the +x wall at the
        // room's own floor level, which is what the opening count looks for.
        // They start on the shell itself (`CAVITY_X + 1`) because that is the
        // ring the generator inspects.
        for (int d = 0; d < doorways && d < 2 * CAVITY_Z; d++) {
            final int bz = z - CAVITY_Z + d;
            for (int bx = x + CAVITY_X + 1; bx <= x + CAVITY_X + 3; bx++) {
                setBlock.invoke(world, bx, y, bz, 0);
                setBlock.invoke(world, bx, y + 1, bz, 0);
            }
        }
    }

    private static void emitDungeon(String jarPath, String scratchDir) {
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> worldClass = loader.loadClass(WORLD);
            Class<?> dungeonClass = loader.loadClass("cg");
            Class<?> chestClass = loader.loadClass("fe");
            Class<?> spawnerClass = loader.loadClass("bd");
            Class<?> stackClass = loader.loadClass("ev");

            java.lang.reflect.Constructor<?> worldCtor =
                worldClass.getConstructor(java.io.File.class, String.class, long.class);
            java.lang.reflect.Field snowField = worldClass.getField(SNOW_FIELD);
            java.lang.reflect.Method getChunk = worldClass.getMethod("b", int.class, int.class);
            java.lang.reflect.Method getBlock =
                worldClass.getMethod("a", int.class, int.class, int.class);
            java.lang.reflect.Method setBlock =
                worldClass.getMethod("a", int.class, int.class, int.class, int.class);
            java.lang.reflect.Method updateLights = worldClass.getMethod("e");
            // `cn.b(III)` -- getBlockTileEntity.
            java.lang.reflect.Method tileEntityAt =
                worldClass.getMethod("b", int.class, int.class, int.class);
            java.lang.reflect.Method run = dungeonClass.getMethod(
                "a", worldClass, Random.class, int.class, int.class, int.class);

            // `fe.c(I)Lev;` -- getStackInSlot; `fe.c()I` -- getSizeInventory.
            // **Two methods called `c`, told apart only by signature**, which is
            // why this is spelled out rather than looked up by name alone. And
            // `fe.a(I, Lev;)` is the setter, not a getter.
            java.lang.reflect.Method chestSlot = chestClass.getMethod("c", int.class);
            java.lang.reflect.Method chestSize = chestClass.getMethod("c");
            java.lang.reflect.Field spawnerMob = spawnerClass.getField("b");
            // Read off `ev.<init>(II)`: the first argument lands in `c` and
            // the second in `a`, so `c` is the id and `a` is the count. Not
            // guessed from the field order.
            java.lang.reflect.Field stackId = stackClass.getField("c");
            java.lang.reflect.Field stackCountField = stackClass.getField("a");
            java.lang.reflect.Field stackDamage = stackClass.getField("b");

            // {rngSeed, doorways}
            long[][] cases = {
                {601, 1}, {602, 1}, {603, 1}, {604, 2}, {605, 2},
                {606, 3}, {607, 3}, {608, 4}, {609, 5},
                // Six doorways: past the limit, so the room is refused. The
                // two size rolls are still spent, which is what the stream
                // fingerprint checks.
                {610, 6},
                {611, 1}, {612, 2}, {613, 1}, {614, 2},
            };

            java.io.File dir = new java.io.File(scratchDir, "dungeon");
            deleteTree(dir);
            dir.mkdirs();

            StringBuilder table = new StringBuilder();
            StringBuilder blocks = new StringBuilder();
            int totalChanges = 0;
            int totalStacks = 0;

            p("// Generated by tools/genref.java --jar <client.jar> --dungeon <scratch-dir>.");
            p("// Do not edit by hand.");
            p("//");
            p("// WorldGenDungeons (`cg`) against a **real a1.1.2 World** in a built scene: a");
            p("// block of stone with a cavity carved in it and a variable number of doorways,");
            p("// because the generator counts wall openings and refuses outside 1..5.");
            p("//");
            p("// Each case carries the block changes, the spawner's mob name, and every stack");
            p("// in every chest with its slot -- the only generator whose output is not");
            p("// entirely blocks.");
            p("//");
            p("// Regenerate with:");
            p("//   java tools/genref.java --jar <client.jar> --dungeon /tmp/genref-scratch");
            p("//     redirected to tests/dungeon_vectors.hpp");
            p("");
            p("#pragma once");
            p("");
            p("#include \"core/util/types.hpp\"");
            p("");
            p("namespace mc::test {");
            p("");
            p("inline constexpr int kDungeonPatch = " + DUNGEON_PATCH + ";");
            p("inline constexpr int kDungeonY = " + DUNGEON_Y + ";");
            p("inline constexpr int kCavityX = " + CAVITY_X + ";");
            p("inline constexpr int kCavityZ = " + CAVITY_Z + ";");
            p("inline constexpr int kCavityH = " + CAVITY_H + ";");
            p("");
            p("struct DungeonBlock {");
            p("    i32 dx;");
            p("    i32 y;");
            p("    i32 dz;");
            p("    u8 id;");
            p("};");
            p("");
            p("struct DungeonStack {");
            p("    i32 chest;   // index into this case's chests");
            p("    i32 slot;");
            p("    i32 id;");
            p("    i32 count;");
            p("    i32 damage;");
            p("};");
            p("");
            p("struct DungeonChestAt {");
            p("    i32 dx;");
            p("    i32 y;");
            p("    i32 dz;");
            p("};");
            p("");
            p("struct DungeonCase {");
            p("    i64 rngSeed;");
            p("    int doorways;");
            p("    i32 x;");
            p("    i32 y;");
            p("    i32 z;");
            p("    bool placed;");
            p("    i64 afterDraw;");
            p("    const char* mob;");
            p("    int chestCount;");
            p("    const DungeonChestAt* chests;");
            p("    int stackCount;");
            p("    const DungeonStack* stacks;");
            p("    int changeCount;");
            p("    const DungeonBlock* changes;");
            p("};");
            p("");

            for (int c = 0; c < cases.length; c++) {
                final long rngSeed = cases[c][0];
                final int doorways = (int) cases[c][1];

                java.io.File caseDir = new java.io.File(dir, "case" + c);
                caseDir.mkdirs();
                Object world = worldCtor.newInstance(caseDir, "genref", 24682468L);
                snowField.setBoolean(world, false);

                final int x = 3000 + c * 64;
                final int z = 3000;
                for (int cx = (x - DUNGEON_PATCH) >> 4; cx <= (x + DUNGEON_PATCH) >> 4; cx++) {
                    for (int cz = (z - DUNGEON_PATCH) >> 4;
                         cz <= (z + DUNGEON_PATCH) >> 4; cz++) {
                        getChunk.invoke(world, cx, cz);
                    }
                }
                drainLights(updateLights, world);
                buildDungeonScene(setBlock, world, x, z, doorways);
                drainLights(updateLights, world);

                final int span = 2 * DUNGEON_PATCH + 1;
                int[] before = new int[span * span * 128];
                int at = 0;
                for (int dx = -DUNGEON_PATCH; dx <= DUNGEON_PATCH; dx++) {
                    for (int dz = -DUNGEON_PATCH; dz <= DUNGEON_PATCH; dz++) {
                        for (int by = 0; by < 128; by++) {
                            before[at++] = ((Integer) getBlock.invoke(world, x + dx, by, z + dz))
                                               .intValue();
                        }
                    }
                }

                Random random = new Random(rngSeed);
                Object gen = dungeonClass.getConstructor().newInstance();
                final boolean placed =
                    ((Boolean) run.invoke(gen, world, random, x, DUNGEON_Y, z)).booleanValue();
                final long afterDraw = random.nextLong();

                StringBuilder changes = new StringBuilder();
                StringBuilder chestsAt = new StringBuilder();
                StringBuilder stacks = new StringBuilder();
                int count = 0;
                int chestCount = 0;
                int stackCount = 0;
                String mob = "";

                at = 0;
                for (int dx = -DUNGEON_PATCH; dx <= DUNGEON_PATCH; dx++) {
                    for (int dz = -DUNGEON_PATCH; dz <= DUNGEON_PATCH; dz++) {
                        for (int by = 0; by < 128; by++) {
                            final int now =
                                ((Integer) getBlock.invoke(world, x + dx, by, z + dz)).intValue();
                            if (now != before[at]) {
                                changes.append("    {").append(dx).append(", ").append(by)
                                       .append(", ").append(dz).append(", ").append(now)
                                       .append("},\n");
                                count++;
                            }
                            at++;
                        }
                    }
                }

                if (placed) {
                    Object spawner = tileEntityAt.invoke(world, x, DUNGEON_Y, z);
                    mob = (String) spawnerMob.get(spawner);

                    // Walk the room for chests, in the same order the fixture
                    // will be read back.
                    for (int dx = -DUNGEON_PATCH; dx <= DUNGEON_PATCH; dx++) {
                        for (int dz = -DUNGEON_PATCH; dz <= DUNGEON_PATCH; dz++) {
                            for (int by = 0; by < 128; by++) {
                                final int id =
                                    ((Integer) getBlock.invoke(world, x + dx, by, z + dz))
                                        .intValue();
                                if (id != 54) {
                                    continue;
                                }
                                Object chest = tileEntityAt.invoke(world, x + dx, by, z + dz);
                                chestsAt.append("    {").append(dx).append(", ").append(by)
                                        .append(", ").append(dz).append("},\n");
                                final int size = ((Integer) chestSize.invoke(chest)).intValue();
                                for (int slot = 0; slot < size; slot++) {
                                    Object stack = chestSlot.invoke(chest, slot);
                                    if (stack == null) {
                                        continue;
                                    }
                                    stacks.append("    {").append(chestCount).append(", ")
                                          .append(slot).append(", ")
                                          .append(stackId.getInt(stack)).append(", ")
                                          .append(stackCountField.getInt(stack)).append(", ")
                                          .append(stackDamage.getInt(stack)).append("},\n");
                                    stackCount++;
                                }
                                chestCount++;
                            }
                        }
                    }
                }

                totalChanges += count;
                totalStacks += stackCount;

                blocks.append("inline constexpr DungeonBlock kDungeonBlocks").append(c)
                      .append("[").append(Math.max(count, 1)).append("] = {\n")
                      .append(count == 0 ? "    {0, 0, 0, 0},\n" : changes.toString())
                      .append("};\n");
                blocks.append("inline constexpr DungeonChestAt kDungeonChests").append(c)
                      .append("[").append(Math.max(chestCount, 1)).append("] = {\n")
                      .append(chestCount == 0 ? "    {0, 0, 0},\n" : chestsAt.toString())
                      .append("};\n");
                blocks.append("inline constexpr DungeonStack kDungeonStacks").append(c)
                      .append("[").append(Math.max(stackCount, 1)).append("] = {\n")
                      .append(stackCount == 0 ? "    {0, 0, 0, 0, 0},\n" : stacks.toString())
                      .append("};\n");

                table.append("    {").append(lit(rngSeed)).append(", ").append(doorways)
                     .append(", ").append(x).append(", ").append(DUNGEON_Y).append(", ")
                     .append(z).append(", ").append(placed).append(", ").append(lit(afterDraw))
                     .append(", \"").append(mob).append("\", ")
                     .append(chestCount).append(", kDungeonChests").append(c).append(", ")
                     .append(stackCount).append(", kDungeonStacks").append(c).append(", ")
                     .append(count).append(", kDungeonBlocks").append(c).append("},\n");
            }

            System.out.print(blocks);
            p("");
            p("inline constexpr int kDungeonCaseCount = " + cases.length + ";");
            p("inline constexpr int kDungeonTotalChanges = " + totalChanges + ";");
            p("inline constexpr int kDungeonTotalStacks = " + totalStacks + ";");
            p("");
            p("inline constexpr DungeonCase kDungeonCases[kDungeonCaseCount] = {");
            System.out.print(table);
            p("};");
            p("");
            p("}  // namespace mc::test");
        } catch (Exception e) {
            System.err.println("genref --dungeon failed: " + e);
            e.printStackTrace();
            System.exit(1);
        }
    }

    // ---------------------------------------------------------------------
    // The whole population pass (`nw.a(aw, int, int)`)
    // ---------------------------------------------------------------------
    //
    // **The check no per-generator fixture can make.** Every generator is
    // already verified block-for-block on its own, but they share one Random,
    // so the order they run in and the number of draws each makes are as much
    // a part of the seed as their contents. One pass out of place, or one draw
    // too many, moves everything after it -- and every generator still passes
    // its own test.
    //
    // The trick is getting a *before* and an *after* of the same chunk out of
    // a World that populates eagerly. `ft.b(II)` populates as soon as a **2x2
    // quadrant** exists -- the chunk at (x, z) is populated once (x, z),
    // (x+1, z), (x, z+1) and (x+1, z+1) are all loaded, and the same call
    // checks three further quadrants for its neighbours. It is not the "all
    // eight neighbours" rule it is often described as, which matters here:
    // a plus-shaped neighbourhood was the first attempt and it populated the
    // centre anyway, because the plus contains (cx, cz), (cx+1, cz) and
    // (cx, cz+1) and the trigger only needed the fourth.
    //
    // So the "before" world loads a **column strip** -- the centre and its two
    // z-neighbours, and nothing at cx+1 or cx-1 -- which cannot complete any
    // quadrant. The "after" world of the same seed loads the full 5x5. Both
    // are deterministic, so the pair really is one chunk before and after, and
    // `ga.n` (isTerrainPopulated) is asserted on both sides rather than
    // assumed.

    private static void emitPopulate(String jarPath, String scratchDir) {
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> worldClass = loader.loadClass(WORLD);
            Class<?> chunkClass = loader.loadClass("ga");

            java.lang.reflect.Constructor<?> worldCtor =
                worldClass.getConstructor(java.io.File.class, String.class, long.class);
            java.lang.reflect.Field snowField = worldClass.getField(SNOW_FIELD);
            java.lang.reflect.Method getChunk = worldClass.getMethod("b", int.class, int.class);
            java.lang.reflect.Field chunkBlocks = chunkClass.getField("b");
            // **`ga.n`, not `ga.p`** -- isTerrainPopulated. Read off `ft`'s
            // bytecode, where the trigger tests `getfield ga.n` before calling
            // populate; `ga.p` is a different boolean that stays false. Using
            // the wrong one made the "after" assertion fail against a world
            // that had in fact populated correctly.
            java.lang.reflect.Field populatedField = chunkClass.getField("n");

            // {seed, chunkX, chunkZ, snowCovered}
            // **All far from the origin.** The World constructor runs a spawn
            // search, which generates chunks around (0, 0) before any of this
            // code sees the world -- so a case anywhere near there finds its
            // "lone" chunks already populated and the before/after pair is
            // meaningless. `{0, 0}` and `{12, 12}` were in the first version
            // and the one-per-world assertion caught both.
            long[][] cases = {
                {1234567890L, 37, -14, 0},
                {1234567890L, 512, 512, 0},
                {-8675309L, 1875, -2048, 0},
                {42L, -100, 200, 0},
                {7L, 300, -300, 0},
                // Snow on, which turns the final sweep from a no-op into a
                // pass that writes a block on most columns.
                {1234567890L, 37, -14, 1},
            };

            // **Deleted, not merely created.** A leftover directory from an
            // earlier run leaves chunk files on disk, `ft.a(II)` counts a
            // saved chunk as existing, and quadrants complete that the caller
            // never loaded -- so neighbours populate too and the fixture
            // silently carries several chunks' work. Two rounds of confusing
            // diffs traced back to exactly that.
            java.io.File dir = new java.io.File(scratchDir, "populate");
            deleteTree(dir);
            dir.mkdirs();

            p("// Generated by tools/genref.java --jar <client.jar> --populate <scratch-dir>.");
            p("// Do not edit by hand.");
            p("//");
            p("// The **whole population pass** against a real a1.1.2 World: the 5x5 of chunk");
            p("// columns before population runs, and the same 5x5 after. Comparing the pair");
            p("// checks the driver's ordering and every generator's draw count at once, which");
            p("// no per-generator fixture can do.");
            p("//");
            p("// `before` is read from a World given only a plus-shaped neighbourhood, which");
            p("// is never enough to trigger population; `after` from a second World of the");
            p("// same seed given the full 5x5. See tools/genref.java.");
            p("//");
            p("// Regenerate with:");
            p("//   java tools/genref.java --jar <client.jar> --populate /tmp/genref-scratch");
            p("//     redirected to tests/populate_vectors.hpp");
            p("");
            p("#pragma once");
            p("");
            p("#include \"core/util/types.hpp\"");
            p("");
            p("namespace mc::test {");
            p("");
            p("struct PopulateCase {");
            p("    i64 seed;");
            p("    i32 chunkX;");
            p("    i32 chunkZ;");
            p("    bool snowCovered;");
            p("    // **Twenty-five columns, x slowest**: (ix, iz) covers chunk");
            p("    // (chunkX - 2 + ix, chunkZ - 2 + iz).");
            p("    //");
            p("    // 5x5 and not 3x3, and that was measured rather than chosen. The ore");
            p("    // generator adds +8 to the coordinate it is handed and then grows a vein");
            p("    // several blocks either way, so a single chunk's population writes from");
            p("    // ten blocks *below* its own origin to twenty-five above -- 36 blocks of");
            p("    // span against a 16-block chunk. A 3x3 clips both ends.");
            p("    int beforeRuns[25];");
            p("    const u32* before[25];");
            p("    int afterRuns[25];");
            p("    const u32* after[25];");
            p("};");
            p("");

            StringBuilder table = new StringBuilder();
            for (int c = 0; c < cases.length; c++) {
                final long seed = cases[c][0];
                final int cx = (int) cases[c][1];
                final int cz = (int) cases[c][2];
                final boolean snowy = cases[c][3] != 0;

                // ---- before -------------------------------------------------
                // **One chunk per World, nine Worlds.** Anything less is not
                // enough: a quadrant is completed by (x,z), (x+1,z), (x,z+1)
                // and (x+1,z+1), and `ft.b(II)` checks four overlapping
                // quadrants per call, so even a single column of three
                // z-adjacent chunks triggers population. Only a World that has
                // ever seen exactly one chunk is guaranteed to have populated
                // nothing.
                //
                // Nine Worlds per case is slow and completely reliable, and
                // they agree with each other because the seed and coordinates
                // are the same -- the determinism the terrain fixtures already
                // pin. The assertion below is what proves the trick worked
                // rather than the comment.
                StringBuilder beforeRuns = new StringBuilder();
                for (int ix = 0; ix < 5; ix++) {
                    for (int iz = 0; iz < 5; iz++) {
                        java.io.File soloDir =
                            new java.io.File(dir, "before" + c + "_" + ix + "_" + iz);
                        soloDir.mkdirs();
                        Object solo = worldCtor.newInstance(soloDir, "genref", seed);
                        snowField.setBoolean(solo, snowy);

                        Object chunk = getChunk.invoke(solo, cx - 2 + ix, cz - 2 + iz);
                        if (((Boolean) populatedField.get(chunk)).booleanValue()) {
                            throw new IllegalStateException(
                                "a lone chunk was populated at " + (cx - 2 + ix) + "," +
                                (cz - 2 + iz) + "; the one-per-world trick broke");
                        }
                        byte[] blocks = (byte[]) chunkBlocks.get(chunk);
                        int runs = emitRuns("kPopBefore" + c + "_" + (ix * 5 + iz), blocks);
                        beforeRuns.append(runs).append(", ");
                    }
                }

                // ---- after --------------------------------------------------
                //
                // **Exactly one chunk's population, not nine.** Loading a 5x5
                // populates the centre *and* its neighbours, so the resulting
                // 3x3 carries nine overlapping populations and comparing it
                // against a single `populateChunk` call is comparing different
                // things. The first version of this fixture did that, and the
                // test failed with our side showing ~500 changes against the
                // jar's ~1900 -- the missing 1400 were the neighbours' work,
                // not ours.
                //
                // So the "after" world loads a 2x2 quadrant, which is the
                // minimum that triggers population and triggers it for exactly
                // one chunk: `ft.b(II)` populates (x,z) when (x,z), (x+1,z),
                // (x,z+1) and (x+1,z+1) are present, and with only those four
                // loaded no other chunk can complete a quadrant of its own.
                // The quadrant is anchored at the chunk under test: loading
                // (cx, cz)..(cx+1, cz+1) populates exactly `(cx, cz)`. Measured
                // across all four candidate anchors rather than reasoned about,
                // because reasoning about it got the answer wrong twice.
                java.io.File afterDir = new java.io.File(dir, "after" + c);
                afterDir.mkdirs();
                Object afterWorld = worldCtor.newInstance(afterDir, "genref", seed);
                snowField.setBoolean(afterWorld, snowy);

                getChunk.invoke(afterWorld, cx, cz);
                getChunk.invoke(afterWorld, cx + 1, cz);
                getChunk.invoke(afterWorld, cx, cz + 1);
                getChunk.invoke(afterWorld, cx + 1, cz + 1);

                // **Checked over the four that were loaded, and only those.**
                // Asking about a fifth would load it, which can complete
                // another quadrant and populate another chunk -- the check
                // would then be reporting damage it caused itself. A 5x5 scan
                // was the first version and it turned one populated chunk into
                // six.
                int populatedCount = 0;
                for (int dx = 0; dx <= 1; dx++) {
                    for (int dz = 0; dz <= 1; dz++) {
                        Object chunk = getChunk.invoke(afterWorld, cx + dx, cz + dz);
                        if (((Boolean) populatedField.get(chunk)).booleanValue()) {
                            populatedCount++;
                        }
                    }
                }
                if (populatedCount != 1) {
                    throw new IllegalStateException(
                        "expected exactly one populated chunk, got " + populatedCount);
                }
                if (!((Boolean) populatedField.get(getChunk.invoke(afterWorld, cx, cz)))
                         .booleanValue()) {
                    throw new IllegalStateException("the centre chunk was never populated");
                }

                // **One World per column of the readout, and each reads only
                // the four chunks of its own quadrant.**
                //
                // The obvious version -- populate once, then read the 5x5 out
                // of that World -- does not work, and this is the trap that
                // cost the most time here. Every `getChunk` for a column
                // outside the quadrant *loads* it, which completes further
                // quadrants and populates them: measured, reading out a 5x5
                // after loading one quadrant leaves **sixteen** chunks
                // populated, and the fixture then holds sixteen chunks' work
                // where the test drives one.
                //
                // So each of the 25 columns is fetched from its own fresh
                // World, which loads the same quadrant (populating the same
                // one chunk) and then reads exactly one of those four. Any
                // column outside that quadrant is unreachable this way and is
                // simply the unpopulated terrain -- which is correct, because
                // one chunk's population does not reach that far.
                StringBuilder afterRuns = new StringBuilder();
                for (int ix = 0; ix < 5; ix++) {
                    for (int iz = 0; iz < 5; iz++) {
                        final int tx = cx - 2 + ix;
                        final int tz = cz - 2 + iz;
                        final boolean inQuadrant =
                            (tx == cx || tx == cx + 1) && (tz == cz || tz == cz + 1);

                        java.io.File colDir =
                            new java.io.File(dir, "after" + c + "_" + ix + "_" + iz);
                        colDir.mkdirs();
                        Object colWorld = worldCtor.newInstance(colDir, "genref", seed);
                        snowField.setBoolean(colWorld, snowy);

                        Object chunk;
                        if (inQuadrant) {
                            // Load the quadrant in the same order every time,
                            // so the same chunk populates, then keep the one
                            // we want.
                            Object[] quad = new Object[4];
                            int qi = 0;
                            for (int dx = 0; dx <= 1; dx++) {
                                for (int dz = 0; dz <= 1; dz++) {
                                    quad[qi++] = getChunk.invoke(colWorld, cx + dx, cz + dz);
                                }
                            }
                            chunk = quad[(tx - cx) * 2 + (tz - cz)];
                        } else {
                            chunk = getChunk.invoke(colWorld, tx, tz);
                            if (((Boolean) populatedField.get(chunk)).booleanValue()) {
                                throw new IllegalStateException(
                                    "an out-of-quadrant column populated at " + tx + "," + tz);
                            }
                        }

                        byte[] blocks = (byte[]) chunkBlocks.get(chunk);
                        int runs = emitRuns("kPopAfter" + c + "_" + (ix * 5 + iz), blocks);
                        afterRuns.append(runs).append(", ");
                    }
                }

                table.append("    {").append(lit(seed)).append(", ").append(cx).append(", ")
                     .append(cz).append(", ").append(snowy).append(",\n     {")
                     .append(beforeRuns.substring(0, beforeRuns.length() - 2)).append("},\n     {");
                for (int i = 0; i < 25; i++) {
                    table.append("kPopBefore").append(c).append("_").append(i)
                         .append(i == 24 ? "" : ", ");
                }
                table.append("},\n     {")
                     .append(afterRuns.substring(0, afterRuns.length() - 2)).append("},\n     {");
                for (int i = 0; i < 25; i++) {
                    table.append("kPopAfter").append(c).append("_").append(i)
                         .append(i == 24 ? "" : ", ");
                }
                table.append("}},\n");
            }

            p("inline constexpr int kPopulateCaseCount = " + cases.length + ";");
            p("");
            p("inline constexpr PopulateCase kPopulateCases[kPopulateCaseCount] = {");
            System.out.print(table);
            p("};");
            p("");
            p("}  // namespace mc::test");
        } catch (Exception e) {
            System.err.println("genref --populate failed: " + e);
            e.printStackTrace();
            System.exit(1);
        }
    }

    private static void emitOre(String jarPath, String scratchDir) {
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> worldClass = loader.loadClass(WORLD);
            Class<?> minable = loader.loadClass("cu");

            java.lang.reflect.Constructor<?> worldCtor =
                worldClass.getConstructor(java.io.File.class, String.class, long.class);
            java.lang.reflect.Field snowField = worldClass.getField(SNOW_FIELD);
            java.lang.reflect.Method getChunk = worldClass.getMethod("b", int.class, int.class);
            java.lang.reflect.Method getBlock =
                worldClass.getMethod("a", int.class, int.class, int.class);
            java.lang.reflect.Method setBlock =
                worldClass.getMethod("a", int.class, int.class, int.class, int.class);

            java.lang.reflect.Constructor<?> oreCtor = minable.getConstructor(int.class, int.class);
            java.lang.reflect.Method generate =
                minable.getMethod("a", worldClass, Random.class, int.class, int.class, int.class);

            // {rngSeed, blockId, veinSize, x, y, z} -- the seven real ore passes
            // plus a couple of edge positions.
            long[][] cases = {
                {1L, 3, 32, 400, 64, 400},      // dirt
                {2L, 13, 32, 400, 64, 400},     // gravel
                {3L, 16, 16, 400, 40, 400},     // coal
                {4L, 15, 8, 400, 32, 400},      // iron
                {5L, 14, 8, 400, 24, 400},      // gold
                {6L, 73, 7, 400, 12, 400},      // redstone
                {7L, 56, 7, 400, 12, 400},      // diamond
                {8L, 16, 16, 400, 2, 400},      // clipped at the world floor
                {9L, 16, 16, 400, 125, 400},    // clipped at the world ceiling
                {10L, 16, 16, 400, 64, 400},    // a different roll at the same spot
                // Clay, marked by a sentinel block id: the harness fills the
                // box with sand and puts water at the origin instead.
                {11L, CLAY_MARKER, 32, 400, 64, 400},
                {12L, CLAY_MARKER, 32, 400, 40, 400},
                {13L, CLAY_MARKER, 32, 400, 100, 400},
            };

            p("// Generated by tools/genref.java --jar <client.jar> --ore <scratch-dir>.");
            p("// Do not edit by hand.");
            p("//");
            p("// WorldGenMinable run against a **real a1.1.2 World** with a stone-filled box");
            p("// around the target, so the fixture carries the vein and not the terrain.");
            p("// See tools/genref.java.");
            p("//");
            p("// Regenerate with:");
            p("//   java tools/genref.java --jar <client.jar> --ore /tmp/genref-scratch");
            p("//     redirected to tests/ore_vectors.hpp");
            p("");
            p("#pragma once");
            p("");
            p("#include \"core/util/types.hpp\"");
            p("");
            p("namespace mc::test {");
            p("");
            p("// Half-extent of the stone box the vein is grown inside.");
            p("inline constexpr int kOreBox = " + ORE_BOX + ";");
            p("");
            p("struct OrePlacement {");
            p("    i32 x;");
            p("    i32 y;");
            p("    i32 z;");
            p("    u8 id;");
            p("};");
            p("");
            p("struct OreCase {");
            p("    i64 rngSeed;");
            p("    i32 blockId;");
            p("    i32 veinSize;");
            p("    i32 x;");
            p("    i32 y;");
            p("    i32 z;");
            p("    int placementCount;");
            p("    const OrePlacement* placements;");
            p("};");
            p("");

            java.io.File dir = new java.io.File(scratchDir, "ore");
            deleteTree(dir);
            dir.mkdirs();
            Object world = worldCtor.newInstance(dir, "genref", 987654321L);
            snowField.setBoolean(world, false);

            StringBuilder table = new StringBuilder();
            for (int c = 0; c < cases.length; c++) {
                long rngSeed = cases[c][0];
                int blockId = (int) cases[c][1];
                int veinSize = (int) cases[c][2];
                int x = (int) cases[c][3];
                int y = (int) cases[c][4];
                int z = (int) cases[c][5];

                // Make sure every chunk the box touches exists before writing.
                for (int cx = (x - ORE_BOX) >> 4; cx <= (x + ORE_BOX) >> 4; cx++) {
                    for (int cz = (z - ORE_BOX) >> 4; cz <= (z + ORE_BOX) >> 4; cz++) {
                        getChunk.invoke(world, cx, cz);
                    }
                }
                // Fill the box with stone. Outside it, whatever the terrain has
                // -- the vein cannot reach that far.
                for (int bx = x - ORE_BOX; bx <= x + ORE_BOX; bx++) {
                    for (int by = Math.max(0, y - ORE_BOX); by <= Math.min(127, y + ORE_BOX); by++) {
                        for (int bz = z - ORE_BOX; bz <= z + ORE_BOX; bz++) {
                            setBlock.invoke(world, bx, by, bz, 1);
                        }
                    }
                }

                Object gen;
                java.lang.reflect.Method run;
                if (blockId == CLAY_MARKER) {
                    // Clay: the box is sand rather than stone, and the origin
                    // must be water by material or the guard refuses outright.
                    for (int bx = x - ORE_BOX; bx <= x + ORE_BOX; bx++) {
                        for (int by = Math.max(0, y - ORE_BOX);
                             by <= Math.min(127, y + ORE_BOX); by++) {
                            for (int bz = z - ORE_BOX; bz <= z + ORE_BOX; bz++) {
                                setBlock.invoke(world, bx, by, bz, 12);   // sand
                            }
                        }
                    }
                    setBlock.invoke(world, x, y, z, 9);                   // still water
                    Class<?> clay = loader.loadClass("gv");
                    gen = clay.getConstructor(int.class).newInstance(veinSize);
                    run = clay.getMethod("a", worldClass, Random.class, int.class, int.class,
                                         int.class);
                } else {
                    gen = oreCtor.newInstance(blockId, veinSize);
                    run = generate;
                }
                run.invoke(gen, world, new Random(rngSeed), x, y, z);

                StringBuilder placements = new StringBuilder();
                int count = 0;
                for (int bx = x - ORE_BOX; bx <= x + ORE_BOX; bx++) {
                    for (int by = Math.max(0, y - ORE_BOX); by <= Math.min(127, y + ORE_BOX); by++) {
                        for (int bz = z - ORE_BOX; bz <= z + ORE_BOX; bz++) {
                            int id = (Integer) getBlock.invoke(world, bx, by, bz);
                            if (id != (blockId == CLAY_MARKER ? 12 : 1)) {
                                placements.append("    {").append(bx).append(", ").append(by)
                                          .append(", ").append(bz).append(", ").append(id)
                                          .append("},\n");
                                count++;
                            }
                        }
                    }
                }

                p("inline constexpr OrePlacement kOrePlacements" + c + "[" + Math.max(count, 1)
                  + "] = {");
                if (count == 0) {
                    p("    {0, 0, 0, 0},   // none placed");
                } else {
                    System.out.print(placements);
                }
                p("};");
                p("");

                table.append("    {").append(rngSeed).append("LL, ").append(blockId).append(", ")
                     .append(veinSize).append(", ").append(x).append(", ").append(y).append(", ")
                     .append(z).append(", ").append(count).append(", kOrePlacements").append(c)
                     .append("},\n");
            }
            deleteTree(dir);

            p("inline constexpr int kOreCaseCount = " + cases.length + ";");
            p("");
            p("inline constexpr OreCase kOreCases[kOreCaseCount] = {");
            System.out.print(table);
            p("};");
            p("");
            p("}  // namespace mc::test");
        } catch (Exception e) {
            System.err.println("genref --ore failed: " + e);
            e.printStackTrace();
            System.exit(1);
        }
    }

    // ---------------------------------------------------------------------
    // Whole populated chunks, from a real running World
    // ---------------------------------------------------------------------
    //
    // The --terrain oracle drives ChunkProviderGenerate directly over an
    // uninitialised World, which is enough for terrain, the surface pass and
    // caves because none of them touch the World. **Population does.** Its nine
    // generators read and write blocks through the World, across chunk
    // boundaries, and a chunk is only populated once its neighbours exist.
    //
    // So this mode runs the real thing: `new cn(dir, name, seed)` constructs an
    // actual World with an actual save directory, and asking for a chunk
    // generates, populates and lights it. It works headlessly -- World is game
    // logic, not rendering -- which was the open question.
    //
    // Two things have to be pinned or the output is not reproducible:
    //
    //   * **SnowCovered is forced.** It is rolled from `new Random()`, entropy
    //     and not the seed, so a fresh world flips a coin. Setting the public
    //     field straight after construction makes it deterministic.
    //   * **The chunks are far from spawn.** The constructor looks for a spawn
    //     point, which generates chunks around the origin; targeting those would
    //     read back whatever the constructor happened to cache.
    //
    // Verified deterministic by running it twice and comparing.

    private static void emitWorld(String jarPath, String scratchDir) {
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> worldClass = loader.loadClass(WORLD);
            Class<?> chunkClass = loader.loadClass("ga");

            java.lang.reflect.Constructor<?> ctor =
                worldClass.getConstructor(java.io.File.class, String.class, long.class);
            java.lang.reflect.Field snowField = worldClass.getField(SNOW_FIELD);
            java.lang.reflect.Method getChunk =
                worldClass.getMethod("b", int.class, int.class);

            java.lang.reflect.Field blocksField = chunkClass.getField("b");
            java.lang.reflect.Field heightField = chunkClass.getField("h");

            // {seed, chunkX, chunkZ, snowCovered}
            long[][] cases = {
                {1234567890L, 200, -150, 0},
                {1234567890L, 200, -150, 1},
                {0L, -321, 404, 0},
                {-8675309L, 1875, -2048, 0},
            };

            p("// Generated by tools/genref.java --jar <client.jar> --world <scratch-dir>.");
            p("// Do not edit by hand.");
            p("//");
            p("// Populated and lit chunk columns from a **real running a1.1.2 World** -- terrain,");
            p("// surface, caves, population and lighting, the whole pipeline. SnowCovered is");
            p("// forced rather than rolled, and the chunks are far from spawn so the World");
            p("// constructor has not already cached them. See tools/genref.java.");
            p("//");
            p("// Regenerate with:");
            p("//   java tools/genref.java --jar <client.jar> --world /tmp/genref-scratch");
            p("//     redirected to tests/world_vectors.hpp");
            p("");
            p("#pragma once");
            p("");
            p("#include \"core/util/types.hpp\"");
            p("");
            p("namespace mc::test {");
            p("");
            p("struct WorldCase {");
            p("    i64 seed;");
            p("    i32 chunkX;");
            p("    i32 chunkZ;");
            p("    bool snowCovered;");
            p("    int blockRuns;");
            p("    const u32* blocks;      // (count << 8) | blockId");
            p("    const u8* heightMap;    // 256 entries");
            p("};");
            p("");

            StringBuilder table = new StringBuilder();
            for (int c = 0; c < cases.length; c++) {
                long seed = cases[c][0];
                int cx = (int) cases[c][1];
                int cz = (int) cases[c][2];
                boolean snowy = cases[c][3] != 0;

                java.io.File dir = new java.io.File(scratchDir, "w" + c);
                deleteTree(dir);
                dir.mkdirs();

                Object world = ctor.newInstance(dir, "genref", seed);
                snowField.setBoolean(world, snowy);

                // A 5x5 so the target certainly has all eight neighbours and is
                // therefore populated, not merely generated.
                for (int x = cx - 2; x <= cx + 2; x++) {
                    for (int z = cz - 2; z <= cz + 2; z++) {
                        getChunk.invoke(world, x, z);
                    }
                }

                Object chunk = getChunk.invoke(world, cx, cz);
                byte[] blocks = (byte[]) blocksField.get(chunk);
                byte[] heights = (byte[]) heightField.get(chunk);

                int runs = emitRuns("kWorldBlocks" + c, blocks);

                p("inline constexpr u8 kWorldHeight" + c + "[256] = {");
                for (int row = 0; row < 256; row += 16) {
                    StringBuilder sb = new StringBuilder();
                    for (int i = row; i < row + 16; i++) {
                        sb.append(i == row ? "" : ", ").append(heights[i] & 0xFF);
                    }
                    p("    " + sb + ",");
                }
                p("};");
                p("");

                table.append("    {")
                     .append(lit(seed)).append(", ")
                     .append(cx).append(", ").append(cz).append(", ")
                     .append(snowy).append(", ")
                     .append(runs).append(", kWorldBlocks").append(c).append(", ")
                     .append("kWorldHeight").append(c)
                     .append("},\n");

                deleteTree(dir);
            }

            p("inline constexpr int kWorldCaseCount = " + cases.length + ";");
            p("");
            p("inline constexpr WorldCase kWorldCases[kWorldCaseCount] = {");
            System.out.print(table);
            p("};");
            p("");
            p("}  // namespace mc::test");
        } catch (Exception e) {
            System.err.println("genref --world failed: " + e);
            e.printStackTrace();
            System.exit(1);
        }
    }


    // ---------------------------------------------------------------------
    // The whole chunk pipeline -- `ft` (ChunkProviderLoadOrGenerate) driving
    // `nw` (ChunkProviderGenerate) and the light queue
    // ---------------------------------------------------------------------
    //
    // Every stage below this already has an oracle: terrain, caves, each of the
    // nine population generators, the driver that orders them, and converged
    // lighting. What none of them can check is the **driver above** -- when a
    // chunk is populated, and therefore in what order two neighbouring chunks
    // write into the column they share.
    //
    // `ft.b(int,int)` populates a chunk once a 2x2 quadrant containing it is
    // resident, so the order is a consequence of the sequence of getChunk calls
    // and nothing else. That makes it reproducible only if the sequence is
    // pinned, which is what this does: it asks the World for chunks in exactly
    // the order `ChunkGenerator::provide` sweeps them, row-major over the 6x6
    // its closure needs, one provided column at a time.
    //
    // So the fixture is not "the world for seed S". It is "the world for seed S
    // reached by this sequence of loads", which is the only thing an Alpha seed
    // ever determines. See the note at the top of chunk_generator.hpp.
    private static void emitGenerate(String jarPath, String scratchDir) {
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> worldClass = loader.loadClass(WORLD);
            Class<?> chunkClass = loader.loadClass("ga");
            Class<?> nibbleClass = loader.loadClass("mu");

            java.lang.reflect.Constructor<?> worldCtor =
                worldClass.getConstructor(java.io.File.class, String.class, long.class);
            java.lang.reflect.Field snowField = worldClass.getField(SNOW_FIELD);
            java.lang.reflect.Method getChunk = worldClass.getMethod("b", int.class, int.class);
            java.lang.reflect.Method updateLights = worldClass.getMethod("e");

            java.lang.reflect.Field chunkBlocks = chunkClass.getField("b");      // byte[]
            java.lang.reflect.Field chunkHeight = chunkClass.getField("h");      // byte[]
            java.lang.reflect.Field chunkSky = chunkClass.getField("f");         // mu
            java.lang.reflect.Field chunkBlockLight = chunkClass.getField("g");  // mu
            java.lang.reflect.Field chunkPopulated = chunkClass.getField("n");   // isTerrainPopulated
            java.lang.reflect.Field nibbleData = nibbleClass.getDeclaredField("a");
            nibbleData.setAccessible(true);

            // {seed, chunkX, chunkZ, snowCovered}. The 2x2 whose origin this is
            // gets asked for, in row-major order, one provide() at a time.
            //
            // **Far from the origin, in every case.** The World constructor
            // runs a spawn search that generates chunks around (0, 0) before
            // this code sees the world; a case near there finds its neighbours
            // already loaded and populated, and the sequence being pinned is no
            // longer the sequence that ran.
            long[][] cases = {
                {1234567890L, 37, -14, 0},
                {-8675309L, 1875, -2048, 0},
                {42L, -100, 200, 0},
                {1234567890L, 37, -14, 1},
            };

            java.io.File dir = new java.io.File(scratchDir, "generate");
            deleteTree(dir);
            dir.mkdirs();

            p("// Generated by tools/genref.java --jar <client.jar> --generate <scratch-dir>.");
            p("// Do not edit by hand.");
            p("//");
            p("// **The whole chunk pipeline against a real a1.1.2 World**: `ft` deciding what");
            p("// to generate and when to populate it, `nw` generating it, and the light queue");
            p("// drained to its fixed point. Four columns per case, asked for in the order");
            p("// ChunkGenerator::provide sweeps them, so the population order the fixture");
            p("// carries is the one our driver produces.");
            p("//");
            p("// Blocks and both light planes are run-length encoded as (count << 8) | value,");
            p("// indexed x << 11 | z << 7 | y. Height maps are 256 raw bytes, z << 4 | x.");
            p("//");
            p("// Regenerate with:");
            p("//   java tools/genref.java --jar <client.jar> --generate /tmp/genref-scratch");
            p("//     redirected to tests/generate_vectors.hpp");
            p("");
            p("#pragma once");
            p("");
            p("#include \"core/util/types.hpp\"");
            p("");
            p("namespace mc::test {");
            p("");
            p("struct GenerateCase {");
            p("    i64 seed;");
            p("    i32 chunkX;");
            p("    i32 chunkZ;");
            p("    bool snowCovered;");
            p("    // The four provided columns, x slowest: entry (ix, iz) is chunk");
            p("    // (chunkX + ix, chunkZ + iz), and they are provided in that order.");
            p("    int blockRuns[4];");
            p("    const u32* blocks[4];");
            p("    int skyRuns[4];");
            p("    const u32* sky[4];");
            p("    int blockLightRuns[4];");
            p("    const u32* blockLight[4];");
            p("    const u8* heightMap[4];");
            p("};");
            p("");

            StringBuilder table = new StringBuilder();
            for (int c = 0; c < cases.length; c++) {
                final long seed = cases[c][0];
                final int cx0 = (int) cases[c][1];
                final int cz0 = (int) cases[c][2];
                final boolean snowy = cases[c][3] != 0;

                java.io.File caseDir = new java.io.File(dir, "case" + c);
                caseDir.mkdirs();
                Object world = worldCtor.newInstance(caseDir, "genref", seed);
                snowField.setBoolean(world, snowy);

                // **The sequence, and it is the fixture.** One provide() is a
                // row-major sweep of the 6x6 its closure needs; four provides
                // in row-major order over the 2x2.
                for (int ix = 0; ix < 2; ix++) {
                    for (int iz = 0; iz < 2; iz++) {
                        final int cx = cx0 + ix;
                        final int cz = cz0 + iz;
                        for (int x = cx - 3; x <= cx + 2; x++) {
                            for (int z = cz - 3; z <= cz + 2; z++) {
                                getChunk.invoke(world, x, z);
                            }
                        }
                    }
                }

                // Run the queue dry, exactly as --light does. Reaching the
                // bound would make this a snapshot part way through rather than
                // the fixed point our engine solves for.
                int drains = 0;
                while (((Boolean) updateLights.invoke(world)).booleanValue()) {
                    if (++drains > 200000) {
                        throw new IllegalStateException("light queue did not converge");
                    }
                }

                StringBuilder blockRuns = new StringBuilder();
                StringBuilder skyRuns = new StringBuilder();
                StringBuilder lightRuns = new StringBuilder();
                for (int ix = 0; ix < 2; ix++) {
                    for (int iz = 0; iz < 2; iz++) {
                        final int k = ix * 2 + iz;
                        Object chunk = getChunk.invoke(world, cx0 + ix, cz0 + iz);

                        // Every column the fixture carries must be populated,
                        // or the sweep above did not do what it claims and the
                        // comparison would be against half a chunk.
                        if (!chunkPopulated.getBoolean(chunk)) {
                            throw new IllegalStateException(
                                "chunk " + (cx0 + ix) + "," + (cz0 + iz) + " never populated");
                        }

                        byte[] blocks = (byte[]) chunkBlocks.get(chunk);
                        blockRuns.append(emitRuns("kGenBlocks" + c + "_" + k, blocks)).append(", ");

                        byte[] skyPacked = (byte[]) nibbleData.get(chunkSky.get(chunk));
                        skyRuns.append(emitNibbleRuns("kGenSky" + c + "_" + k, skyPacked))
                               .append(", ");

                        byte[] litPacked = (byte[]) nibbleData.get(chunkBlockLight.get(chunk));
                        lightRuns.append(emitNibbleRuns("kGenLight" + c + "_" + k, litPacked))
                                 .append(", ");

                        byte[] heights = (byte[]) chunkHeight.get(chunk);
                        p("inline constexpr u8 kGenHeight" + c + "_" + k + "[256] = {");
                        StringBuilder hs = new StringBuilder("   ");
                        for (int i = 0; i < 256; i++) {
                            hs.append(' ').append(heights[i] & 255).append(',');
                            if (i % 16 == 15) { p(hs.toString()); hs = new StringBuilder("   "); }
                        }
                        p("};");
                        p("");
                    }
                }

                table.append("    {").append(lit(seed)).append(", ").append(cx0).append(", ")
                     .append(cz0).append(", ").append(snowy).append(",\n");
                table.append("     {").append(trimComma(blockRuns)).append("},\n     {");
                for (int k = 0; k < 4; k++) {
                    table.append("kGenBlocks").append(c).append("_").append(k)
                         .append(k == 3 ? "" : ", ");
                }
                table.append("},\n");
                table.append("     {").append(trimComma(skyRuns)).append("},\n     {");
                for (int k = 0; k < 4; k++) {
                    table.append("kGenSky").append(c).append("_").append(k)
                         .append(k == 3 ? "" : ", ");
                }
                table.append("},\n");
                table.append("     {").append(trimComma(lightRuns)).append("},\n     {");
                for (int k = 0; k < 4; k++) {
                    table.append("kGenLight").append(c).append("_").append(k)
                         .append(k == 3 ? "" : ", ");
                }
                table.append("},\n     {");
                for (int k = 0; k < 4; k++) {
                    table.append("kGenHeight").append(c).append("_").append(k)
                         .append(k == 3 ? "" : ", ");
                }
                table.append("}},\n");
            }

            p("inline constexpr int kGenerateCaseCount = " + cases.length + ";");
            p("");
            p("inline constexpr GenerateCase kGenerateCases[kGenerateCaseCount] = {");
            System.out.print(table);
            p("};");
            p("");
            p("}  // namespace mc::test");
        } catch (Exception e) {
            System.err.println("genref --generate failed: " + e);
            e.printStackTrace();
            System.exit(1);
        }
    }

    private static String trimComma(StringBuilder sb) {
        return sb.substring(0, sb.length() - 2);
    }

    private static void deleteTree(java.io.File f) {
        java.io.File[] kids = f.listFiles();
        if (kids != null) {
            for (java.io.File kid : kids) {
                deleteTree(kid);
            }
        }
        f.delete();
    }

    // ---------------------------------------------------------------------
    // MathHelper's sine table
    // ---------------------------------------------------------------------
    //
    // The cave carver never calls Math.sin. It goes through a 65,536-entry
    // float table that MathHelper builds once from `(float)Math.sin(i*2pi/65536)`,
    // so reproducing the *table* bit-for-bit is enough and the transcendental
    // disappears from the runtime path entirely.
    //
    // 65,536 floats is far too much to check in as a fixture, and would be a
    // 256 KB array in the binary besides. Instead this emits a hash over the
    // whole table plus a scattering of individual entries. The hash proves all
    // 65,536 agree; the samples say *where* if they do not. Our side builds the
    // table at startup with its own sin and hashes it the same way -- if the
    // two ever diverge, that is a real finding about libm and not a rounding
    // detail to paper over.
    //
    // Written out as its own mode rather than folded into --jar because the
    // table is defined by an expression, not by the jar: any implementation of
    // `(float)Math.sin(i * PI * 2 / 65536)` must produce it.

    private static void emitSinTable() {
        float[] table = new float[65536];
        for (int i = 0; i < 65536; i++) {
            // Exactly the original's expression, in the original's order.
            // Reassociating it -- (i * 2 * PI) / 65536, say -- changes the last
            // bits of some entries.
            table[i] = (float) Math.sin((double) i * 3.141592653589793D * 2.0D / 65536.0D);
        }

        // FNV-1a over the raw float bits, little-endian, so the hash depends on
        // every bit of every entry.
        long hash = 0xCBF29CE484222325L;
        for (float value : table) {
            int bits = Float.floatToRawIntBits(value);
            for (int b = 0; b < 4; b++) {
                hash ^= (bits >>> (b * 8)) & 0xFF;
                hash *= 0x100000001B3L;
            }
        }

        p("// Generated by tools/genref.java --sintable. Do not edit by hand.");
        p("//");
        p("// MathHelper's 65,536-entry sine table, as a hash over every bit plus a");
        p("// scattering of individual entries for diagnosis. See tools/genref.java.");
        p("//");
        p("// Regenerate with:  java tools/genref.java --sintable > tests/sin_table_vectors.hpp");
        p("");
        p("#pragma once");
        p("");
        p("#include \"core/util/types.hpp\"");
        p("");
        p("namespace mc::test {");
        p("");
        p("inline constexpr u64 kSinTableHash = " + String.format("0x%016XULL", hash) + ";");
        p("");
        p("struct SinSample {");
        p("    int index;");
        p("    u32 bits;");
        p("};");
        p("");

        java.util.ArrayList<Integer> indices = new java.util.ArrayList<>();
        // The quadrant boundaries, where sin is exactly 0 or +-1 and a wrong
        // table is least likely to look wrong.
        for (int q : new int[]{0, 1, 16383, 16384, 16385, 32767, 32768, 32769, 49151, 49152,
                               49153, 65534, 65535}) {
            indices.add(q);
        }
        // Plus a deterministic spread across the range.
        for (int i = 0; i < 51; i++) {
            indices.add((i * 1279 + 613) & 65535);
        }

        p("inline constexpr int kSinSampleCount = " + indices.size() + ";");
        p("");
        p("inline constexpr SinSample kSinSamples[kSinSampleCount] = {");
        for (int index : indices) {
            p("    {" + index + ", " + hex32(Float.floatToRawIntBits(table[index])) + "},");
        }
        p("};");
        p("");
        p("}  // namespace mc::test");
    }

    // ---------------------------------------------------------------------
    // Whole chunks, from the original's own ChunkProviderGenerate
    // ---------------------------------------------------------------------
    //
    // `nw` takes a World (`cn`) in its constructor, which looked like a wall:
    // every World constructor takes a file and does I/O. It is not one. The
    // provider's constructor never *calls* the World -- it only stores the
    // reference -- and generateTerrain reads exactly one field from it, the
    // public boolean `d`, which is level.dat's `SnowCovered`. So an
    // uninitialised World is enough to drive the whole thing, and Unsafe will
    // hand us one without running any constructor.
    //
    // Emitted run-length encoded. A generated column is 32,768 bytes of mostly
    // long runs -- air above, stone below -- so RLE turns a 100 KB hex dump per
    // case into a few hundred pairs, and leaves the fixture legible enough that
    // a human can see the sea level in it.

    private static final String PROVIDER = "nw";
    private static final String WORLD = "cn";
    private static final String SNOW_FIELD = "d";

    private static void emitTerrain(String jarPath) {
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> world = loader.loadClass(WORLD);
            Class<?> provider = loader.loadClass(PROVIDER);

            java.lang.reflect.Constructor<?> ctor = provider.getConstructor(world, long.class);
            java.lang.reflect.Method generateTerrain =
                provider.getMethod("a", int.class, int.class, byte[].class);
            java.lang.reflect.Method replaceBlocks =
                provider.getMethod("b", int.class, int.class, byte[].class);
            java.lang.reflect.Field snow = world.getField(SNOW_FIELD);
            // `cn.u` is RandomSeed. Only the cave pass reads it.
            java.lang.reflect.Field worldSeedField = world.getField("u");

            // {seed, chunkX, chunkZ, snowCovered}
            long[][] cases = {
                {0, 0, 0, 0},
                {0, 1, -1, 0},
                {1234567890L, 0, 0, 0},
                {1234567890L, 37, -14, 0},
                // The snow branch, which is the only thing the World is read
                // for: ice instead of water at sea level - 1.
                {1234567890L, 0, 0, 1},
                // Far from the origin, where the noise coordinates are large
                // and any float/double confusion shows up first.
                {-8675309L, 1875, -2048, 0},
                // Cave-heavy. Caves are sparse -- most chunk cells roll
                // `nextInt(15) != 0` and generate nothing -- so cases chosen
                // only for terrain leave the carver barely exercised. These
                // were picked by counting carved blocks.
                {-8675309L, 1876, -2048, 0},
                {-8675309L, 1874, -2047, 0},
                {42L, -100, 200, 0},
                {7L, 12, 12, 0},
                // **The Far Lands.** At chunk 784,426 the noise coordinate
                // `chunkX * 4 * 684.412` reaches 2,147,483,647 and the `d2i` in
                // the Perlin lattice stops being a conversion and starts being
                // a clamp, so the fractional part it subtracts is garbage and
                // the terrain turns into a wall. 784,426 is the chunk the wall
                // starts in -- some of its five lattice columns overflow and
                // some do not -- and 784,427 is past it entirely. The negative
                // side is a separate case because the floor's `i - 1` underflows
                // Integer.MIN_VALUE there, which Java wraps and C++ does not.
                {1234567890L, 784426, 0, 0},
                {1234567890L, 784427, 0, 0},
                {1234567890L, 0, 784427, 0},
                {1234567890L, -784427, 0, 0},
            };

            p("// Generated by tools/genref.java --jar <client.jar> --terrain. Do not edit by hand.");
            p("//");
            p("// Whole chunk columns from the a1.1.2 client jar's own ChunkProviderGenerate,");
            p("// driven by reflection over an uninitialised World. Two stages per case: the");
            p("// raw terrain, and the same column after the surface pass. Run-length encoded");
            p("// as (count, blockId) pairs -- see tools/genref.java.");
            p("//");
            p("// Regenerate with:");
            p("//   java tools/genref.java --jar <client.jar> --terrain > tests/terrain_vectors.hpp");
            p("");
            p("#pragma once");
            p("");
            p("#include \"core/util/types.hpp\"");
            p("");
            p("namespace mc::test {");
            p("");
            p("struct TerrainCase {");
            p("    i64 seed;");
            p("    i32 chunkX;");
            p("    i32 chunkZ;");
            p("    bool snowCovered;");
            p("    int terrainRuns;");
            p("    const u32* terrain;      // (count << 8) | blockId");
            p("    int surfaceRuns;");
            p("    const u32* surface;");
            p("    int caveRuns;");
            p("    const u32* caved;");
            p("};");
            p("");

            StringBuilder table = new StringBuilder();
            for (int c = 0; c < cases.length; c++) {
                long seed = cases[c][0];
                int cx = (int) cases[c][1];
                int cz = (int) cases[c][2];
                boolean snowCovered = cases[c][3] != 0;

                Object worldInstance = allocateUninitialised(world);
                snow.setBoolean(worldInstance, snowCovered);

                Object gen = ctor.newInstance(worldInstance, seed);

                byte[] terrain = new byte[32768];
                generateTerrain.invoke(gen, cx, cz, terrain);
                int terrainRuns = emitRuns("kTerrain" + c, terrain);

                // The surface pass runs on the same array, in place, and is
                // seeded per chunk by provideChunk rather than by itself -- so
                // the seed has to be set here, exactly as `nw.b(int,int)` does
                // before calling either of these.
                byte[] surface = new byte[32768];
                Object gen2 = ctor.newInstance(worldInstance, seed);
                java.lang.reflect.Field randomField = fieldOfType(provider, Random.class);
                randomField.setAccessible(true);
                Random providerRandom = (Random) randomField.get(gen2);
                providerRandom.setSeed((long) cx * 341873128712L + (long) cz * 132897987541L);
                generateTerrain.invoke(gen2, cx, cz, surface);
                replaceBlocks.invoke(gen2, cx, cz, surface);
                int surfaceRuns = emitRuns("kSurface" + c, surface);

                // Third stage: the same column with caves carved. The cave
                // generator reads the *world* seed off the World, so unlike
                // everything above it the uninitialised instance needs a field
                // filled in.
                byte[] caved = new byte[32768];
                Object gen3 = ctor.newInstance(worldInstance, seed);
                Random r3 = (Random) randomField.get(gen3);
                r3.setSeed((long) cx * 341873128712L + (long) cz * 132897987541L);
                generateTerrain.invoke(gen3, cx, cz, caved);
                replaceBlocks.invoke(gen3, cx, cz, caved);
                worldSeedField.setLong(worldInstance, seed);
                Class<?> caveBase = loader.loadClass("cy");
                java.lang.reflect.Field caveField = fieldOfType(provider, caveBase);
                caveField.setAccessible(true);
                Object caves = caveField.get(gen3);
                java.lang.reflect.Method caveGen =
                    caveBase.getMethod("a", provider, world, int.class, int.class, byte[].class);
                caveGen.invoke(caves, gen3, worldInstance, cx, cz, caved);
                int caveRuns = emitRuns("kCaved" + c, caved);

                table.append("    {")
                     .append(lit(seed)).append(", ")
                     .append(cx).append(", ").append(cz).append(", ")
                     .append(snowCovered).append(", ")
                     .append(terrainRuns).append(", kTerrain").append(c).append(", ")
                     .append(surfaceRuns).append(", kSurface").append(c).append(", ")
                     .append(caveRuns).append(", kCaved").append(c)
                     .append("},\n");
            }

            p("inline constexpr int kTerrainCaseCount = " + cases.length + ";");
            p("");
            p("inline constexpr TerrainCase kTerrainCases[kTerrainCaseCount] = {");
            System.out.print(table);
            p("};");
            p("");
            p("}  // namespace mc::test");
        } catch (Exception e) {
            System.err.println("genref --terrain failed: " + e);
            e.printStackTrace();
            System.exit(1);
        }
    }

    // (count << 8) | blockId, so one u32 per run and no struct packing to argue
    // about. A run never exceeds 32768, which fits the 24 bits available.
    private static int emitIntRuns(String name, int[] values) {
        java.util.ArrayList<Long> runs = new java.util.ArrayList<>();
        int i = 0;
        while (i < values.length) {
            int value = values[i];
            int count = 0;
            while (i < values.length && values[i] == value) {
                count++;
                i++;
            }
            runs.add(((long) count << 8) | value);
        }

        p("inline constexpr u32 " + name + "[" + runs.size() + "] = {");
        for (int row = 0; row < runs.size(); row += 8) {
            StringBuilder sb = new StringBuilder();
            for (int k = row; k < row + 8 && k < runs.size(); k++) {
                sb.append(k == row ? "" : ", ").append("0x")
                  .append(String.format("%06X", runs.get(k))).append('U');
            }
            p("    " + sb + ",");
        }
        p("};");
        p("");
        return runs.size();
    }

    private static int emitRuns(String name, byte[] blocks) {
        java.util.ArrayList<Long> runs = new java.util.ArrayList<>();
        int i = 0;
        while (i < blocks.length) {
            int value = blocks[i] & 0xFF;
            int count = 0;
            while (i < blocks.length && (blocks[i] & 0xFF) == value) {
                count++;
                i++;
            }
            runs.add(((long) count << 8) | value);
        }

        p("inline constexpr u32 " + name + "[" + runs.size() + "] = {");
        for (int row = 0; row < runs.size(); row += 8) {
            StringBuilder sb = new StringBuilder();
            for (int k = row; k < row + 8 && k < runs.size(); k++) {
                sb.append(k == row ? "" : ", ").append("0x").append(String.format("%06X", runs.get(k))).append('U');
            }
            p("    " + sb + ",");
        }
        p("};");
        p("");
        return runs.size();
    }

    // sun.misc.Unsafe.allocateInstance: zeroed fields, no constructor. The only
    // way to get a World without a save directory, and the reason the whole
    // terrain oracle is possible.
    private static Object allocateUninitialised(Class<?> type) throws Exception {
        Class<?> unsafeClass = Class.forName("sun.misc.Unsafe");
        java.lang.reflect.Field theUnsafe = unsafeClass.getDeclaredField("theUnsafe");
        theUnsafe.setAccessible(true);
        Object unsafe = theUnsafe.get(null);
        java.lang.reflect.Method allocate =
            unsafeClass.getMethod("allocateInstance", Class.class);
        return allocate.invoke(unsafe, type);
    }

    private static java.lang.reflect.Field fieldOfType(Class<?> owner, Class<?> type) {
        for (java.lang.reflect.Field f : owner.getDeclaredFields()) {
            if (f.getType() == type) {
                return f;
            }
        }
        throw new IllegalStateException("no field of type " + type + " on " + owner);
    }

    // Sorted by name, deliberately. getDeclaredFields() has no specified order,
    // and this happens to come back in declaration order on HotSpot -- which is
    // exactly the kind of thing that works until the day it does not. The three
    // offsets are named a, b, c and the constructor assigns them in that order,
    // so sorting by name reproduces the assignment order from the class file
    // rather than from the JVM's mood.
    private static java.lang.reflect.Field[] doubleFields(Class<?> owner) {
        java.util.ArrayList<java.lang.reflect.Field> out = new java.util.ArrayList<>();
        for (java.lang.reflect.Field f : owner.getDeclaredFields()) {
            if (f.getType() == double.class) {
                out.add(f);
            }
        }
        out.sort(java.util.Comparator.comparing(java.lang.reflect.Field::getName));
        return out.toArray(new java.lang.reflect.Field[0]);
    }

    private static java.lang.reflect.Method methodByDescriptor(Class<?> owner, String descriptor) {
        for (java.lang.reflect.Method m : owner.getDeclaredMethods()) {
            if (descriptorOf(m).equals(descriptor)) {
                return m;
            }
        }
        throw new IllegalStateException("no method " + descriptor + " on " + owner);
    }

    private static String descriptorOf(java.lang.reflect.Method m) {
        StringBuilder sb = new StringBuilder("(");
        for (Class<?> p : m.getParameterTypes()) {
            sb.append(typeDescriptor(p));
        }
        return sb.append(')').append(typeDescriptor(m.getReturnType())).toString();
    }

    private static String typeDescriptor(Class<?> c) {
        if (c == void.class) return "V";
        if (c == int.class) return "I";
        if (c == long.class) return "J";
        if (c == float.class) return "F";
        if (c == double.class) return "D";
        if (c == boolean.class) return "Z";
        if (c == byte.class) return "B";
        if (c == char.class) return "C";
        if (c == short.class) return "S";
        if (c.isArray()) return "[" + typeDescriptor(c.getComponentType());
        return "L" + c.getName().replace('.', '/') + ";";
    }

    // StrictMath.log, which is contractually fdlibm and is *not* the same
    // function as the host's libm log. nextGaussian is the one Random method
    // that consumes it, and the difference showed up immediately: one ulp.
    //
    // The inputs are the values nextGaussian actually feeds to log -- s in
    // (0, 1) -- plus the boundaries and a spread of ordinary magnitudes, so a
    // transcription that is right in the middle of the range and wrong near
    // the ends cannot pass.
    private static void emitStrictMath() {
        double[] xs = buildLogInputs();

        p("// Generated by tools/genref.java --strictmath. Do not edit by hand.");
        p("//");
        p("// StrictMath.log from a real JVM. StrictMath is contractually fdlibm, and the");
        p("// host's libm is not -- glibc's log is correctly rounded where fdlibm's is not,");
        p("// so they disagree in the last bit. That one ulp reaches Random.nextGaussian,");
        p("// which is why mc::strictmath::log exists at all.");
        p("//");
        p("// Regenerate with:  java tools/genref.java --strictmath > tests/strict_math_vectors.hpp");
        p("");
        p("#pragma once");
        p("");
        p("#include \"core/util/types.hpp\"");
        p("");
        p("namespace mc::test {");
        p("");
        p("struct LogVector {");
        p("    u64 xBits;");
        p("    u64 logBits;");
        p("};");
        p("");
        p("inline constexpr int kLogVectorCount = " + xs.length + ";");
        p("");
        p("inline constexpr LogVector kLogVectors[kLogVectorCount] = {");
        for (double x : xs) {
            p("    {" + hex64(Double.doubleToRawLongBits(x)) + ", "
              + hex64(Double.doubleToRawLongBits(StrictMath.log(x))) + "},");
        }
        p("};");
        p("");
        p("}  // namespace mc::test");
    }

    private static double[] buildLogInputs() {
        java.util.ArrayList<Double> xs = new java.util.ArrayList<>();

        // Exact powers of two and their neighbours: fdlibm branches on the
        // exponent and on how close the mantissa is to 1, so these are where a
        // transcription goes wrong.
        for (int e = -60; e <= 60; e += 4) {
            double p2 = StrictMath.pow(2.0, e);
            xs.add(p2);
            xs.add(Math.nextUp(p2));
            xs.add(Math.nextDown(p2));
        }

        // The near-1 path, which fdlibm handles with a separate polynomial.
        xs.add(1.0);
        xs.add(Math.nextUp(1.0));
        xs.add(Math.nextDown(1.0));
        for (int i = 1; i <= 20; i++) {
            xs.add(1.0 + i * 1e-9);
            xs.add(1.0 - i * 1e-9);
            xs.add(1.0 + i * 1e-3);
            xs.add(1.0 - i * 1e-3);
        }

        // Exactly what nextGaussian sees: s = v1*v1 + v2*v2 in (0, 1), drawn
        // from the same generator that will be asking.
        Random r = new Random(12345);
        int taken = 0;
        while (taken < 200) {
            double v1 = 2 * r.nextDouble() - 1;
            double v2 = 2 * r.nextDouble() - 1;
            double s = v1 * v1 + v2 * v2;
            if (s < 1 && s != 0) {
                xs.add(s);
                taken++;
            }
        }

        // Denormals and the extremes of the range.
        xs.add(Double.MIN_VALUE);
        xs.add(Double.MIN_NORMAL);
        xs.add(Math.nextDown(Double.MIN_NORMAL));
        xs.add(Double.MAX_VALUE);

        double[] out = new double[xs.size()];
        for (int i = 0; i < out.length; i++) {
            out[i] = xs.get(i);
        }
        return out;
    }

    private static void emitRandom() {
        p("// Generated by tools/genref.java --random. Do not edit by hand.");
        p("//");
        p("// Reference values taken from a real JVM's java.util.Random, which is the");
        p("// definition our JavaRandom has to match. Floats and doubles are stored as bit");
        p("// patterns so the comparison is exact rather than approximate.");
        p("//");
        p("// Regenerate with:  java tools/genref.java --random > tests/java_random_vectors.hpp");
        p("");
        p("#pragma once");
        p("");
        p("#include \"core/util/types.hpp\"");
        p("");
        p("namespace mc::test {");
        p("");
        p("inline constexpr int kRandomDraws = " + N + ";");
        p("inline constexpr int kRandomBoundCount = " + BOUNDS.length + ";");
        p("");
        p("inline constexpr i32 kRandomBounds[kRandomBoundCount] = {" + join(BOUNDS) + "};");
        p("");
        p("struct RandomVector {");
        p("    i64 seed;");
        p("    u64 rawSeed;");
        p("    i32 nextInt[kRandomDraws];");
        p("    i32 nextIntBound[kRandomBoundCount][kRandomDraws];");
        p("    i64 nextLong[kRandomDraws];");
        p("    u32 nextFloatBits[kRandomDraws];");
        p("    u64 nextDoubleBits[kRandomDraws];");
        p("    u8 nextBoolean[kRandomDraws];");
        p("    u64 nextGaussianBits[kRandomDraws];");
        p("};");
        p("");
        p("inline constexpr int kRandomVectorCount = " + SEEDS.length + ";");
        p("");
        p("inline constexpr RandomVector kRandomVectors[kRandomVectorCount] = {");
        for (long seed : SEEDS) {
            emitVector(seed);
        }
        p("};");
        p("");
        p("}  // namespace mc::test");
    }

    private static void emitVector(long seed) {
        p("    {");
        p("        " + lit(seed) + ",");
        p("        " + ulit(rawSeedOf(seed)) + ",");

        // Each block starts from a fresh Random so that a failure names one
        // method instead of everything after the first divergence.
        StringBuilder sb = new StringBuilder();
        Random r = new Random(seed);
        for (int i = 0; i < N; i++) {
            sb.append(i == 0 ? "" : ", ").append(r.nextInt());
        }
        p("        {" + sb + "},");

        p("        {");
        for (int b = 0; b < BOUNDS.length; b++) {
            sb = new StringBuilder();
            r = new Random(seed);
            for (int i = 0; i < N; i++) {
                sb.append(i == 0 ? "" : ", ").append(r.nextInt(BOUNDS[b]));
            }
            p("            {" + sb + "},");
        }
        p("        },");

        sb = new StringBuilder();
        r = new Random(seed);
        for (int i = 0; i < N; i++) {
            sb.append(i == 0 ? "" : ", ").append(lit(r.nextLong()));
        }
        p("        {" + sb + "},");

        sb = new StringBuilder();
        r = new Random(seed);
        for (int i = 0; i < N; i++) {
            sb.append(i == 0 ? "" : ", ")
              .append(hex32(Float.floatToRawIntBits(r.nextFloat())));
        }
        p("        {" + sb + "},");

        sb = new StringBuilder();
        r = new Random(seed);
        for (int i = 0; i < N; i++) {
            sb.append(i == 0 ? "" : ", ")
              .append(hex64(Double.doubleToRawLongBits(r.nextDouble())));
        }
        p("        {" + sb + "},");

        sb = new StringBuilder();
        r = new Random(seed);
        for (int i = 0; i < N; i++) {
            sb.append(i == 0 ? "" : ", ").append(r.nextBoolean() ? "1" : "0");
        }
        p("        {" + sb + "},");

        sb = new StringBuilder();
        r = new Random(seed);
        for (int i = 0; i < N; i++) {
            sb.append(i == 0 ? "" : ", ")
              .append(hex64(Double.doubleToRawLongBits(r.nextGaussian())));
        }
        p("        {" + sb + "},");

        p("    },");
    }

    // What Random's constructor scrambles the seed into. Not reachable through
    // the public API, so it is recomputed here from the documented expression
    // rather than read out of the object.
    private static long rawSeedOf(long seed) {
        return (seed ^ 0x5DEECE66DL) & ((1L << 48) - 1);
    }

    private static String join(int[] values) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < values.length; i++) {
            sb.append(i == 0 ? "" : ", ").append(values[i]);
        }
        return sb.toString();
    }

    // Long.MIN_VALUE has no positive literal in C++ either, so it is written as
    // the shifted form the compiler will accept without an overflow warning.
    private static String lit(long value) {
        if (value == Long.MIN_VALUE) {
            return "(-0x7FFFFFFFFFFFFFFFLL - 1)";
        }
        return value + "LL";
    }

    private static String ulit(long value) {
        return String.format("0x%016XULL", value);
    }

    private static String hex32(int value) {
        return String.format("0x%08XU", value);
    }

    private static String hex64(long value) {
        return String.format("0x%016XULL", value);
    }

    // ---------------------------------------------------------------------
    // Block collision boxes -- `ly.a(cn,III,cf,ArrayList)`, addCollisionBoxesToList
    // ---------------------------------------------------------------------
    //
    // The whole truth table: every block this version constructs, crossed with
    // all sixteen metadata values, asked the same question `Entity.moveEntity`
    // asks through `World.getCollidingBoundingBoxes`. Not a sample -- 16 is the
    // entire domain of a metadata nibble, so there is nothing else to ask.
    //
    // **Why addCollisionBoxesToList and not getCollisionBoundingBoxFromPool.**
    // The list form is the one the physics calls, and it is the only one that
    // can answer with more than one box. Exactly one block in a1.1.2 does:
    // stairs return two, a lower half-step and an upper full-height part. Going
    // through the single-box entry point would silently flatten them.
    //
    // **Two facts this fixture establishes by measurement, both of which could
    // reasonably have gone the other way:**
    //
    //   * A collision box is a pure function of (id, metadata). Nothing reads a
    //     neighbour. The obvious candidate was the top half of a door, which
    //     looked like it must consult the half below it for the hinge -- it does
    //     not, and a door's box comes from its own low three bits, with bit 8
    //     (the top-half flag) ignored entirely. Fences do not connect either.
    //     This is what lets our resolver be a pure function with no world
    //     access, which in turn is what makes it testable without a world.
    //
    //   * **A ladder whose metadata is not 2, 3, 4 or 5 sets no bounds at
    //     all.** BlockLadder assigns its box inside four un-elsed `if`s, so an
    //     out-of-range value leaves the shared Block singleton holding whatever
    //     the *previous* query left there -- ask the same ladder the same
    //     question twice in a different order and it answers differently. That
    //     is a bug in the original, and it is why the loop below restores each
    //     block's bounds before every query. **It restores that block's own
    //     constructor defaults, snapshotted before anything has run, not a unit
    //     cube:** a slab, a cactus and a fence all set their bounds once in the
    //     constructor and never again, so resetting to a unit cube would
    //     quietly record a slab as a full block. The value recorded for a
    //     ladder's unreachable metadata is therefore its constructor default.
    //     The game never writes those values.
    //
    // Doubles are emitted as plain decimal literals rather than the raw bit
    // patterns the noise fixtures use, because every bound here is a multiple
    // of a sixteenth and so is exact in binary -- and a table a human is going
    // to cross-check against a slab and a stair is worth being able to read.
    // The emitter does not take that on trust: it parses each literal back and
    // refuses to emit one that does not compare equal to the double it came
    // from.
    //
    // Regenerate with:
    //   java tools/genref.java --jar <client.jar> --collision /tmp/genref-scratch
    //     redirected to tests/collision_box_vectors.hpp

    private static final int COLLISION_MAX_BOXES = 2;

    private static String dbl(double v) {
        String s;
        if (v == Math.rint(v) && Math.abs(v) < 1e15) {
            s = String.valueOf((long) v) + ".0";
        } else {
            s = String.valueOf(v);
        }
        if (Double.parseDouble(s) != v) {
            throw new IllegalStateException("literal " + s + " does not round-trip to " + v);
        }
        return s;
    }

    private static void emitCollision(String jarPath, String scratchDir) {
        java.io.File dir = new java.io.File(scratchDir, "collision");
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> blockClass = loader.loadClass("ly");
            Class<?> worldClass = loader.loadClass(WORLD);
            Class<?> aabbClass = loader.loadClass("cf");

            Object[] blocks = (Object[]) blockClass.getField("n").get(null);

            java.lang.reflect.Constructor<?> ctor =
                worldClass.getConstructor(java.io.File.class, String.class, long.class);
            java.lang.reflect.Method getChunk = worldClass.getMethod("b", int.class, int.class);
            // setBlockAndMetadata(x, y, z, id, metadata). **Not raw**: it runs
            // onBlockAdded, which drops an unsupported torch on the floor and
            // rewrites a placed one's metadata to 5. That is correct game
            // behaviour and exactly wrong for enumerating a shape table, so the
            // metadata is forced afterwards through the chunk -- see below.
            java.lang.reflect.Method setBlockAndData = worldClass.getMethod(
                "a", int.class, int.class, int.class, int.class, int.class);
            // Chunk.setBlockMetadata(localX, y, localZ, metadata), which writes
            // the nibble and nothing else.
            Class<?> chunkClass = loader.loadClass("ga");
            java.lang.reflect.Method setChunkMeta = chunkClass.getMethod(
                "b", int.class, int.class, int.class, int.class);
            java.lang.reflect.Method getBlockId = worldClass.getMethod(
                "a", int.class, int.class, int.class);
            java.lang.reflect.Method getBlockMeta = worldClass.getMethod(
                "e", int.class, int.class, int.class);
            // addCollisionBoxesToList(world, x, y, z, mask, out)
            java.lang.reflect.Method addBoxes = blockClass.getMethod(
                "a", worldClass, int.class, int.class, int.class, aabbClass,
                java.util.ArrayList.class);
            // Block's own minX..maxZ. Written directly rather than through
            // setBlockBounds(float x6), because these are doubles and a round
            // trip through float is a lossy step this has no reason to take.
            java.lang.reflect.Field[] bound = {
                blockClass.getField("bf"), blockClass.getField("bg"), blockClass.getField("bh"),
                blockClass.getField("bi"), blockClass.getField("bj"), blockClass.getField("bk"),
            };

            // Block.slipperiness -- 0.6 for everything except ice.
            java.lang.reflect.Field slipperiness = blockClass.getField("bo");

            // **collisionRayTrace, called for its side effect.**
            // `setBlockBoundsBasedOnState` is an *empty method* on Block in
            // a1.1.2 and a torch does not override it -- the torch overrides
            // collisionRayTrace instead and sets its bounds inline before
            // delegating to super. So the only way to see the shape the ray
            // actually tests against is to run the same entry point the ray
            // does and then read what it left on the singleton. Asking
            // setBlockBoundsBasedOnState instead reports every torch in the
            // game as a full cube.
            java.lang.reflect.Method collisionRayTrace = blockClass.getMethod(
                "a", worldClass, int.class, int.class, int.class,
                loader.loadClass("aj"), loader.loadClass("aj"));
            // **getSelectedBoundingBoxFromPool, and it is the one the outline
            // is actually drawn around.** `RenderGlobal.drawSelectionBox` asks
            // this and nothing else, and three blocks in a1.1.2 -- the ladder
            // (`br`), the cactus (`hy`) and the staircase (`km`) -- override it
            // while overriding `collisionRayTrace` not at all. Reading the ray
            // trace's residue alone therefore recorded **a ladder as a full
            // cube**, because `br` sets its two-sixteenths box in `d` and `f`
            // and never on the path a ray takes.
            //
            // It is called *after* collisionRayTrace and not instead of it, so
            // the sequence matches the frame's: `Block.f` falls through to the
            // block's own bf..bk for everything that does not override it, and
            // for a torch those are what its collisionRayTrace just set. Doing
            // only one of the two loses one shape or the other.
            java.lang.reflect.Method selectedBox = blockClass.getMethod(
                "f", worldClass, int.class, int.class, int.class);
            Class<?> vecClass = loader.loadClass("aj");
            java.lang.reflect.Method makeVec = vecClass.getMethod(
                "a", double.class, double.class, double.class);
            // canCollideCheck(metadata, hitLiquids) -- whether the ray sees it
            // at all.
            // canCollideCheck(metadata, hitLiquids) -- asked **both ways**.
            // `hitLiquids` is the flag `ItemBucket` passes when it looks for
            // something to scoop, and exactly one class reads it: `jp`,
            // BlockFluid, whose whole override is `hitLiquids && metadata == 0`.
            // So the answer varies with metadata in the true case and not in
            // the false one, and the two need separate columns.
            java.lang.reflect.Method canCollideCheck = blockClass.getMethod(
                "a", int.class, boolean.class);
            // AxisAlignedBB.getBoundingBox -- the unpooled factory, so the mask
            // cannot be recycled out from under us by anything the block does.
            java.lang.reflect.Method makeBox = aabbClass.getMethod(
                "a", double.class, double.class, double.class,
                double.class, double.class, double.class);

            java.lang.reflect.Field[] edge = {
                aabbClass.getField("a"), aabbClass.getField("b"), aabbClass.getField("c"),
                aabbClass.getField("d"), aabbClass.getField("e"), aabbClass.getField("f"),
            };

            deleteTree(dir);
            dir.mkdirs();
            Object world = ctor.newInstance(dir, "genref", 1234567890L);
            getChunk.invoke(world, 0, 0);

            // High above the generated surface, so the block under test is
            // surrounded by air and nothing it is placed against can matter.
            final int bx = 8;
            final int by = 100;
            final int bz = 8;
            // Something to stand on. Without it a torch, a sapling or a rail
            // deletes itself the instant it is placed and the sweep records the
            // shape of air.
            final int supportY = by - 1;
            final double[] origin = {bx, by, bz, bx, by, bz};

            // Snapshotted before a single query runs, so nothing has had a
            // chance to leave its own bounds behind on the singleton.
            double[][] defaults = new double[256][6];
            for (int id = 0; id < 256 && id < blocks.length; id++) {
                if (blocks[id] == null) {
                    continue;
                }
                for (int e = 0; e < 6; e++) {
                    defaults[id][e] = bound[e].getDouble(blocks[id]);
                }
            }

            p("// Generated by tools/genref.java --jar <client.jar> --collision <scratch-dir>.");
            p("// Do not edit by hand.");
            p("//");
            p("// **Every collision box in a1.1.2**: each block the version constructs, crossed");
            p("// with all sixteen metadata values, asked through `addCollisionBoxesToList` --");
            p("// the same entry point `Entity.moveEntity` reaches through");
            p("// `World.getCollidingBoundingBoxes`. Sixteen is the whole domain of a metadata");
            p("// nibble, so this is the entire truth table rather than a sample of it.");
            p("//");
            p("// Coordinates are relative to the block's own corner, so a full cube is");
            p("// 0,0,0 -> 1,1,1. Boxes are listed in the order the original appends them.");
            p("//");
            p("// Measured facts worth not re-deriving:");
            p("//");
            p("//   * A box depends only on (id, metadata). Nothing consults a neighbour --");
            p("//     not the top half of a door, which reads its own low three bits and");
            p("//     ignores bit 8 entirely, and not a fence, which does not connect. That");
            p("//     is what lets mc::block::collisionBoxes be a pure function.");
            p("//   * Stairs are the only block that answers with more than one box.");
            p("//   * A ladder with metadata outside 2..5 sets no bounds at all, and in the");
            p("//     original answers with whatever the shared Block singleton was left");
            p("//     holding by the previous query. The generator restores each block's own");
            p("//     constructor bounds before every query, so what is recorded here is that");
            p("//     default rather than an artefact of iteration order.");
            p("//   * **The selection box comes from getSelectedBoundingBoxFromPool**, which is");
            p("//     the method the outline is drawn around -- not from the bounds a ray trace");
            p("//     leaves behind. Three blocks override one and not the other, and reading");
            p("//     only the ray's residue reported a ladder as a full cube and a cactus as");
            p("//     its collision box. See kSelectionBoxes below.");
            p("//");
            p("// Regenerate with:");
            p("//   java tools/genref.java --jar <client.jar> --collision /tmp/genref-scratch");
            p("//     redirected to tests/collision_box_vectors.hpp");
            p("");
            p("#pragma once");
            p("");
            p("#include \"core/util/types.hpp\"");
            p("");
            p("namespace mc::test {");
            p("");
            p("struct CollisionBox {");
            p("    double minX, minY, minZ, maxX, maxY, maxZ;");
            p("};");
            p("");
            p("struct CollisionCase {");
            p("    u16 id;");
            p("    u8 metadata;");
            p("    int boxCount;                 // 0 means the block does not collide");
            p("    CollisionBox boxes[" + COLLISION_MAX_BOXES + "];");
            p("};");
            p("");

            StringBuilder table = new StringBuilder();
            StringBuilder selection = new StringBuilder();
            StringBuilder targets = new StringBuilder();
            StringBuilder liquidTargets = new StringBuilder();
            int cases = 0;
            int empty = 0;
            int multi = 0;
            int nonUnit = 0;
            int targetableCases = 0;
            int unplaceable = 0;

            for (int id = 0; id < 256; id++) {
                if (id >= blocks.length || blocks[id] == null) {
                    continue;
                }
                for (int meta = 0; meta < 16; meta++) {
                    // Clear the block, then put this block's own constructor
                    // bounds back, so one that sets none per-state is recorded
                    // as itself rather than as the last block's leftovers.
                    setBlockAndData.invoke(world, bx, by, bz, 0, 0);
                    setBlockAndData.invoke(world, bx, supportY, bz, 1, 0);
                    for (int e = 0; e < 6; e++) {
                        bound[e].setDouble(blocks[id], defaults[id][e]);
                    }
                    setBlockAndData.invoke(world, bx, by, bz, id, meta);
                    // Force the nibble under the placement rules, so the sweep
                    // really is every metadata rather than every metadata the
                    // block would have accepted.
                    Object chunk = getChunk.invoke(world, bx >> 4, bz >> 4);
                    setChunkMeta.invoke(chunk, bx & 15, by, bz & 15, meta);
                    final boolean survived =
                        ((Integer) getBlockId.invoke(world, bx, by, bz)).intValue() == id
                        && ((Integer) getBlockMeta.invoke(world, bx, by, bz)).intValue() == meta;
                    if (!survived) {
                        unplaceable++;
                    }

                    Object mask = makeBox.invoke(null,
                        bx - 1.0, by - 1.0, bz - 1.0, bx + 2.0, by + 2.0, bz + 2.0);
                    java.util.ArrayList<Object> found = new java.util.ArrayList<Object>();
                    addBoxes.invoke(blocks[id], world, bx, by, bz, mask, found);

                    // The **selection** shape, which is not the collision shape:
                    // `collisionRayTrace` calls setBlockBoundsBasedOnState and
                    // then reads the block's own bounds, so a torch -- which
                    // collides with nothing -- still has a box to be looked at.
                    for (int e = 0; e < 6; e++) {
                        bound[e].setDouble(blocks[id], defaults[id][e]);
                    }
                    // A ray from well outside the block to well outside the
                    // other side, so it is a real query rather than a
                    // degenerate one. What it returns is discarded; what it
                    // leaves in bf..bk is the answer.
                    collisionRayTrace.invoke(blocks[id], world, bx, by, bz,
                        makeVec.invoke(null, bx - 2.0, by + 0.5, bz + 0.5),
                        makeVec.invoke(null, bx + 3.0, by + 0.5, bz + 0.5));
                    // Then the outline's own question, on top of that state.
                    // The answer comes back in **world** coordinates, so the
                    // block's corner comes off each edge to leave the local box
                    // the rest of this fixture is written in.
                    Object picked = selectedBox.invoke(blocks[id], world, bx, by, bz);
                    final double[] corner = {bx, by, bz, bx, by, bz};
                    StringBuilder pick = new StringBuilder();
                    for (int e = 0; e < 6; e++) {
                        pick.append(e == 0 ? "" : ", ")
                            .append(dbl(edge[e].getDouble(picked) - corner[e]));
                    }
                    final boolean targetable =
                        ((Boolean) canCollideCheck.invoke(blocks[id], meta, Boolean.FALSE))
                            .booleanValue();
                    final boolean targetableLiquid =
                        ((Boolean) canCollideCheck.invoke(blocks[id], meta, Boolean.TRUE))
                            .booleanValue();
                    selection.append("    {").append(pick).append("},   // ")
                             .append(id).append(" meta ").append(meta).append("\n");
                    targets.append(targetable ? "    true,\n" : "    false,\n");
                    liquidTargets.append(targetableLiquid ? "    true,\n" : "    false,\n");
                    if (targetable) {
                        targetableCases++;
                    }

                    if (found.size() > COLLISION_MAX_BOXES) {
                        throw new IllegalStateException("block " + id + " metadata " + meta
                            + " returned " + found.size() + " boxes; COLLISION_MAX_BOXES is "
                            + COLLISION_MAX_BOXES);
                    }

                    StringBuilder boxes = new StringBuilder();
                    for (int b = 0; b < COLLISION_MAX_BOXES; b++) {
                        boxes.append(b == 0 ? "" : ", ").append("{");
                        if (b < found.size()) {
                            Object box = found.get(b);
                            boolean unit = true;
                            for (int e = 0; e < 6; e++) {
                                double v = edge[e].getDouble(box) - origin[e];
                                boxes.append(e == 0 ? "" : ", ").append(dbl(v));
                                if (v != (e < 3 ? 0.0 : 1.0)) {
                                    unit = false;
                                }
                            }
                            if (!unit) {
                                nonUnit++;
                            }
                        } else {
                            boxes.append("0.0, 0.0, 0.0, 0.0, 0.0, 0.0");
                        }
                        boxes.append("}");
                    }

                    if (found.isEmpty()) {
                        empty++;
                    }
                    if (found.size() > 1) {
                        multi++;
                    }
                    cases++;
                    table.append("    {").append(id).append(", ").append(meta).append(", ")
                         .append(found.size()).append(", {").append(boxes).append("}},\n");
                }
            }
            setBlockAndData.invoke(world, bx, by, bz, 0, 0);

            // **The shape a block is drawn as when it is an item**, which is a
            // third question again and is the one the hand and the inventory
            // slot ask. `RenderBlocks.renderBlockAsItem` calls
            // `Block.setBlockBoundsForItemRender` -- `ly.e()V`, empty on Block
            // -- and then draws the block's bf..bk, so for all but a handful of
            // classes it is the constructor's own bounds. Two classes in
            // a1.1.2 override it, and both are blocks whose bounds otherwise
            // live only in `setBlockBoundsBasedOnState`, where an item render
            // would find the previous query's leftovers: `hu` (the button) and
            // `al` (both pressure plates).
            //
            // Measured the same way everything else here is: put the block's
            // constructor bounds back, call the method, read what it left.
            java.lang.reflect.Method itemRenderBounds = blockClass.getMethod("e");
            StringBuilder itemBoxes = new StringBuilder();
            int itemOverrides = 0;
            for (int id = 0; id < 256; id++) {
                if (id >= blocks.length || blocks[id] == null) {
                    itemBoxes.append("    {0.0, 0.0, 0.0, 1.0, 1.0, 1.0},   // ")
                             .append(id).append(" -- not a block in this version\n");
                    continue;
                }
                for (int e = 0; e < 6; e++) {
                    bound[e].setDouble(blocks[id], defaults[id][e]);
                }
                itemRenderBounds.invoke(blocks[id]);
                StringBuilder row = new StringBuilder();
                boolean moved = false;
                for (int e = 0; e < 6; e++) {
                    double v = bound[e].getDouble(blocks[id]);
                    row.append(e == 0 ? "" : ", ").append(dbl(v));
                    if (v != defaults[id][e]) {
                        moved = true;
                    }
                }
                if (moved) {
                    itemOverrides++;
                }
                itemBoxes.append("    {").append(row).append("},   // ").append(id)
                         .append(moved ? "  <- setBlockBoundsForItemRender\n" : "\n");
                for (int e = 0; e < 6; e++) {
                    bound[e].setDouble(blocks[id], defaults[id][e]);
                }
            }

            // Slipperiness rides along in this fixture rather than getting one of
            // its own: it is the other half of what a block does to something
            // standing on it, `EntityLiving.moveEntityWithHeading` reads it out
            // of the same table in the same breath as the collision box, and it
            // is one float per block rather than a fixture's worth of data.
            p("// Ground friction, `Block.slipperiness`. The default is 0.6 and exactly one");
            p("// block in a1.1.2 overrides it. `moveEntityWithHeading` multiplies this by");
            p("// 0.91 to get the factor it applies to horizontal motion each tick.");
            p("inline constexpr float kBlockSlipperiness[256] = {");
            for (int row = 0; row < 256; row += 8) {
                StringBuilder sb = new StringBuilder();
                for (int id = row; id < row + 8; id++) {
                    float value = 0.0f;
                    if (id < blocks.length && blocks[id] != null) {
                        value = slipperiness.getFloat(blocks[id]);
                    }
                    sb.append(id == row ? "" : ", ").append(value).append("f");
                }
                p("    " + sb + ",");
            }
            p("};");
            p("");
            p("inline constexpr int kCollisionCaseCount = " + cases + ";");
            p("");
            p("// The **selection** box, in the same order as kCollisionCases: what");
            p("// `Block.getSelectedBoundingBoxFromPool` answers once a ray trace has run over");
            p("// the block, which is exactly the sequence a frame performs -- rayTraceBlocks");
            p("// first, then drawSelectionBox. It is not the collision box: a torch has no");
            p("// collision box at all and still has one of these, which is why a torch can be");
            p("// aimed at and broken but not stood on.");
            p("//");
            p("// **Both calls are needed and neither alone is right.** `Block.f` falls through");
            p("// to the block's own bf..bk, and a torch's are set by its own collisionRayTrace");
            p("// override -- so asking only `f` loses the torch's post. The ladder (`br`), the");
            p("// cactus (`hy`) and the staircase (`km`) instead override `f` and leave");
            p("// collisionRayTrace alone -- so asking only the ray loses the ladder's two");
            p("// sixteenths and reported it, wrongly, as a full cube.");
            p("inline constexpr CollisionBox kSelectionBoxes[kCollisionCaseCount] = {");
            System.out.print(selection);
            p("};");
            p("");
            p("// **The bounds an item render uses**, one box per block id rather than one per");
            p("// (id, metadata) pair: `RenderBlocks.renderBlockAsItem` has no metadata to");
            p("// consult and calls `Block.setBlockBoundsForItemRender` on the singleton before");
            p("// it draws. Block's own is empty, so this is the constructor's bounds for all");
            p("// but the two classes that override it -- `hu`, the button, and `al`, both");
            p("// pressure plates -- and those two are exactly the blocks whose world bounds");
            p("// are set in setBlockBoundsBasedOnState and are therefore leftovers at");
            p("// metadata 0. An id this version does not construct is a unit cube here, as it");
            p("// is everywhere else.");
            p("inline constexpr CollisionBox kItemRenderBoxes[256] = {");
            System.out.print(itemBoxes);
            p("};");
            p("");
            p("// How many blocks moved their bounds in setBlockBoundsForItemRender. A reader");
            p("// that ignored the call entirely would still match every other row.");
            p("inline constexpr int kItemRenderOverrides = " + itemOverrides + ";");
            p("");
            p("// Block.canCollideCheck(metadata, false), which in a1.1.2 is just isCollidable():");
            p("// whether the ray notices the block before it looks at its shape.");
            p("inline constexpr bool kTargetable[kCollisionCaseCount] = {");
            System.out.print(targets);
            p("};");
            p("");
            p("// The same question with **hitLiquids true**, which is the flag ItemBucket passes");
            p("// when it goes looking for something to scoop. One class in a1.1.2 reads it --");
            p("// `jp`, BlockFluid, whose entire override is `hitLiquids && metadata == 0` -- so");
            p("// this differs from kTargetable only for water and lava, and only at metadata 0.");
            p("// **That is why an empty bucket fills from a source block and not from a stream.**");
            p("inline constexpr bool kTargetableWithLiquids[kCollisionCaseCount] = {");
            System.out.print(liquidTargets);
            p("};");
            p("");
            p("inline constexpr int kTargetableCases = " + targetableCases + ";");
            p("");
            p("// How many (id, metadata) pairs the game refused to hold even with the nibble");
            p("// forced -- a block that deletes itself on placement whatever is under it. Their");
            p("// shapes are still enumerated, because the shape of a block is a property of the");
            p("// block and not of whether this scene could keep one alive.");
            p("inline constexpr int kUnplaceableCases = " + unplaceable + ";");
            p("");
            p("");
            p("// Negative controls. A resolver that answered \"unit cube, always\" would pass a");
            p("// fixture with no empties and no odd sizes in it; one that answered \"nothing");
            p("// collides\" would pass a fixture that was all empties. The tests assert these");
            p("// counts, so neither degenerate resolver can be green.");
            p("inline constexpr int kCollisionEmptyCases = " + empty + ";");
            p("inline constexpr int kCollisionMultiBoxCases = " + multi + ";");
            p("inline constexpr int kCollisionNonUnitBoxes = " + nonUnit + ";");
            p("");
            p("inline constexpr CollisionCase kCollisionCases[kCollisionCaseCount] = {");
            System.out.print(table);
            p("};");
            p("");
            p("}  // namespace mc::test");
        } catch (Exception e) {
            System.err.println("genref --collision failed: " + e);
            e.printStackTrace();
            System.exit(1);
        } finally {
            deleteTree(dir);
        }
    }

    // ---------------------------------------------------------------------
    // The player body -- `ge.b(FF)` (moveEntityWithHeading) driving
    // `kh.c(DDD)` (moveEntity), on a real `dm` (EntityPlayer)
    // ---------------------------------------------------------------------
    //
    // **The entity is the real EntityPlayer, not a stand-in.** `dm` turns out to
    // be concrete, to take nothing but a World, and to override neither of the
    // two movement methods -- so this drives the same code the game does, with
    // the game's own 0.6 x 1.8 box, 1.62 eye and 0.5 step height, all of which
    // the emitter reads back out and prints rather than assuming.
    //
    // **Each scene is emitted as the list of blocks that make it**, not as a
    // procedure, so the C++ test builds a bit-identical world without either
    // side reimplementing the other's scene builder. The play volume is cleared
    // to air first and the clear is part of the contract: the harness starts
    // from air and places exactly these blocks.
    //
    // **Doubles are raw bit patterns here, unlike the collision fixture.**
    // There every bound was a multiple of a sixteenth and exact in decimal;
    // here almost nothing is -- gravity compounds through a drag of
    // 0.9800000190734863 -- and a decimal round trip could hide the one-ulp
    // disagreement that this exists to catch.
    //
    // **What it does not cover: sneaking.** `Entity.isSneaking` is a hardcoded
    // false and only the client-side player class overrides it, so an
    // EntityPlayer cannot be made to sneak from outside. The ledge walk-back in
    // moveEntity is therefore unreachable from here and is tested by hand.
    // Water and lava are likewise out: the C++ side implements the land branch.
    //
    // Regenerate with:
    //   java tools/genref.java --jar <client.jar> --player /tmp/genref-scratch
    //     redirected to tests/player_body_vectors.hpp

    // A scene: blocks placed relative to the case's origin, as
    // {dx, dy, dz, id, metadata}.
    private static final int[][] SCENE_NONE = {};

    private static int[][] plate(int id, int x0, int x1, int z0, int z1) {
        java.util.List<int[]> out = new java.util.ArrayList<int[]>();
        for (int x = x0; x <= x1; x++) {
            for (int z = z0; z <= z1; z++) {
                out.add(new int[]{x, 0, z, id, 0});
            }
        }
        return out.toArray(new int[0][]);
    }

    // A solid volume of one block, `y0` to `y1` inclusive above the plate. Used
    // for the liquid cases; `plate` only makes a single layer.
    private static int[][] pool(int id, int x0, int x1, int z0, int z1, int y0, int y1) {
        java.util.List<int[]> out = new java.util.ArrayList<int[]>();
        for (int x = x0; x <= x1; x++) {
            for (int z = z0; z <= z1; z++) {
                for (int y = y0; y <= y1; y++) {
                    out.add(new int[]{x, y, z, id, 0});
                }
            }
        }
        return out.toArray(new int[0][]);
    }

    private static int[][] concat(int[][] a, int[][] b) {
        int[][] out = new int[a.length + b.length][];
        System.arraycopy(a, 0, out, 0, a.length);
        System.arraycopy(b, 0, out, a.length, b.length);
        return out;
    }

    // Some of Entity's fields are protected rather than public -- fallDistance
    // is -- so this walks the hierarchy and forces access rather than making
    // the caller know which are which.
    private static java.lang.reflect.Field entityField(Class<?> from, String name)
            throws NoSuchFieldException {
        for (Class<?> c = from; c != null; c = c.getSuperclass()) {
            try {
                java.lang.reflect.Field f = c.getDeclaredField(name);
                f.setAccessible(true);
                return f;
            } catch (NoSuchFieldException ignored) {
                // keep walking
            }
        }
        throw new NoSuchFieldException(name);
    }

    private static String dbits(double v) {
        return "0x" + Long.toHexString(Double.doubleToRawLongBits(v)) + "ULL";
    }

    private static String fbits(float v) {
        return "0x" + Integer.toHexString(Float.floatToRawIntBits(v)) + "u";
    }

    private static void emitPlayerBody(String jarPath, String scratchDir) {
        java.io.File dir = new java.io.File(scratchDir, "player");
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> worldClass = loader.loadClass(WORLD);
            Class<?> entityClass = loader.loadClass("kh");
            Class<?> livingClass = loader.loadClass("ge");
            Class<?> playerClass = loader.loadClass("dm");

            java.lang.reflect.Constructor<?> worldCtor =
                worldClass.getConstructor(java.io.File.class, String.class, long.class);
            java.lang.reflect.Constructor<?> playerCtor = playerClass.getConstructor(worldClass);
            java.lang.reflect.Method getChunk = worldClass.getMethod("b", int.class, int.class);
            java.lang.reflect.Method setBlockAndData = worldClass.getMethod(
                "a", int.class, int.class, int.class, int.class, int.class);

            java.lang.reflect.Method setPosition = entityClass.getMethod(
                "a", double.class, double.class, double.class);
            java.lang.reflect.Method moveWithHeading =
                livingClass.getMethod("b", float.class, float.class);
            java.lang.reflect.Method jumpMethod = livingClass.getDeclaredMethod("C");
            jumpMethod.setAccessible(true);
            // `kh.g_()` -- isInWater, and `kh.G()` -- handleLavaMovement. The
            // jump branch of onLivingUpdate asks both before it decides that
            // holding the button means "jump" rather than "rise".
            java.lang.reflect.Method inWaterMethod = entityClass.getMethod("g_");
            java.lang.reflect.Method inLavaMethod = entityClass.getMethod("G");

            java.lang.reflect.Field posX = entityField(entityClass, "ak");
            java.lang.reflect.Field posY = entityField(entityClass, "al");
            java.lang.reflect.Field posZ = entityField(entityClass, "am");
            java.lang.reflect.Field motionX = entityField(entityClass, "an");
            java.lang.reflect.Field motionY = entityField(entityClass, "ao");
            java.lang.reflect.Field motionZ = entityField(entityClass, "ap");
            java.lang.reflect.Field yawField = entityField(entityClass, "aq");
            java.lang.reflect.Field boxField = entityField(entityClass, "au");
            java.lang.reflect.Field onGround = entityField(entityClass, "av");
            java.lang.reflect.Field collidedH = entityField(entityClass, "aw");
            java.lang.reflect.Field collidedV = entityField(entityClass, "ax");
            java.lang.reflect.Field yOffset = entityField(entityClass, "aB");
            java.lang.reflect.Field fallDistance = entityField(entityClass, "aH");
            java.lang.reflect.Field ySize = entityField(entityClass, "aL");
            java.lang.reflect.Field stepHeight = entityField(entityClass, "aM");
            java.lang.reflect.Field widthField = entityField(entityClass, "aC");
            java.lang.reflect.Field heightField = entityField(entityClass, "aD");
            java.lang.reflect.Field boxMinY = entityField(loader.loadClass("cf"), "b");

            final int STONE = 1;
            final int SLAB = 44;
            final int ICE = 79;
            final int FENCE = 85;
            final int WATER = 9;
            final int LAVA = 11;

            // {name, scene, yaw, strafe, forward, jump, startFeetAbove, steps}
            //
            // **Yaw 0 faces +Z**, so every walkway runs long in z and the
            // obstacles sit at +z. Building them along x instead makes the
            // player stroll off the side, which is a good way to spend an
            // afternoon comparing two different falls.
            Object[][] cases = {
                {"fall_onto_stone_from_eight_blocks", plate(STONE, -3, 3, -3, 3),
                 0.0f, 0.0f, 0.0f, false, 8.0, 45},
                {"walk_forward_on_flat_stone", plate(STONE, -3, 3, -3, 12),
                 0.0f, 0.0f, 1.0f, false, 0.0, 40},
                {"walk_forward_into_a_wall", concat(plate(STONE, -3, 3, -3, 12),
                     new int[][]{{-1,1,3,STONE,0},{0,1,3,STONE,0},{1,1,3,STONE,0},
                                 {-1,2,3,STONE,0},{0,2,3,STONE,0},{1,2,3,STONE,0}}),
                 0.0f, 0.0f, 1.0f, false, 0.0, 40},
                {"step_up_a_slab", concat(plate(STONE, -3, 3, -3, 12),
                     new int[][]{{-1,1,3,SLAB,0},{0,1,3,SLAB,0},{1,1,3,SLAB,0}}),
                 0.0f, 0.0f, 1.0f, false, 0.0, 40},
                {"a_full_block_is_too_tall_to_step", concat(plate(STONE, -3, 3, -3, 12),
                     new int[][]{{-1,1,3,STONE,0},{0,1,3,STONE,0},{1,1,3,STONE,0}}),
                 0.0f, 0.0f, 1.0f, false, 0.0, 40},
                {"a_fence_is_a_block_and_a_half_and_cannot_be_jumped",
                 concat(plate(STONE, -3, 3, -3, 12),
                     new int[][]{{-1,1,3,FENCE,0},{0,1,3,FENCE,0},{1,1,3,FENCE,0}}),
                 0.0f, 0.0f, 1.0f, true, 0.0, 45},
                {"walk_off_the_end_of_a_ledge", plate(STONE, -3, 3, -3, 2),
                 0.0f, 0.0f, 1.0f, false, 0.0, 30},
                {"jump_repeatedly_on_the_spot", plate(STONE, -3, 3, -3, 3),
                 0.0f, 0.0f, 0.0f, true, 0.0, 45},
                {"walk_on_ice_keeps_sliding", plate(ICE, -3, 3, -3, 12),
                 0.0f, 0.0f, 1.0f, false, 0.0, 45},
                {"walk_diagonally_into_a_corner", concat(plate(STONE, -8, 8, -8, 8),
                     new int[][]{{-1,1,3,STONE,0},{0,1,3,STONE,0},{1,1,3,STONE,0},
                                 {2,1,3,STONE,0},{2,1,2,STONE,0},{2,1,1,STONE,0},
                                 {2,1,0,STONE,0},{2,1,-1,STONE,0}}),
                 45.0f, 0.0f, 1.0f, false, 0.0, 40},
                {"strafe_and_forward_together_do_not_go_faster",
                 plate(STONE, -10, 10, -10, 10),
                 0.0f, 1.0f, 1.0f, false, 0.0, 30},

                // **The liquid branches**, which `moveEntityWithHeading` takes
                // before it ever looks at the ground. Still water and still
                // lava only, and that is a deliberate restriction rather than
                // laziness: `handleMaterialAcceleration` also pushes the player
                // along a fluid's flow vector, and the port does not implement
                // that push yet. A pool of source blocks has no flow at its
                // interior, so these cases compare the parts that *are*
                // implemented and would stop comparing the moment the pool
                // started running. See core/entity/player_body.cpp.
                {"sink_in_still_water", concat(plate(STONE, -4, 4, -4, 4),
                     pool(WATER, -4, 4, -4, 4, 1, 5)),
                 0.0f, 0.0f, 0.0f, false, 3.0, 60},
                {"swim_forward_in_still_water", concat(plate(STONE, -4, 4, -4, 12),
                     pool(WATER, -4, 4, -4, 12, 1, 5)),
                 0.0f, 0.0f, 1.0f, false, 3.0, 60},
                {"swim_up_by_holding_jump", concat(plate(STONE, -4, 4, -4, 4),
                     pool(WATER, -4, 4, -4, 4, 1, 6)),
                 0.0f, 0.0f, 0.0f, true, 2.0, 60},
                {"wade_out_of_water_onto_a_shore",
                 concat(concat(plate(STONE, -4, 4, -4, 12),
                     pool(WATER, -4, 4, -4, 4, 1, 3)),
                     new int[][]{{-1,1,5,STONE,0},{0,1,5,STONE,0},{1,1,5,STONE,0},
                                 {-1,2,5,STONE,0},{0,2,5,STONE,0},{1,2,5,STONE,0}}),
                 0.0f, 0.0f, 1.0f, false, 2.0, 60},
                {"sink_in_still_lava", concat(plate(STONE, -4, 4, -4, 4),
                     pool(LAVA, -4, 4, -4, 4, 1, 5)),
                 0.0f, 0.0f, 0.0f, false, 3.0, 60},
                {"swim_forward_in_still_lava", concat(plate(STONE, -4, 4, -4, 12),
                     pool(LAVA, -4, 4, -4, 12, 1, 5)),
                 0.0f, 0.0f, 1.0f, false, 3.0, 60},
                {"a_puddle_is_not_deep_enough_to_swim_in",
                 concat(plate(STONE, -4, 4, -4, 12), pool(WATER, -4, 4, -4, 12, 1, 1)),
                 0.0f, 0.0f, 1.0f, false, 0.0, 40},
            };

            // Two origins, so every case is run once in positive coordinates
            // and once across the negative axis -- the place a floor divide
            // that should be an arithmetic shift goes wrong by a whole chunk.
            int[][] origins = {{8, 70, 8}, {-317, 70, -404}};

            p("// Generated by tools/genref.java --jar <client.jar> --player <scratch-dir>.");
            p("// Do not edit by hand.");
            p("//");
            p("// **A real a1.1.2 EntityPlayer, ticked.** `dm` is concrete, takes only a World,");
            p("// and overrides neither moveEntity nor moveEntityWithHeading, so these are the");
            p("// game's own numbers rather than a stand-in's. Every case runs twice: once in");
            p("// positive coordinates and once across the negative axis, which is where a");
            p("// floor divide that should have been an arithmetic shift goes wrong by a whole");
            p("// chunk.");
            p("//");
            p("// Each scene is a list of blocks placed into a volume that was cleared to air,");
            p("// so the harness can rebuild the world exactly rather than reimplementing a");
            p("// scene builder. Positions are relative to the case's origin, whose y is the");
            p("// level the plate's top surface sits at. The clear runs deep -- a player who");
            p("// steps off a ledge falls tens of blocks, and generated terrain below a");
            p("// shallow shell would catch them here and not in the harness.");
            p("//");
            p("// **Yaw 0 faces +Z**, so the walkways run long in z.");
            p("//");
            p("// Doubles are raw bit patterns. Unlike the collision boxes, almost nothing");
            p("// here is exact in decimal -- gravity compounds through a drag of");
            p("// 0.9800000190734863 -- and a decimal round trip could hide exactly the");
            p("// one-ulp disagreement this fixture exists to catch.");
            p("//");
            p("// **Sneaking is not covered.** Entity.isSneaking is a hardcoded false and only");
            p("// the client-side player class overrides it, so an EntityPlayer cannot be made");
            p("// to sneak from outside the game. The ledge walk-back is tested by hand.");
            p("// Water and lava are likewise absent; the C++ side implements the land branch.");
            p("//");
            p("// Regenerate with:");
            p("//   java tools/genref.java --jar <client.jar> --player /tmp/genref-scratch");
            p("//     redirected to tests/player_body_vectors.hpp");
            p("");
            p("#pragma once");
            p("");
            p("#include \"core/util/types.hpp\"");
            p("");
            p("namespace mc::test {");
            p("");
            p("// The dimensions the emitter read back off a constructed EntityPlayer, so the");
            p("// constants in player_body.hpp are checked against the jar and not just against");
            p("// the prose in docs/physics-a1.1.2.md.");
            {
                Object probe = playerCtor.newInstance(
                    worldCtor.newInstance(new java.io.File(dir, "probe"), "genref", 1L));
                p("inline constexpr float kPlayerYOffset = " + yOffset.getFloat(probe) + "f;");
                p("inline constexpr float kPlayerBoxWidth = " + widthField.getFloat(probe) + "f;");
                p("inline constexpr float kPlayerBoxHeight = " + heightField.getFloat(probe) + "f;");
                p("inline constexpr float kPlayerStepHeight = " + stepHeight.getFloat(probe) + "f;");
            }
            p("");
            p("struct ScenePlacement {");
            p("    i32 dx;");
            p("    int dy;");
            p("    i32 dz;");
            p("    u16 id;");
            p("    u8 metadata;");
            p("};");
            p("");
            p("// One tick's worth of state, straight off the entity's fields.");
            p("struct PlayerBodyStep {");
            p("    u64 posX, posY, posZ;        // the original's posY -- the eye");
            p("    u64 motionX, motionY, motionZ;");
            p("    u64 boxMinY;                 // the feet");
            p("    u32 ySize, fallDistance;");
            p("    bool onGround, collidedHorizontally, collidedVertically;");
            p("};");
            p("");
            p("struct PlayerBodyCase {");
            p("    const char* name;");
            p("    i32 originX;");
            p("    int originY;");
            p("    i32 originZ;");
            p("    int placementCount;");
            p("    const ScenePlacement* placements;");
            p("    float yawDegrees, strafe, forward;");
            p("    bool jump;");
            p("    u64 startFeetAbove;          // blocks above the plate's top surface");
            p("    int stepCount;");
            p("    const PlayerBodyStep* steps;");
            p("};");
            p("");

            StringBuilder table = new StringBuilder();
            int emitted = 0;
            int groundedSteps = 0;
            int airborneSteps = 0;

            for (int c = 0; c < cases.length; c++) {
                final String name = (String) cases[c][0];
                final int[][] scene = (int[][]) cases[c][1];
                final float yaw = ((Float) cases[c][2]).floatValue();
                final float strafe = ((Float) cases[c][3]).floatValue();
                final float forward = ((Float) cases[c][4]).floatValue();
                final boolean jumping = ((Boolean) cases[c][5]).booleanValue();
                final double startAbove = ((Double) cases[c][6]).doubleValue();
                final int steps = ((Integer) cases[c][7]).intValue();

                for (int o = 0; o < origins.length; o++) {
                    final int ox = origins[o][0];
                    final int oy = origins[o][1];
                    final int oz = origins[o][2];

                    java.io.File wdir = new java.io.File(dir, "w" + c + "_" + o);
                    deleteTree(wdir);
                    wdir.mkdirs();
                    Object world = worldCtor.newInstance(wdir, "genref", 1234567890L);
                    for (int cx = (ox >> 4) - 2; cx <= (ox >> 4) + 2; cx++) {
                        for (int cz = (oz >> 4) - 2; cz <= (oz >> 4) + 2; cz++) {
                            getChunk.invoke(world, cx, cz);
                        }
                    }

                    // Clear the play volume to air. The harness starts from air,
                    // so this is what makes the two worlds the same world.
                    // Deep, not just around the scene: a player who walks off
                    // a ledge falls tens of blocks, and generated terrain under
                    // the cleared shell would catch them here and nowhere else.
                    for (int dx = -14; dx <= 14; dx++) {
                        for (int dz = -14; dz <= 14; dz++) {
                            for (int dy = -45; dy <= 24; dy++) {
                                setBlockAndData.invoke(world, ox + dx, oy + dy, oz + dz, 0, 0);
                            }
                        }
                    }
                    for (int[] b : scene) {
                        setBlockAndData.invoke(world, ox + b[0], oy + b[1], oz + b[2], b[3], b[4]);
                    }

                    Object player = playerCtor.newInstance(world);
                    // The plate's top surface is at oy + 1, so feet stand there.
                    final double feet = (double) oy + 1.0 + startAbove;
                    setPosition.invoke(player, (double) ox + 0.5,
                                       feet + (double) yOffset.getFloat(player),
                                       (double) oz + 0.5);
                    motionX.setDouble(player, 0.0);
                    motionY.setDouble(player, 0.0);
                    motionZ.setDouble(player, 0.0);
                    onGround.setBoolean(player, false);
                    ySize.setFloat(player, 0.0f);
                    fallDistance.setFloat(player, 0.0f);
                    yawField.setFloat(player, yaw);

                    StringBuilder rows = new StringBuilder();
                    for (int s = 0; s < steps; s++) {
                        // `ge.j()` -- the jump branch of onLivingUpdate, and
                        // it is three-way rather than one. **Holding the
                        // button in a liquid does not jump**: water and lava
                        // each add a flat 0.03999999910593033 to motionY and
                        // the jump is never reached, which is what swimming up
                        // is. Only the third branch, on the ground and in
                        // neither liquid, calls `C()`.
                        if (jumping) {
                            if (((Boolean) inWaterMethod.invoke(player)).booleanValue()
                                || ((Boolean) inLavaMethod.invoke(player)).booleanValue()) {
                                motionY.setDouble(player,
                                                  motionY.getDouble(player)
                                                      + 0.03999999910593033D);
                            } else if (onGround.getBoolean(player)) {
                                jumpMethod.invoke(player);
                            }
                        }
                        moveWithHeading.invoke(player, strafe, forward);

                        Object bb = boxField.get(player);
                        rows.append("    {")
                            .append(dbits(posX.getDouble(player))).append(", ")
                            .append(dbits(posY.getDouble(player))).append(", ")
                            .append(dbits(posZ.getDouble(player))).append(", ")
                            .append(dbits(motionX.getDouble(player))).append(", ")
                            .append(dbits(motionY.getDouble(player))).append(", ")
                            .append(dbits(motionZ.getDouble(player))).append(", ")
                            .append(dbits(boxMinY.getDouble(bb))).append(", ")
                            .append(fbits(ySize.getFloat(player))).append(", ")
                            .append(fbits(fallDistance.getFloat(player))).append(", ")
                            .append(onGround.getBoolean(player)).append(", ")
                            .append(collidedH.getBoolean(player)).append(", ")
                            .append(collidedV.getBoolean(player))
                            .append("},\n");
                        if (onGround.getBoolean(player)) {
                            groundedSteps++;
                        } else {
                            airborneSteps++;
                        }
                    }

                    final String tag = name + (o == 0 ? "" : "_negative");
                    p("inline constexpr ScenePlacement kScene_" + tag + "["
                      + Math.max(scene.length, 1) + "] = {");
                    if (scene.length == 0) {
                        p("    {0, 0, 0, 0, 0},");
                    }
                    for (int[] b : scene) {
                        p("    {" + b[0] + ", " + b[1] + ", " + b[2] + ", " + b[3] + ", " + b[4] + "},");
                    }
                    p("};");
                    p("");
                    p("inline constexpr PlayerBodyStep kSteps_" + tag + "[" + steps + "] = {");
                    System.out.print(rows);
                    p("};");
                    p("");

                    table.append("    {\"").append(tag).append("\", ")
                         .append(ox).append(", ").append(oy).append(", ").append(oz).append(", ")
                         .append(scene.length).append(", kScene_").append(tag).append(", ")
                         .append(yaw).append("f, ").append(strafe).append("f, ")
                         .append(forward).append("f, ").append(jumping).append(", ")
                         .append(dbits(startAbove)).append(", ")
                         .append(steps).append(", kSteps_").append(tag).append("},\n");
                    emitted++;
                    deleteTree(wdir);
                }
            }

            p("inline constexpr int kPlayerBodyCaseCount = " + emitted + ";");
            p("");
            p("// Negative controls. A body that never left the ground, or never touched it,");
            p("// would satisfy a run that only compared positions in one of those states.");
            p("inline constexpr int kPlayerBodyGroundedSteps = " + groundedSteps + ";");
            p("inline constexpr int kPlayerBodyAirborneSteps = " + airborneSteps + ";");
            p("");
            p("inline constexpr PlayerBodyCase kPlayerBodyCases[kPlayerBodyCaseCount] = {");
            System.out.print(table);
            p("};");
            p("");
            p("}  // namespace mc::test");
        } catch (Exception e) {
            System.err.println("genref --player failed: " + e);
            e.printStackTrace();
            System.exit(1);
        } finally {
            deleteTree(dir);
        }
    }

    // ---------------------------------------------------------------------
    // What the crosshair is on -- `cn.a(aj,aj,Z)` (rayTraceBlocks) driving
    // `ly.a(cn,III,aj,aj)` (collisionRayTrace)
    // ---------------------------------------------------------------------
    //
    // A scene with one of everything awkward in it -- a slab, stairs, a wall
    // torch, a fence, a ladder, a door, a pane of water -- and several hundred
    // rays swept across it from four eye positions.
    //
    // **The scene is emitted as a readback, not as what was asked for.** Some
    // blocks rewrite their own metadata when placed (a torch put on a floor
    // becomes metadata 5 whatever it was told) and some delete themselves
    // outright, so what the fixture records is every non-air block the world
    // actually held afterwards. That way the harness rebuilds the same world
    // instead of the same intentions.
    //
    // **Directions are emitted rather than yaw and pitch.** This is a test of
    // the ray walk, not of getLook; feeding both sides the same vector keeps
    // one from covering for the other.

    private static void emitRayTrace(String jarPath, String scratchDir) {
        java.io.File dir = new java.io.File(scratchDir, "raytrace");
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> worldClass = loader.loadClass(WORLD);
            Class<?> vecClass = loader.loadClass("aj");
            Class<?> hitClass = loader.loadClass("mf");
            Class<?> blockClass = loader.loadClass("ly");
            Object[] blocks = (Object[]) blockClass.getField("n").get(null);

            java.lang.reflect.Constructor<?> worldCtor =
                worldClass.getConstructor(java.io.File.class, String.class, long.class);
            java.lang.reflect.Method getChunk = worldClass.getMethod("b", int.class, int.class);
            java.lang.reflect.Method setBlockAndData = worldClass.getMethod(
                "a", int.class, int.class, int.class, int.class, int.class);
            java.lang.reflect.Method getId = worldClass.getMethod(
                "a", int.class, int.class, int.class);
            java.lang.reflect.Method getMeta = worldClass.getMethod(
                "e", int.class, int.class, int.class);
            java.lang.reflect.Method rayTrace = worldClass.getMethod("a", vecClass, vecClass);
            java.lang.reflect.Method makeVec = vecClass.getMethod(
                "a", double.class, double.class, double.class);

            java.lang.reflect.Field hitX = entityField(hitClass, "b");
            java.lang.reflect.Field hitY = entityField(hitClass, "c");
            java.lang.reflect.Field hitZ = entityField(hitClass, "d");
            java.lang.reflect.Field hitSide = entityField(hitClass, "e");
            java.lang.reflect.Field hitVec = entityField(hitClass, "f");
            java.lang.reflect.Field vecX = entityField(vecClass, "a");
            java.lang.reflect.Field vecY = entityField(vecClass, "b");
            java.lang.reflect.Field vecZ = entityField(vecClass, "c");

            deleteTree(dir);
            dir.mkdirs();
            Object world = worldCtor.newInstance(dir, "genref", 1234567890L);

            final int ox = 8;
            final int oy = 70;
            final int oz = 8;
            for (int cx = (ox >> 4) - 1; cx <= (ox >> 4) + 1; cx++) {
                for (int cz = (oz >> 4) - 1; cz <= (oz >> 4) + 1; cz++) {
                    getChunk.invoke(world, cx, cz);
                }
            }
            for (int dx = -10; dx <= 10; dx++) {
                for (int dz = -10; dz <= 10; dz++) {
                    for (int dy = -3; dy <= 10; dy++) {
                        setBlockAndData.invoke(world, ox + dx, oy + dy, oz + dz, 0, 0);
                    }
                }
            }

            // A floor, and then one of everything with an interesting shape.
            for (int dx = -6; dx <= 6; dx++) {
                for (int dz = -6; dz <= 6; dz++) {
                    setBlockAndData.invoke(world, ox + dx, oy, oz + dz, 1, 0);
                }
            }
            setBlockAndData.invoke(world, ox + 0, oy + 1, oz + 2, 44, 0);   // slab
            setBlockAndData.invoke(world, ox + 1, oy + 1, oz + 2, 53, 0);   // wooden stairs
            setBlockAndData.invoke(world, ox + 2, oy + 1, oz + 2, 53, 2);   // and another facing
            setBlockAndData.invoke(world, ox - 1, oy + 1, oz + 2, 85, 0);   // fence
            setBlockAndData.invoke(world, ox - 2, oy + 1, oz + 2, 50, 0);   // torch on the floor
            setBlockAndData.invoke(world, ox + 3, oy + 1, oz + 2, 1, 0);    // a wall to hang things on
            setBlockAndData.invoke(world, ox + 3, oy + 2, oz + 2, 1, 0);
            setBlockAndData.invoke(world, ox + 3, oy + 1, oz + 1, 50, 2);   // wall torch
            setBlockAndData.invoke(world, ox + 3, oy + 2, oz + 1, 65, 4);   // ladder
            setBlockAndData.invoke(world, ox - 3, oy + 1, oz + 2, 6, 0);    // sapling
            setBlockAndData.invoke(world, ox - 4, oy + 1, oz + 2, 78, 0);   // snow layer
            setBlockAndData.invoke(world, ox + 0, oy + 1, oz + 4, 8, 0);    // water: not targetable
            setBlockAndData.invoke(world, ox + 1, oy + 1, oz + 4, 20, 0);   // glass
            setBlockAndData.invoke(world, ox + 2, oy + 1, oz + 4, 64, 1);   // door

            // Read the world back, so the harness rebuilds what is there rather
            // than what was asked for.
            StringBuilder scene = new StringBuilder();
            int placements = 0;
            for (int dx = -10; dx <= 10; dx++) {
                for (int dz = -10; dz <= 10; dz++) {
                    for (int dy = -3; dy <= 10; dy++) {
                        final int id = ((Integer) getId.invoke(world, ox + dx, oy + dy, oz + dz))
                                           .intValue();
                        if (id == 0) {
                            continue;
                        }
                        final int meta =
                            ((Integer) getMeta.invoke(world, ox + dx, oy + dy, oz + dz)).intValue();
                        scene.append("    {").append(dx).append(", ").append(dy).append(", ")
                             .append(dz).append(", ").append(id).append(", ").append(meta)
                             .append("},\n");
                        placements++;
                    }
                }
            }

            // **Warm every block singleton the way a played frame leaves it.**
            //
            // `Block.collisionRayTrace` tests the ray against the block's own
            // `bf..bk` fields, and three blocks in a1.1.2 never write those on
            // the ray's path: the ladder, the cactus and the staircase set
            // their bounds in `getCollisionBoundingBoxFromPool` and
            // `getSelectedBoundingBoxFromPool` instead. On a world built two
            // statements ago those fields still hold the constructor's default
            // -- so a cold sweep records **a ladder as a full cube**, which is
            // not what any running client does: `RenderGlobal.drawSelectionBox`
            // calls `f` on the targeted block every frame, and
            // `Entity.moveEntity` calls `d` on every block the body overlaps
            // every tick, and both leave the real box behind. A player is
            // therefore looking at the two-sixteenths box from the first frame
            // after the first time anything touched that ladder, which is
            // always.
            //
            // Priming with `f` over the scene puts the singletons in that state
            // before the sweep rather than recording the litter of an
            // untouched JVM. It is deterministic here because the scene holds
            // one of each -- in a world with two ladders facing different ways
            // the original genuinely answers with whichever was asked last,
            // which is a bug this port does not reproduce. See
            // docs/physics-a1.1.2.md.
            java.lang.reflect.Method blockSelectedBox = blockClass.getMethod(
                "f", worldClass, int.class, int.class, int.class);
            for (int dx = -10; dx <= 10; dx++) {
                for (int dz = -10; dz <= 10; dz++) {
                    for (int dy = -3; dy <= 10; dy++) {
                        final int id = ((Integer) getId.invoke(world, ox + dx, oy + dy, oz + dz))
                                           .intValue();
                        if (id == 0 || blocks[id] == null) {
                            continue;
                        }
                        blockSelectedBox.invoke(blocks[id], world, ox + dx, oy + dy, oz + dz);
                    }
                }
            }

            // **Within reach of the awkward blocks, not merely in the same
            // room.** The first attempt put the eyes four and a half blocks
            // from the row of interesting geometry -- past a reach of four --
            // and the whole sweep hit the floor and reported three faces.
            final double[][] eyes = {
                {ox + 0.5, oy + 2.62, oz + 0.5},   // facing the slab and stairs
                {ox - 2.5, oy + 2.62, oz + 0.5},   // beside the torch and the fence
                {ox + 3.5, oy + 2.62, oz - 0.5},   // under the wall torch and ladder
                {ox + 1.5, oy + 2.62, oz + 2.5},   // standing among them
                {ox + 1.5, oy + 3.62, oz + 3.5},   // above the water, glass and door
                {ox - 3.5, oy + 2.62, oz + 3.5},   // looking back across the lot
            };

            StringBuilder rows = new StringBuilder();
            int cast = 0;
            int hits = 0;
            java.util.HashSet<Integer> facesSeen = new java.util.HashSet<Integer>();
            java.util.HashSet<Integer> blocksSeen = new java.util.HashSet<Integer>();

            for (double[] eye : eyes) {
                for (int yaw = 0; yaw < 360; yaw += 7) {
                    for (int pitch = -75; pitch <= 75; pitch += 13) {
                        final double ry = Math.toRadians(yaw);
                        final double rp = Math.toRadians(pitch);
                        final double dx = -Math.sin(ry) * Math.cos(rp);
                        final double dy = -Math.sin(rp);
                        final double dz = Math.cos(ry) * Math.cos(rp);

                        Object v1 = makeVec.invoke(null, eye[0], eye[1], eye[2]);
                        Object v2 = makeVec.invoke(null, eye[0] + dx * 4.0, eye[1] + dy * 4.0,
                                                   eye[2] + dz * 4.0);
                        Object hit = rayTrace.invoke(world, v1, v2);

                        rows.append("    {")
                            .append(dbits(eye[0])).append(", ").append(dbits(eye[1])).append(", ")
                            .append(dbits(eye[2])).append(", ")
                            .append(dbits(dx)).append(", ").append(dbits(dy)).append(", ")
                            .append(dbits(dz)).append(", ");
                        if (hit == null) {
                            rows.append("false, 0, 0, 0, 0, 0x0ULL, 0x0ULL, 0x0ULL},\n");
                        } else {
                            Object v = hitVec.get(hit);
                            final int side = hitSide.getInt(hit);
                            rows.append("true, ")
                                .append(hitX.getInt(hit)).append(", ")
                                .append(hitY.getInt(hit)).append(", ")
                                .append(hitZ.getInt(hit)).append(", ")
                                .append(side).append(", ")
                                .append(dbits(vecX.getDouble(v))).append(", ")
                                .append(dbits(vecY.getDouble(v))).append(", ")
                                .append(dbits(vecZ.getDouble(v))).append("},\n");
                            hits++;
                            facesSeen.add(Integer.valueOf(side));
                            blocksSeen.add((Integer) getId.invoke(world, hitX.getInt(hit),
                                                                 hitY.getInt(hit),
                                                                 hitZ.getInt(hit)));
                        }
                        cast++;
                    }
                }
            }

            p("// Generated by tools/genref.java --jar <client.jar> --raytrace <scratch-dir>.");
            p("// Do not edit by hand.");
            p("//");
            p("// **A real a1.1.2 World, ray-traced.** A floor with one of everything awkward");
            p("// standing on it -- a slab, two stairs facing different ways, a fence, a torch on");
            p("// the floor and another on a wall, a ladder, a sapling, a snow layer, glass, a");
            p("// door and a block of water -- swept by several hundred rays from four eye");
            p("// positions.");
            p("//");
            p("// The scene is a **readback**: some blocks rewrite their own metadata when they");
            p("// are placed and some delete themselves, so what is recorded is every non-air");
            p("// block the world actually held, not what it was told to hold.");
            p("//");
            p("// Directions are given rather than yaw and pitch, because this is a test of the");
            p("// ray walk and not of getLook -- feeding both sides the same vector stops one");
            p("// from covering for the other. `side` is the original's sideHit, which is the");
            p("// same numbering as mc::mesh::Face.");
            p("//");
            p("// Regenerate with:");
            p("//   java tools/genref.java --jar <client.jar> --raytrace /tmp/genref-scratch");
            p("//     redirected to tests/ray_trace_vectors.hpp");
            p("");
            p("#pragma once");
            p("");
            p("#include \"core/util/types.hpp\"");
            p("");
            p("namespace mc::test {");
            p("");
            p("struct RayScenePlacement {");
            p("    i32 dx;");
            p("    int dy;");
            p("    i32 dz;");
            p("    u16 id;");
            p("    u8 metadata;");
            p("};");
            p("");
            p("struct RayCase {");
            p("    u64 eyeX, eyeY, eyeZ;");
            p("    u64 dirX, dirY, dirZ;");
            p("    bool hit;");
            p("    i32 blockX;");
            p("    int blockY;");
            p("    i32 blockZ;");
            p("    int side;");
            p("    u64 hitX, hitY, hitZ;");
            p("};");
            p("");
            p("inline constexpr i32 kRayOriginX = " + ox + ";");
            p("inline constexpr int kRayOriginY = " + oy + ";");
            p("inline constexpr i32 kRayOriginZ = " + oz + ";");
            p("");
            p("inline constexpr int kRayScenePlacementCount = " + placements + ";");
            p("");
            p("inline constexpr RayScenePlacement kRayScene[kRayScenePlacementCount] = {");
            System.out.print(scene);
            p("};");
            p("");
            p("inline constexpr int kRayCaseCount = " + cast + ";");
            p("");
            p("// Negative controls: a walk that hit nothing, or that only ever hit the floor");
            p("// from directly above, would satisfy a comparison that only checked misses.");
            p("inline constexpr int kRayHitCount = " + hits + ";");
            p("inline constexpr int kRayDistinctFaces = " + facesSeen.size() + ";");
            p("inline constexpr int kRayDistinctBlocks = " + blocksSeen.size() + ";");
            p("");
            p("inline constexpr RayCase kRayCases[kRayCaseCount] = {");
            System.out.print(rows);
            p("};");
            p("");
            p("}  // namespace mc::test");
        } catch (Exception e) {
            System.err.println("genref --raytrace failed: " + e);
            e.printStackTrace();
            System.exit(1);
        } finally {
            deleteTree(dir);
        }
    }

    // ---------------------------------------------------------------------
    // Which way a placed block ends up facing
    // ---------------------------------------------------------------------
    //
    // The sequence a1.1.2 actually runs, found by following the right-click
    // from the controller down:
    //
    //   av.a(...)  ItemBlock.onItemUse   -> world.setBlockWithNotify(x, y, z, id)
    //                                    -> block.onBlockPlaced(world, x, y, z, side)
    //   ia/nj      the controller        -> block.onBlockPlacedBy(world, x, y, z, player)
    //
    // **ItemBlock does not call onBlockPlacedBy** -- the controller does, after
    // onItemUse has returned true. Six blocks override the side hook and eight
    // declare the player one, so this sweeps every block against every face at
    // eight player headings and records what metadata the world is left
    // holding. Whatever does not vary, does not vary, and the table says so.
    //
    // The clicked block is a stone cube and the target is the cell on the
    // struck face, which is the ordinary case; a placement that needs a
    // different support answers with whatever the game leaves behind, including
    // deleting itself, and the fixture records that too.

    private static void emitPlacement(String jarPath, String scratchDir) {
        java.io.File dir = new java.io.File(scratchDir, "placement");
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> blockClass = loader.loadClass("ly");
            Class<?> worldClass = loader.loadClass(WORLD);
            Class<?> playerClass = loader.loadClass("dm");
            Class<?> entityClass = loader.loadClass("kh");

            Object[] blocks = (Object[]) blockClass.getField("n").get(null);

            Object world = worldClass.getConstructor(java.io.File.class, String.class, long.class)
                               .newInstance(dir, "genref", 1234567890L);
            java.lang.reflect.Method getChunk = worldClass.getMethod("b", int.class, int.class);
            getChunk.invoke(world, 0, 0);

            java.lang.reflect.Method setRaw = worldClass.getMethod(
                "a", int.class, int.class, int.class, int.class, int.class);
            // setBlockWithNotify(x, y, z, id) -- what ItemBlock uses.
            java.lang.reflect.Method setWithNotify = worldClass.getMethod(
                "d", int.class, int.class, int.class, int.class);
            java.lang.reflect.Method getId = worldClass.getMethod(
                "a", int.class, int.class, int.class);
            java.lang.reflect.Method getMeta = worldClass.getMethod(
                "e", int.class, int.class, int.class);
            java.lang.reflect.Method onPlaced = blockClass.getMethod(
                "d", worldClass, int.class, int.class, int.class, int.class);
            // **`ly.b(Lcn;IIILdm;)V` is `onBlockClicked`, and it is not part of
            // placing anything.** This sweep used to call it here, on the theory
            // that a1.1.2 had an `onBlockPlacedBy` the controller ran after
            // `ItemBlock.onItemUse`. It does not: `Block` takes an `EntityPlayer`
            // in exactly three methods, and that one is reached from
            // `PlayerController.clickBlock` -- the *left* button, the start of
            // mining. Calling it after each placement punched every block the
            // sweep had just put down, and the lever noticed: a punch flips it,
            // so every lever row came out with bit 3 set and the shipped table
            // placed levers already switched on.
            java.lang.reflect.Field worldRandom = worldClass.getField("n");

            Object player = playerClass.getConstructor(worldClass).newInstance(world);
            java.lang.reflect.Method setPosition = entityClass.getMethod(
                "a", double.class, double.class, double.class);
            java.lang.reflect.Field yawField = entityField(entityClass, "aq");

            // The clicked block, and the six cells around it.
            final int cx = 8;
            final int cy = 70;
            final int cz = 8;
            final int[][] offset = {
                {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0},
            };
            // **Sixteen, not eight.** Eight lands every sample exactly on a
            // quadrant boundary, which is the worst place to ask a rounding
            // question: the first sweep reported the lever changing at 225
            // degrees and nowhere else, which is a sampling artefact rather
            // than a rule.
            final float[] yaws = new float[16];
            for (int i = 0; i < yaws.length; i++) {
                yaws[i] = i * 22.5f;
            }

            StringBuilder rows = new StringBuilder();
            int cases = 0;
            int oriented = 0;
            java.util.TreeSet<Integer> sideVaries = new java.util.TreeSet<Integer>();
            java.util.TreeSet<Integer> yawVaries = new java.util.TreeSet<Integer>();
            // **Which blocks have an onBlockPlaced at all.** The sweep sees the
            // sum of onBlockAdded and onBlockPlaced, and with one stone cube
            // for a neighbour the two can look alike: a furnace turns its
            // mouth away from the stone in onBlockAdded, and so appears to
            // "take the struck face" when it takes nothing of the kind. Asked
            // of the class rather than inferred from the results.
            java.util.TreeSet<Integer> hooked = new java.util.TreeSet<Integer>();

            for (int id = 0; id < 256; id++) {
                if (id >= blocks.length || blocks[id] == null) {
                    continue;
                }
                if (blocks[id].getClass().getMethod("d", worldClass, int.class, int.class,
                        int.class, int.class).getDeclaringClass() != blockClass) {
                    hooked.add(Integer.valueOf(id));
                }
                int[][] result = new int[6][yaws.length];
                for (int side = 0; side < 6; side++) {
                    for (int y = 0; y < yaws.length; y++) {
                        // A fresh stone cube to click on, and empty space all
                        // round it, every time.
                        for (int dx = -2; dx <= 2; dx++) {
                            for (int dy = -2; dy <= 2; dy++) {
                                for (int dz = -2; dz <= 2; dz++) {
                                    setRaw.invoke(world, cx + dx, cy + dy, cz + dz, 0, 0);
                                }
                            }
                        }
                        setRaw.invoke(world, cx, cy, cz, 1, 0);

                        final int tx = cx + offset[side][0];
                        final int ty = cy + offset[side][1];
                        final int tz = cz + offset[side][2];

                        setPosition.invoke(player, cx + 0.5, cy + 3.0, cz + 0.5);
                        yawField.setFloat(player, yaws[y]);

                        // **The world's Random, reset before every case.** One
                        // block reaches for it while being placed --
                        // `BlockLever.onBlockAdded` writes `5 + nextInt(2)`, which
                        // is which way round a floor lever's handle lies -- and an
                        // unseeded sweep therefore reports that block as varying
                        // for reasons that have nothing to do with the case being
                        // swept. Reseeding makes the table a function of (id, face)
                        // again, which is the shape `placementMetadata` has.
                        worldRandom.set(world, new Random(1234567890L));

                        setWithNotify.invoke(world, tx, ty, tz, id);
                        onPlaced.invoke(blocks[id], world, tx, ty, tz, side);

                        final int landedId = ((Integer) getId.invoke(world, tx, ty, tz)).intValue();
                        result[side][y] = landedId == id
                            ? ((Integer) getMeta.invoke(world, tx, ty, tz)).intValue()
                            : -1;
                        cases++;
                    }
                }

                boolean varies = false;
                for (int side = 0; side < 6; side++) {
                    for (int y = 0; y < yaws.length; y++) {
                        if (result[side][y] != result[0][0]) {
                            varies = true;
                        }
                        if (result[side][y] != result[side][0]) {
                            yawVaries.add(Integer.valueOf(id));
                        }
                        if (result[side][y] != result[0][y]) {
                            sideVaries.add(Integer.valueOf(id));
                        }
                    }
                }
                if (varies) {
                    oriented++;
                }

                rows.append("    {").append(id).append(", {");
                for (int side = 0; side < 6; side++) {
                    rows.append(side == 0 ? "" : ", ").append("{");
                    for (int y = 0; y < yaws.length; y++) {
                        rows.append(y == 0 ? "" : ", ").append(result[side][y]);
                    }
                    rows.append("}");
                }
                rows.append("}},   // ").append("\n");
            }

            p("// Generated by tools/genref.java --jar <client.jar> --place <scratch-dir>.");
            p("// Do not edit by hand.");
            p("//");
            p("// **What metadata a block ends up with when a player places it**, swept over every");
            p("// face of a clicked stone cube and sixteen player headings.");
            p("//");
            p("// The sequence is the one a1.1.2 runs, found by following a right-click down:");
            p("// ItemBlock.onItemUse does setBlockWithNotify -- which runs onBlockAdded -- and");
            p("// then onBlockPlaced(side). That is the whole of it. a1.1.2 has no");
            p("// onBlockPlacedBy: Block takes an EntityPlayer in three methods and none of");
            p("// them is on the placement path, so **nothing in this version orients from the");
            p("// player's heading**. (ItemDoor is the one thing that does, and it reads the");
            p("// heading in the *item*, never in the block.)");
            p("//");
            p("// The world's Random is reseeded before every case, because BlockLever.");
            p("// onBlockAdded rolls nextInt(2) for which way a floor lever lies and an");
            p("// unseeded sweep reports that as variation.");
            p("//");
            p("// -1 means the block did not survive being placed there at all: it deleted");
            p("// itself, the way a torch does with nothing to hang on.");
            p("//");
            p("// side is mc::mesh::Face: 0 -Y, 1 +Y, 2 -Z, 3 +Z, 4 -X, 5 +X. Headings are");
            p("// 0, 45, 90 ... 315 degrees.");
            p("//");
            p("// Regenerate with:");
            p("//   java tools/genref.java --jar <client.jar> --place /tmp/genref-scratch");
            p("//     redirected to tests/placement_vectors.hpp");
            p("");
            p("#pragma once");
            p("");
            p("#include \"core/util/types.hpp\"");
            p("");
            p("namespace mc::test {");
            p("");
            p("inline constexpr int kPlacementHeadingCount = " + yaws.length + ";");
            StringBuilder headings = new StringBuilder();
            for (int i = 0; i < yaws.length; i++) {
                headings.append(i == 0 ? "" : ", ").append(yaws[i]).append("f");
            }
            p("inline constexpr float kPlacementHeadings[kPlacementHeadingCount] = {");
            p("    " + headings + ",");
            p("};");
            p("");
            p("struct PlacementCase {");
            p("    u16 id;");
            p("    int metadata[6][kPlacementHeadingCount];   // -1 if it did not survive");
            p("};");
            p("");
            p("inline constexpr int kPlacementCaseCount = " + (cases / (6 * yaws.length)) + ";");
            p("");
            p("// Blocks whose result depends on the face struck, and on the player's heading.");
            p("// Both lists are short, and a table that came out empty would mean the sweep was");
            p("// not doing anything.");
            p("inline constexpr int kPlacementSideVaryingCount = " + sideVaries.size() + ";");
            p("inline constexpr int kPlacementYawVaryingCount = " + yawVaries.size() + ";");
            p("");
            p("inline constexpr PlacementCase kPlacementCases[kPlacementCaseCount] = {");
            System.out.print(rows);
            p("};");
            p("");
            p("// Blocks whose class overrides onBlockPlaced (`ly.d(Lcn;IIII)V`). For every");
            p("// other block the metadata above is onBlockAdded's alone -- a furnace or a");
            p("// staircase reading its neighbours, which in this sweep are the one stone cube");
            p("// -- and onBlockPlaced leaves it as it found it.");
            StringBuilder hookList = new StringBuilder();
            for (Integer id : hooked) {
                hookList.append(hookList.length() == 0 ? "" : ", ").append(id);
            }
            p("inline constexpr int kPlacementHookCount = " + hooked.size() + ";");
            p("inline constexpr u16 kPlacementHooks[kPlacementHookCount] = {" + hookList + "};");
            p("");
            p("}  // namespace mc::test");

            System.err.println("side-varying ids: " + sideVaries);
            System.err.println("yaw-varying ids:  " + yawVaries);
            System.err.println("onBlockPlaced:    " + hooked);
        } catch (Exception e) {
            System.err.println("genref --place failed: " + e);
            e.printStackTrace();
            System.exit(1);
        } finally {
            deleteTree(dir);
        }
    }

    private static void p(String line) {
        System.out.println(line);
    }

    // ---------------------------------------------------------------------
    // The item table
    // ---------------------------------------------------------------------
    //
    // Emits data/<version>/items.json: every entry of `Item.itemsList`, with
    // the columns the engine actually needs -- the icon it draws, the sheet
    // that icon is on, how high it stacks, how much damage it takes, and which
    // block it puts into the world.
    //
    // **The last column is measured rather than read.** Three of the classes
    // that place a block do not name it in a field: ItemRedstone, ItemSign and
    // ItemDoor decide inside `onItemUse`, and ItemDoor's answer depends on the
    // material it was constructed with. Rather than pattern-match three
    // bytecodes, this builds a real World, stands a real EntityPlayer in it and
    // uses each item on the top face of a real block, then reads back what
    // appeared. That is the same "run it, do not guess it" arrangement as
    // --collision and --player, and it is why doors and sugar cane come out
    // right without anything here knowing what a door is.
    //
    // Five grounds are tried in turn because placement has preconditions: seeds
    // want farmland, cactus wants sand, and reeds want sand or dirt with water
    // beside it. Five faces are tried for the same reason -- a ladder and a
    // wall torch go on the *side* of a block and refuse the top -- and the top
    // face is tried first so a block that would take either is recorded the way
    // a player would place it. The first combination that produces a block
    // wins, and an item that places nothing on any of them -- a sword, an
    // ingot -- reports 0.
    //
    // **A bucket reports 0 and that is correct.** `ItemBucket` does its work in
    // `onItemRightClick` off a ray trace rather than in `onItemUse`, so there
    // is no face to hand it and nothing here to measure. It is a real gap in
    // what a Creative hand can do and it is named rather than papered over.
    //
    // **The sheet is a1.1.2's own rule and not a guess.** `RenderItem` picks
    // between terrain.png and gui/items.png on `itemID < 256`, which is exactly
    // the boundary between the ItemBlocks the Item static initialiser builds
    // for every block and the items declared after them.
    //
    // **Names are ours**, exactly as they are in blocks.json, and for the same
    // reason: a1.1.2 predates `setItemName` and its Item class carries no
    // strings at all. They are the one column this tool cannot check. Ids below
    // 256 take the block's name straight out of blocks.json rather than
    // repeating it here, so the two files cannot drift.
    //
    // **The palette column, which is the one place a judgement is recorded.**
    // `palette` says whether an item is offered in the Creative hand. Two rules
    // decide it and only the second is invented:
    //
    //   1. **Derived.** An ItemBlock is hidden when some item above 255 places
    //      the same block, because that item is the form a player holds: the
    //      door you carry is item 324, not block 64, and offering the block
    //      form is what put the *lower half of a door* in the hotbar instead of
    //      a door. This is read out of the measured `places` column, so it
    //      needs no list -- it covers signs, doors, reeds, seeds and redstone
    //      without naming any of them.
    //
    //   2. **Ours, and seven ids long.** Each of these is a block *state* the
    //      engine writes rather than a thing a player holds, and each has a
    //      resting form already in the palette:
    //
    //        8, 10  flowing water and flowing lava -- water and lava stay
    //        51     fire, which is what flint and steel is for
    //        62     the burning furnace, beside the furnace
    //        74     the lit redstone ore, beside the ore
    //        75     the *unlit* redstone torch -- the one you carry is 76, the
    //               lit one, which is why this pair is the way round it is
    //        43     the double slab, which is not placed but *made*: two slabs
    //               stacked merge into one, which the placement path does
    //               because the jar does. Offering it directly would be a
    //               shortcut past the only interesting thing about slabs.
    //
    //      a1.1.2 has no Creative mode and therefore no list to check this
    //      against -- see core/item/creative_palette.hpp -- so it is spelled out
    //      here rather than dressed up as a derivation. Everything else stays:
    //      the mob spawner and bedrock are both reachable, because a Creative
    //      mode with opinions about what you should want is worse than one
    //      without.
    private static final int[] PALETTE_HIDDEN = {8, 10, 43, 51, 62, 74, 75};

    private static final String[] ITEM_NAMES_FROM_256 = {
        "iron_shovel", "iron_pickaxe", "iron_axe", "flint_and_steel", "apple",
        "bow", "arrow", "coal", "diamond", "iron_ingot", "gold_ingot",
        "iron_sword", "wooden_sword", "wooden_shovel", "wooden_pickaxe",
        "wooden_axe", "stone_sword", "stone_shovel", "stone_pickaxe",
        "stone_axe", "diamond_sword", "diamond_shovel", "diamond_pickaxe",
        "diamond_axe", "stick", "bowl", "mushroom_stew", "golden_sword",
        "golden_shovel", "golden_pickaxe", "golden_axe", "string", "feather",
        "gunpowder", "wooden_hoe", "stone_hoe", "iron_hoe", "diamond_hoe",
        "golden_hoe", "seeds", "wheat", "bread", "leather_helmet",
        "leather_chestplate", "leather_leggings", "leather_boots",
        "chainmail_helmet", "chainmail_chestplate", "chainmail_leggings",
        "chainmail_boots", "iron_helmet", "iron_chestplate", "iron_leggings",
        "iron_boots", "diamond_helmet", "diamond_chestplate",
        "diamond_leggings", "diamond_boots", "golden_helmet",
        "golden_chestplate", "golden_leggings", "golden_boots", "flint",
        "raw_porkchop", "cooked_porkchop", "painting", "golden_apple", "sign",
        "wooden_door", "bucket", "water_bucket", "lava_bucket", "minecart",
        "saddle", "iron_door", "redstone", "snowball", "boat", "leather",
        "milk_bucket", "brick", "clay", "sugar_cane", "paper", "book",
        "slimeball", "storage_minecart", "powered_minecart", "egg", "compass",
        "fishing_rod",
    };

    // **The ids that are not in the run above.** a1.1.2's items are 256..346
    // and then jump to 2256, which is `Item.record13` -- so a name array would
    // need 1,910 nulls to reach them. The two records are the whole of it in
    // this version:
    //
    //     di.aQ = new lg(2000, "13").a(240);      // 2256, icon 240
    //     di.aR = new lg(2001, "cat").a(241);     // 2257, icon 241
    //
    // `lg`'s second argument is the *record* name -- what `World.playRecord`
    // is given -- and not an item name; `getItemName` is null for both, which
    // is why they fell through to `item_2256` before. The names below are the
    // record names with the kind in front, which is what every later version
    // calls them.
    private static final java.util.Map<Integer, String> ITEM_NAMES_ABOVE_THE_RUN =
        new java.util.HashMap<Integer, String>();
    static {
        ITEM_NAMES_ABOVE_THE_RUN.put(Integer.valueOf(2256), "record_13");
        ITEM_NAMES_ABOVE_THE_RUN.put(Integer.valueOf(2257), "record_cat");
    }

    // ---------------------------------------------------------------------
    // What a block leaves behind -- Block.idDropped and Block.quantityDropped
    // ---------------------------------------------------------------------
    //
    // Both take a `java.util.Random`, so neither can be read as a constant and
    // neither can be *sampled* honestly either: a table built from one draw
    // would say gravel drops flint. What they can be is **characterised**, by
    // handing them a Random that answers every `nextInt(bound)` with the lowest
    // value once and the highest value once and writes down the bounds it was
    // asked for. Every one of a1.1.2's twenty-four overrides draws at most once,
    // so two calls pin the whole distribution:
    //
    //   * no draw at all, and the two answers agree -- a constant;
    //   * one draw, low answer < high answer -- `min + nextInt(bound)`, which is
    //     redstone ore's 4..5;
    //   * one draw, low answer > high answer -- `nextInt(bound) == 0 ? v : 0`,
    //     which is a leaf block's one sapling in twenty;
    //   * for `idDropped`, one draw with two different ids -- gravel's flint.
    //
    // The generator asserts the "at most one draw" property rather than
    // assuming it, so a version whose block draws twice fails here instead of
    // shipping a table that is quietly wrong.
    //
    // **`Block.dropBlockAsItemWithChance` does not call `damageDropped`.** It
    // builds `new ItemStack(id)`, which is one item at damage zero -- so wool
    // does not keep its colour and a log does not keep its kind, in this
    // version. There is no damage column here because there is nothing to put
    // in it.
    private static final int DROP_MAX_BLOCK = 256;

    // A Random that reports what it was asked for and answers at one end of the
    // range or the other. `mode` 0 is the low end and 1 the high end.
    private static final class DropProbe extends Random {
        int mode = 0;
        int draws = 0;
        int lastBound = 0;

        DropProbe() { super(0L); }

        @Override public int nextInt(int bound) {
            draws++;
            lastBound = bound;
            return mode == 0 ? 0 : bound - 1;
        }
        @Override public int nextInt() { return nextInt(Integer.MAX_VALUE); }
        @Override public float nextFloat() { draws++; return mode == 0 ? 0.0f : 0.9999999f; }
    }

    // ---------------------------------------------------------------------
    // The paintings, out of `er` -- EnumArt
    // ---------------------------------------------------------------------
    //
    // Twenty-four rows of five values, and every one of them is a literal in
    // one static initialiser. This still goes through a generator rather than
    // being typed into a header, for the reason every other table here does: a
    // number that was read out of the class file cannot be a number somebody
    // remembered wrong, and a version whose art sheet differs regenerates
    // instead of being re-checked by eye.
    //
    // The five fields are `y` (the name, which is the game's own and is what a
    // save file stores), `z` and `A` (the size in texels), and `B` and `C` (the
    // offset into art/kz.png). Read by *type and order* rather than by name --
    // the four ints are declared in that order and there is nothing else in the
    // class to confuse them with.
    private static void emitArt(String jarPath) {
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> artClass = loader.loadClass("er");
            Object[] arts = (Object[]) artClass.getMethod("values").invoke(null);

            java.lang.reflect.Field nameField = artClass.getField("y");
            java.lang.reflect.Field sizeX = artClass.getField("z");
            java.lang.reflect.Field sizeY = artClass.getField("A");
            java.lang.reflect.Field offsetX = artClass.getField("B");
            java.lang.reflect.Field offsetY = artClass.getField("C");

            p("{");
            p("  \"$comment\": [");
            p("    \"GENERATED by tools/genref.java --art from a running a1.1.2 client jar.\",");
            p("    \"Do not edit by hand -- regenerate instead. This is `er` (EnumArt) read by\",");
            p("    \"reflection: `name` is the game's own string and is what a painting's NBT\",");
            p("    \"stores, `width`/`height` are the size in texels of art/kz.png, and\",");
            p("    \"`u`/`v` are where in that 256 x 256 sheet the picture begins.\",");
            p("    \"Order is EnumArt's declaration order, which is what EntityPainting's\",");
            p("    \"constructor draws from when it picks one at random.\"");
            p("  ],");
            p("  \"version\": \"a1.1.2\",");
            p("  \"paintings\": [");
            for (int i = 0; i < arts.length; i++) {
                Object art = arts[i];
                p("    {\"name\": \"" + nameField.get(art) + "\""
                  + ", \"width\": " + sizeX.getInt(art)
                  + ", \"height\": " + sizeY.getInt(art)
                  + ", \"u\": " + offsetX.getInt(art)
                  + ", \"v\": " + offsetY.getInt(art) + "}"
                  + (i + 1 < arts.length ? "," : ""));
            }
            p("  ]");
            p("}");
        } catch (Throwable t) {
            t.printStackTrace();
            System.exit(1);
        }
    }

    private static void emitDrops(String jarPath) {
        java.io.PrintStream realOut = System.out;
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> blockClass = loader.loadClass("ly");
            Object[] blocks = (Object[]) blockClass.getField("n").get(null);
            java.lang.reflect.Method idDropped =
                blockClass.getMethod("a", int.class, Random.class);
            java.lang.reflect.Method quantityDropped =
                blockClass.getMethod("a", Random.class);

            java.util.Map<Integer, String> blockNames = blockNamesFromJson();
            java.util.List<String> rows = new java.util.ArrayList<String>();
            java.util.List<String> notes = new java.util.ArrayList<String>();

            DropProbe probe = new DropProbe();

            // **`BlockCrops.idDropped` prints to stdout**, which is where the
            // JSON goes. That `System.out.println("Get resource: " + l)` is a
            // debug line left in the shipped jar; silencing it for the length
            // of the probe is the only way to read the method at all.
            java.io.ByteArrayOutputStream swallowed = new java.io.ByteArrayOutputStream();
            System.setOut(new java.io.PrintStream(swallowed, true, "UTF-8"));

            for (int id = 0; id < DROP_MAX_BLOCK && id < blocks.length; id++) {
                Object block = blocks[id];
                if (block == null) {
                    continue;
                }

                probe.mode = 0; probe.draws = 0;
                int qLow = ((Integer) quantityDropped.invoke(block, probe)).intValue();
                int qLowDraws = probe.draws;
                int qBound = probe.lastBound;
                probe.mode = 1; probe.draws = 0;
                int qHigh = ((Integer) quantityDropped.invoke(block, probe)).intValue();

                if (qLowDraws > 1 || probe.draws > 1) {
                    notes.add("block " + id + " draws " + qLowDraws + " times for its count");
                }

                int countMin;
                int countSpread;   // nextInt(spread) added to countMin; 0 for none
                int countOneIn;    // nextInt(countOneIn) == 0 ? countMin : 0
                if (qLowDraws == 0) {
                    countMin = qLow; countSpread = 0; countOneIn = 0;
                } else if (qLow <= qHigh) {
                    countMin = qLow; countSpread = qBound; countOneIn = 0;
                } else {
                    countMin = qLow; countSpread = 0; countOneIn = qBound;
                }

                int[] byMeta = new int[16];
                int altItem = 0;
                int altOneIn = 0;
                boolean varies = false;
                for (int m = 0; m < 16; m++) {
                    probe.mode = 1; probe.draws = 0;
                    int high = ((Integer) idDropped.invoke(block, Integer.valueOf(m), probe))
                        .intValue();
                    int highDraws = probe.draws;
                    int bound = probe.lastBound;
                    probe.mode = 0; probe.draws = 0;
                    int low = ((Integer) idDropped.invoke(block, Integer.valueOf(m), probe))
                        .intValue();

                    if (highDraws > 1 || probe.draws > 1) {
                        notes.add("block " + id + " draws " + highDraws + " times for its id");
                    }
                    byMeta[m] = high < 0 ? 0 : high;
                    if (highDraws > 0 && low != high) {
                        int alt = low < 0 ? 0 : low;
                        if (altItem != 0 && altItem != alt) {
                            notes.add("block " + id + " has two different alternate drops");
                        }
                        altItem = alt;
                        altOneIn = bound;
                    }
                    if (byMeta[m] != byMeta[0]) {
                        varies = true;
                    }
                }
                if (varies && altOneIn != 0) {
                    notes.add("block " + id + " both varies by metadata and rolls for its id");
                }

                String name = blockNames.get(Integer.valueOf(id));
                StringBuilder row = new StringBuilder();
                row.append("    {\"id\": ").append(id);
                if (name != null) {
                    row.append(", \"name\": \"").append(name).append("\"");
                }
                row.append(", \"item\": ").append(byMeta[0]);
                if (varies) {
                    row.append(", \"itemByMetadata\": [");
                    for (int m = 0; m < 16; m++) {
                        row.append(m == 0 ? "" : ", ").append(byMeta[m]);
                    }
                    row.append("]");
                }
                if (altOneIn != 0) {
                    row.append(", \"altItem\": ").append(altItem)
                       .append(", \"altOneIn\": ").append(altOneIn);
                }
                row.append(", \"countMin\": ").append(countMin);
                if (countSpread != 0) {
                    row.append(", \"countSpread\": ").append(countSpread);
                }
                if (countOneIn != 0) {
                    row.append(", \"countOneIn\": ").append(countOneIn);
                }
                row.append("}");
                rows.add(row.toString());
            }

            System.setOut(realOut);

            p("{");
            p("  \"$comment\": [");
            p("    \"GENERATED by tools/genref.java --drops from a running a1.1.2 client jar.\",");
            p("    \"Do not edit by hand -- regenerate instead. Every row is Block.idDropped and\",");
            p("    \"Block.quantityDropped characterised by a Random that answers at both ends of\",");
            p("    \"its range and records the bounds it was asked for; see that tool's emitDrops.\",");
            p("    \"`item` is what the block leaves behind, 0 for nothing. `itemByMetadata` is\",");
            p("    \"present only when the answer depends on the metadata -- a door's upper half\",");
            p("    \"drops nothing, a crop drops only when it is grown. `altItem`/`altOneIn` is\",");
            p("    \"the second answer and its odds: nextInt(altOneIn) == 0 picks it, which is\",");
            p("    \"gravel's flint and nothing else in this version. `countMin` is how many drop,\",");
            p("    \"`countSpread` adds nextInt(countSpread) to it, and `countOneIn` is the other\",");
            p("    \"form -- nextInt(countOneIn) == 0 ? countMin : 0, which is a leaf block.\",");
            p("    \"There is no damage column: dropBlockAsItemWithChance builds new ItemStack(id),\",");
            p("    \"so a1.1.2 drops every block at damage zero.\"");
            p("  ],");
            p("  \"version\": \"a1.1.2\",");
            if (!notes.isEmpty()) {
                p("  \"$warnings\": [");
                for (int i = 0; i < notes.size(); i++) {
                    p("    \"" + notes.get(i) + "\"" + (i + 1 < notes.size() ? "," : ""));
                }
                p("  ],");
            }
            p("  \"drops\": [");
            for (int i = 0; i < rows.size(); i++) {
                p(rows.get(i) + (i + 1 < rows.size() ? "," : ""));
            }
            p("  ]");
            p("}");
        } catch (Throwable t) {
            System.setOut(realOut);
            t.printStackTrace();
            System.exit(1);
        }
    }

    // The names `spawns` is emitted as, indexed by the code emitItems assigns.
    private static final String[] SPAWN_NAMES = {"none", "painting", "boat", "minecart", "arrow"};

    // ---------------------------------------------------------------------
    // What Survival asks of an item and a block -- `--harvest`
    // ---------------------------------------------------------------------
    //
    // Emits data/<version>/harvest.json. Every number is asked of the running
    // jar, through the same methods the game calls, rather than read off a
    // constructor:
    //
    //   * `needsTool` -- `dm.b(Lly;)Z` (canHarvestBlock) with an **empty hand**.
    //     `eu.b(Lly;)Z` answers true outright unless the block's material is
    //     one of four, so an empty hand is exactly the question "is this block
    //     gated at all". Measured, so the four materials never have to be named.
    //   * `efficiency` -- `di.a(Lev;Lly;)F` (getStrVsBlock) for every block, kept
    //     only where it is not `Item`'s constant 1. A sword answers 1.5 for
    //     everything; a tool answers `(material + 1) * 2` for its own list.
    //   * `harvests` -- `di.a(Lly;)Z` (canHarvestBlock) for every block. Only the
    //     pickaxe (`y`) and the shovel (`ms`) override it.
    //   * `wearOnHit` / `wearOnBreak` -- what `di.a(Lev;Lge;)V` (hitEntity) and
    //     `di.a(Lev;IIII)V` (hitBlock) add to a fresh stack's damage. A tool is
    //     2 and 1, a sword 1 and 2, the hoe and flint and steel neither -- their
    //     wear is spent from their own use methods, not from these two.
    //   * `armourPoints` -- `mr.aY`, what `eu.f()` sums.
    //   * `heals` -- `oj.a`, what ItemFood hands to `ge.b(I)V`.
    //
    // Only rows with something to say are emitted.

    private static void emitHarvest(String jarPath) {
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> worldClass = loader.loadClass(WORLD);
            Class<?> blockClass = loader.loadClass("ly");
            Class<?> itemClass = loader.loadClass("di");
            Class<?> stackClass = loader.loadClass("ev");
            Class<?> playerClass = loader.loadClass("dm");
            Class<?> livingClass = loader.loadClass("ge");
            Class<?> armorClass = loader.loadClass("mr");
            Class<?> foodClass = loader.loadClass("oj");

            Object[] blocks = (Object[]) blockClass.getField("n").get(null);
            Object[] items = (Object[]) itemClass.getField("c").get(null);

            java.lang.reflect.Constructor<?> stackCtor =
                stackClass.getConstructor(int.class, int.class, int.class);
            java.lang.reflect.Method strVsBlock =
                itemClass.getMethod("a", stackClass, blockClass);
            java.lang.reflect.Method canHarvest = itemClass.getMethod("a", blockClass);
            java.lang.reflect.Method hitEntity =
                itemClass.getMethod("a", stackClass, livingClass);
            java.lang.reflect.Method hitBlock = itemClass.getMethod(
                "a", stackClass, int.class, int.class, int.class, int.class);
            java.lang.reflect.Method playerCanHarvest = playerClass.getMethod("b", blockClass);
            java.lang.reflect.Field stackDamage = stackClass.getField("d");
            java.lang.reflect.Field armourPoints = armorClass.getField("aY");
            java.lang.reflect.Field heals = foodClass.getDeclaredField("a");
            heals.setAccessible(true);

            java.io.File dir = java.nio.file.Files.createTempDirectory("genref-harvest").toFile();
            java.lang.reflect.Constructor<?> worldCtor =
                worldClass.getConstructor(java.io.File.class, String.class, long.class);
            Object world = worldCtor.newInstance(dir, "genref", 1L);
            Object player = playerClass.getConstructor(worldClass).newInstance(world);

            java.util.Map<Integer, String> blockNames = blockNamesFromJson();
            java.util.Map<Integer, String> itemNames = itemNamesFromJson();

            java.util.List<String> gated = new java.util.ArrayList<String>();
            for (int b = 0; b < blocks.length && b < 256; b++) {
                if (blocks[b] == null) {
                    continue;
                }
                if (!((Boolean) playerCanHarvest.invoke(player, blocks[b])).booleanValue()) {
                    gated.add("    {\"id\": " + b + ", \"name\": \""
                              + blockNames.get(Integer.valueOf(b)) + "\"}");
                }
            }

            java.util.List<String> tools = new java.util.ArrayList<String>();
            java.util.List<String> armour = new java.util.ArrayList<String>();
            java.util.List<String> food = new java.util.ArrayList<String>();
            for (int id = 256; id < items.length; id++) {
                Object it = items[id];
                if (it == null) {
                    continue;
                }
                String name = itemNames.get(Integer.valueOf(id));

                StringBuilder efficiency = new StringBuilder();
                StringBuilder harvests = new StringBuilder();
                for (int b = 0; b < blocks.length && b < 256; b++) {
                    if (blocks[b] == null) {
                        continue;
                    }
                    Object stack = stackCtor.newInstance(id, 1, 0);
                    float f = ((Float) strVsBlock.invoke(it, stack, blocks[b])).floatValue();
                    if (f != 1.0f) {
                        efficiency.append(efficiency.length() == 0 ? "" : ", ")
                                  .append("[").append(b).append(", ").append(f).append("]");
                    }
                    if (((Boolean) canHarvest.invoke(it, blocks[b])).booleanValue()) {
                        harvests.append(harvests.length() == 0 ? "" : ", ").append(b);
                    }
                }
                Object hitStack = stackCtor.newInstance(id, 1, 0);
                hitEntity.invoke(it, hitStack, null);
                int wearOnHit = stackDamage.getInt(hitStack);
                Object breakStack = stackCtor.newInstance(id, 1, 0);
                hitBlock.invoke(it, breakStack, 1, 0, 0, 0);
                int wearOnBreak = stackDamage.getInt(breakStack);

                if (efficiency.length() > 0 || harvests.length() > 0 || wearOnHit != 0
                    || wearOnBreak != 0) {
                    tools.add("    {\"id\": " + id + ", \"name\": \"" + name + "\""
                              + ", \"wearOnHit\": " + wearOnHit
                              + ", \"wearOnBreak\": " + wearOnBreak
                              + ", \"efficiency\": [" + efficiency + "]"
                              + ", \"harvests\": [" + harvests + "]}");
                }
                if (armorClass.isInstance(it)) {
                    armour.add("    {\"id\": " + id + ", \"name\": \"" + name + "\""
                               + ", \"points\": " + armourPoints.getInt(it) + "}");
                }
                if (foodClass.isInstance(it)) {
                    food.add("    {\"id\": " + id + ", \"name\": \"" + name + "\""
                             + ", \"heals\": " + heals.getInt(it) + "}");
                }
            }
            deleteTree(dir);

            p("{");
            p("  \"$comment\": [");
            p("    \"GENERATED by tools/genref.java --harvest from a running a1.1.2 client jar.\",");
            p("    \"Do not edit by hand -- regenerate instead. See that tool's emitHarvest.\",");
            p("    \"`needsTool` lists the blocks an empty hand cannot harvest: dm.b(ly) is\",");
            p("    \"true outright unless the material is one of four, so this is that set.\",");
            p("    \"`tools` is every item that answers something other than Item's defaults:\",");
            p("    \"`efficiency` is getStrVsBlock as [block, value] where it is not 1,\",");
            p("    \"`harvests` the blocks canHarvestBlock accepts, and `wearOnHit` and\",");
            p("    \"`wearOnBreak` the damage hitEntity and hitBlock add to a fresh stack.\",");
            p("    \"`armour` is ItemArmor's damage-reduction points (mr.aY), `food` the\",");
            p("    \"health ItemFood restores (oj.a).\"");
            p("  ],");
            p("  \"version\": \"a1.1.2\",");
            p("  \"needsTool\": [");
            for (int i = 0; i < gated.size(); i++) {
                p(gated.get(i) + (i + 1 < gated.size() ? "," : ""));
            }
            p("  ],");
            p("  \"tools\": [");
            for (int i = 0; i < tools.size(); i++) {
                p(tools.get(i) + (i + 1 < tools.size() ? "," : ""));
            }
            p("  ],");
            p("  \"armour\": [");
            for (int i = 0; i < armour.size(); i++) {
                p(armour.get(i) + (i + 1 < armour.size() ? "," : ""));
            }
            p("  ],");
            p("  \"food\": [");
            for (int i = 0; i < food.size(); i++) {
                p(food.get(i) + (i + 1 < food.size() ? "," : ""));
            }
            p("  ]");
            p("}");
        } catch (Throwable t) {
            t.printStackTrace();
            System.exit(1);
        }
    }

    // ---------------------------------------------------------------------
    // Crafting and the furnace -- `--recipes`
    // ---------------------------------------------------------------------
    //
    // Emits data/<version>/recipes.json.
    //
    //   * `crafting` is `dw.b` -- CraftingManager's list -- **in list order**,
    //     because `dw.a([I)Lev;` returns the first match and the order is
    //     therefore part of the rule. Each entry is `bv`'s four fields as they
    //     are: width, height, the cells row by row (-1 for a blank), and the
    //     result stack. a1.1.2 has only shaped recipes. The matching itself --
    //     every offset, mirrored before straight -- is code, not data; see
    //     core/item/crafting.hpp.
    //   * `smelting` is `ke.d(I)I`, asked of every id: the furnace's result is a
    //     chain of comparisons in the tile entity, not a table, in this version.
    //   * `fuel` is `ke.a(Lev;)I`, asked of every item the version defines.

    private static void emitRecipes(String jarPath) {
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> managerClass = loader.loadClass("dw");
            Class<?> recipeClass = loader.loadClass("bv");
            Class<?> stackClass = loader.loadClass("ev");
            Class<?> itemClass = loader.loadClass("di");
            Class<?> furnaceClass = loader.loadClass("ke");

            Object[] items = (Object[]) itemClass.getField("c").get(null);
            // Loading `ly` is what fills Item.itemsList's first 256 entries.
            loader.loadClass("ly").getField("n").get(null);

            // **CraftingManager's constructor prints to stdout** --
            // `System.out.println(list.size() + " recipes")`, left in the shipped
            // jar -- which is where the JSON goes. Silenced for the length of the
            // class initialiser, the same way emitDrops silences BlockCrops.
            java.io.PrintStream realOut = System.out;
            Object manager;
            try {
                System.setOut(new java.io.PrintStream(new java.io.ByteArrayOutputStream(), true,
                                                      "UTF-8"));
                manager = managerClass.getMethod("a").invoke(null);
            } finally {
                System.setOut(realOut);
            }
            java.lang.reflect.Field listField = managerClass.getDeclaredField("b");
            listField.setAccessible(true);
            java.util.List<?> list = (java.util.List<?>) listField.get(manager);

            java.lang.reflect.Field width = recipeClass.getDeclaredField("b");
            java.lang.reflect.Field height = recipeClass.getDeclaredField("c");
            java.lang.reflect.Field cells = recipeClass.getDeclaredField("d");
            java.lang.reflect.Field result = recipeClass.getDeclaredField("e");
            width.setAccessible(true);
            height.setAccessible(true);
            cells.setAccessible(true);
            result.setAccessible(true);
            java.lang.reflect.Field stackCount = stackClass.getField("a");
            java.lang.reflect.Field stackId = stackClass.getField("c");
            java.lang.reflect.Field stackDamage = stackClass.getField("d");
            java.lang.reflect.Constructor<?> stackCtor =
                stackClass.getConstructor(int.class, int.class, int.class);

            java.util.Map<Integer, String> itemNames = itemNamesFromJson();

            java.util.List<String> crafting = new java.util.ArrayList<String>();
            for (Object r : list) {
                int[] c = (int[]) cells.get(r);
                Object out = result.get(r);
                StringBuilder row = new StringBuilder();
                for (int i = 0; i < c.length; i++) {
                    row.append(i == 0 ? "" : ", ").append(c[i]);
                }
                int outId = stackId.getInt(out);
                crafting.add("    {\"width\": " + width.getInt(r) + ", \"height\": "
                             + height.getInt(r) + ", \"cells\": [" + row + "]"
                             + ", \"result\": " + outId
                             + ", \"count\": " + stackCount.getInt(out)
                             + ", \"damage\": " + stackDamage.getInt(out)
                             + ", \"name\": \"" + itemNames.get(Integer.valueOf(outId)) + "\"}");
            }

            Object furnace = furnaceClass.getConstructor().newInstance();
            java.lang.reflect.Method smeltResult =
                furnaceClass.getDeclaredMethod("d", int.class);
            java.lang.reflect.Method burnTime =
                furnaceClass.getDeclaredMethod("a", stackClass);
            smeltResult.setAccessible(true);
            burnTime.setAccessible(true);

            java.util.List<String> smelting = new java.util.ArrayList<String>();
            java.util.List<String> fuel = new java.util.ArrayList<String>();
            for (int id = 0; id < items.length; id++) {
                int out = ((Integer) smeltResult.invoke(furnace, id)).intValue();
                if (out >= 0) {
                    smelting.add("    {\"input\": " + id + ", \"result\": " + out
                                 + ", \"name\": \"" + itemNames.get(Integer.valueOf(id)) + "\"}");
                }
                if (items[id] == null) {
                    continue;
                }
                int ticks = ((Integer) burnTime.invoke(
                    furnace, stackCtor.newInstance(id, 1, 0))).intValue();
                if (ticks != 0) {
                    fuel.add("    {\"id\": " + id + ", \"ticks\": " + ticks
                             + ", \"name\": \"" + itemNames.get(Integer.valueOf(id)) + "\"}");
                }
            }

            p("{");
            p("  \"$comment\": [");
            p("    \"GENERATED by tools/genref.java --recipes from a running a1.1.2 client jar.\",");
            p("    \"Do not edit by hand -- regenerate instead. See that tool's emitRecipes.\",");
            p("    \"`crafting` is CraftingManager's list in its own order, which matters: the\",");
            p("    \"first recipe that matches wins. `cells` are item ids row by row, -1 for\",");
            p("    \"a blank. `smelting` is TileEntityFurnace's result for each input id, and\",");
            p("    \"`fuel` its burn time in ticks for each item that has one.\"");
            p("  ],");
            p("  \"version\": \"a1.1.2\",");
            p("  \"crafting\": [");
            for (int i = 0; i < crafting.size(); i++) {
                p(crafting.get(i) + (i + 1 < crafting.size() ? "," : ""));
            }
            p("  ],");
            p("  \"smelting\": [");
            for (int i = 0; i < smelting.size(); i++) {
                p(smelting.get(i) + (i + 1 < smelting.size() ? "," : ""));
            }
            p("  ],");
            p("  \"fuel\": [");
            for (int i = 0; i < fuel.size(); i++) {
                p(fuel.get(i) + (i + 1 < fuel.size() ? "," : ""));
            }
            p("  ]");
            p("}");
        } catch (Throwable t) {
            t.printStackTrace();
            System.exit(1);
        }
    }

    // Item names out of data/a1.1.2/items.json, for readable rows. The names are
    // this project's, not the jar's, exactly as in blockNamesFromJson.
    private static java.util.Map<Integer, String> itemNamesFromJson() throws Exception {
        java.util.Map<Integer, String> out = new java.util.HashMap<Integer, String>();
        java.io.File file = new java.io.File("data/a1.1.2/items.json");
        if (!file.isFile()) {
            return out;
        }
        String text = new String(java.nio.file.Files.readAllBytes(file.toPath()), "UTF-8");
        java.util.regex.Matcher m = java.util.regex.Pattern
            .compile("\"id\": (\\d+), \"name\": \"([^\"]*)\"").matcher(text);
        while (m.find()) {
            out.put(Integer.valueOf(m.group(1)), m.group(2));
        }
        return out;
    }

    private static void emitItems(String jarPath, String scratchDir) {
        java.io.File dir = new java.io.File(scratchDir, "items");
        try {
            java.net.URLClassLoader loader = new java.net.URLClassLoader(
                new java.net.URL[]{new java.io.File(jarPath).toURI().toURL()},
                genref.class.getClassLoader());

            Class<?> itemClass = loader.loadClass("di");
            // **ItemArmor, and the field the game itself validates against.**
            // `lj` is SlotArmor and its `isItemValid` is the whole rule:
            // `stack.getItem() instanceof ItemArmor && armorType == this.slotType`.
            // So "which armour slot does this item belong in" is not a judgement
            // to be made from a name -- it is one field, and this reads it.
            Class<?> armorClass = loader.loadClass("mr");
            java.lang.reflect.Field armorType = armorClass.getField("aX");
            // **ItemBucket, and the one int that says what it is.** `ac`'s
            // private `a` is 0 for the empty bucket, the **flowing** block id
            // for a full one -- 8 and 10, not the still 9 and 11 -- and -1 for
            // milk. `onItemRightClick` branches on all three, so the column
            // carries the field verbatim rather than splitting it up.
            Class<?> bucketClass = loader.loadClass("ac");
            java.lang.reflect.Field bucketFill = bucketClass.getDeclaredField("a");
            bucketFill.setAccessible(true);
            // **The items that spawn an entity instead of placing a block.**
            //
            // Four of them in a1.1.2, and they are the reason four things on a
            // play-session list did nothing at all: `onItemUse` and
            // `onItemRightClick` on these build an entity and hand it to
            // `World.entityJoinedWorld`, so a build with nowhere to put an
            // entity performs the whole click and produces nothing. The
            // `places` column below measures them as 0, correctly, and that is
            // exactly why a second column is needed -- "places no block" and
            // "does nothing" are not the same answer.
            //
            // Read as `instanceof` against the item classes, which is the same
            // shape the armour and bucket columns above already use and is the
            // one place in this project where obfuscated names live. `jo`
            // carries a public `a` as well: 0 a plain minecart, 1 a chest, 2 a
            // furnace, which is the constructor argument and not a guess.
            Class<?> paintingItem = loader.loadClass("od");
            Class<?> boatItem = loader.loadClass("me");
            Class<?> minecartItem = loader.loadClass("jo");
            Class<?> bowItem = loader.loadClass("jg");
            // **ItemHoe**, and it is asked as a class for the same reason the
            // four above are: `onItemUse` is the whole of what a hoe does and
            // there is no field, name or measurable placement that gives it
            // away. `places` measures 0 for all five hoes -- farmland is
            // written by `setBlockWithNotify` on the *struck* cell rather than
            // into the face's offset, so the probe below never sees it -- which
            // is the same "places nothing and still does something" hole the
            // `spawns` column exists for.
            Class<?> hoeItem = loader.loadClass("fu");
            // **ItemRecord**, whose private `a` is the track name -- "13" and
            // "cat". It is read rather than matched on the item's name because
            // a1.1.2's `Item` carries no names of its own: this one string is
            // the game's own answer to which file a disc plays, and it is also
            // the key `SoundPool` files `streaming/13.mus` under.
            Class<?> recordItem = loader.loadClass("lg");
            java.lang.reflect.Field recordName = recordItem.getDeclaredField("a");
            recordName.setAccessible(true);
            java.lang.reflect.Field minecartType = minecartItem.getField("a");
            Class<?> worldClass = loader.loadClass(WORLD);
            Class<?> playerClass = loader.loadClass("dm");
            Class<?> stackClass = loader.loadClass("ev");
            Class<?> entityClass = loader.loadClass("kh");

            // **The animated icon, asked of the class that animates it.**
            // a1.1.2 does not draw a compass; `RenderEngine` registers an `aa`
            // -- TextureCompassFX -- and overwrites one 16 x 16 tile of one
            // sheet every frame. So "which item is the compass" is the wrong
            // question and this does not ask it: it constructs the FX and reads
            // the two fields `z` gives every one of them, `b` (the tile it
            // owns) and `f` (0 for terrain.png, 1 for gui/items.png). Any item
            // whose icon is that tile on that sheet shows the animation,
            // because the animation is a property of the *tile*.
            //
            // `aa`'s constructor takes a Minecraft and null does for it: the
            // reference is only stored, and the two things it reads at
            // construction -- Item.compass's icon index, and the tile's pixels
            // out of /gui/items.png -- come off the jar and the class loader.
            //
            // The other five `z` subclasses in this jar are water, flowing
            // water, lava, flowing lava and the two flames, all on terrain.png;
            // **there is no clock in a1.1.2**, which is why this looks for one
            // FX rather than a list.
            int compassTile = -1;
            int compassSheet = -1;
            try {
                Class<?> fxBase = loader.loadClass("z");
                Class<?> compassFx = loader.loadClass("aa");
                Class<?> mcClass = loader.loadClass("net.minecraft.client.Minecraft");
                Object fx = compassFx.getConstructor(mcClass).newInstance(new Object[]{null});
                compassTile = fxBase.getField("b").getInt(fx);
                compassSheet = fxBase.getField("f").getInt(fx);
            } catch (Throwable t) {
                System.err.println("genref --items: no compass FX in this jar (" + t + ")");
            }

            Object[] items = (Object[]) itemClass.getField("c").get(null);

            // maxStackSize, maxDamage, bFull3D. Read as fields rather than
            // through their getters because two of the getters are overridden
            // per item and this wants the constructed value.
            java.lang.reflect.Field maxStack = itemClass.getDeclaredField("aT");
            java.lang.reflect.Field maxDamage = itemClass.getDeclaredField("aU");
            maxStack.setAccessible(true);
            maxDamage.setAccessible(true);

            // getIconIndex(ItemStack) rather than the iconIndex field: armour
            // and tools answer per stack, and the stack is what the UI holds.
            java.lang.reflect.Method iconOf = itemClass.getMethod("a", stackClass);
            // onItemUse(stack, player, world, x, y, z, face)
            java.lang.reflect.Method onItemUse = itemClass.getMethod(
                "a", stackClass, playerClass, worldClass,
                int.class, int.class, int.class, int.class);
            // **getDamageVsEntity(Entity)** -- `di.a(Lkh;)I`, which is what
            // `InventoryPlayer.getDamageVsEntity` forwards a held stack to and
            // is therefore the whole of how hard a click hits. Two classes
            // override it, `bs` (ItemTool) and `iu` (ItemSword), and both
            // answer from a field the constructor set; everything else takes
            // `Item`'s constant 1. **Null is a safe argument**: no override in
            // this version looks at the entity at all, which is exactly why the
            // number can be a column rather than a call.
            java.lang.reflect.Method damageVsEntity = itemClass.getMethod("a", entityClass);

            java.lang.reflect.Constructor<?> stackCtor =
                stackClass.getConstructor(int.class, int.class, int.class);
            java.lang.reflect.Constructor<?> worldCtor =
                worldClass.getConstructor(java.io.File.class, String.class, long.class);
            java.lang.reflect.Constructor<?> playerCtor = playerClass.getConstructor(worldClass);

            java.lang.reflect.Method getChunk = worldClass.getMethod("b", int.class, int.class);
            java.lang.reflect.Method setBlock = worldClass.getMethod(
                "a", int.class, int.class, int.class, int.class, int.class);
            java.lang.reflect.Method getBlockId =
                worldClass.getMethod("a", int.class, int.class, int.class);
            java.lang.reflect.Method setPosition =
                entityClass.getMethod("a", double.class, double.class, double.class);
            java.lang.reflect.Field yawField = entityField(entityClass, "aq");

            deleteTree(dir);
            dir.mkdirs();
            Object world = worldCtor.newInstance(dir, "genref", 1234567890L);
            getChunk.invoke(world, 0, 0);
            Object player = playerCtor.newInstance(world);

            final int bx = 8;
            final int by = 100;
            final int bz = 8;

            // Stone, dirt, sand, farmland, grass -- enough between them to
            // satisfy every precondition a1.1.2's placeable items have.
            final int[] grounds = {1, 3, 12, 60, 2};
            final int WATER_STILL = 9;
            // Top first, then the four sides. a1.1.2's face numbering, and the
            // offset each one places into.
            final int[] faces = {1, 2, 3, 4, 5};
            final int[][] faceOffset = {
                {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0},
            };

            // Far from the block being used, so nothing an item places lands
            // inside the player and gets refused, and with a fixed heading so
            // the blocks that orient from it are reproducible.
            setPosition.invoke(player, bx + 16.5, by + 4.0, bz + 16.5);
            yawField.setFloat(player, 0.0f);

            java.util.Map<Integer, String> blockNames = blockNamesFromJson();

            // Two passes: the palette rule needs the whole `places` column
            // before any row can be written.
            java.util.List<int[]> measured = new java.util.ArrayList<int[]>();
            java.util.List<String> names = new java.util.ArrayList<String>();
            java.util.List<String> records = new java.util.ArrayList<String>();
            java.util.List<java.util.Set<Integer>> placedByItem =
                new java.util.ArrayList<java.util.Set<Integer>>();
            java.util.List<String> rows = new java.util.ArrayList<String>();
            for (int id = 0; id < items.length; id++) {
                Object it = items[id];
                if (it == null) {
                    continue;
                }
                Object sample = stackCtor.newInstance(id, 64, 0);
                int icon = ((Integer) iconOf.invoke(it, sample)).intValue();
                int stack = maxStack.getInt(it);
                int damage = maxDamage.getInt(it);

                // `places` is the first block this item puts down with the
                // top face preferred, and `alsoPlaces` is every block it can
                // put down at all. The second is what the palette rule needs: a
                // sign is item 323 on a post *and* on a wall, and recording
                // only the post would leave the wall sign's block form offered
                // in the hand.
                int places = 0;
                java.util.Set<Integer> alsoPlaces = new java.util.TreeSet<Integer>();
                for (int ground : grounds) {
                    for (int face : faces) {
                        clearScene(setBlock, world, bx, by, bz);
                        // One block to click on, standing clear of everything
                        // so all five of its exposed faces place into air, on a
                        // floor a block below so nothing that needs support
                        // falls off the moment it lands.
                        setBlock.invoke(world, bx, by, bz, ground, 0);
                        for (int x = bx - 2; x <= bx + 2; x++) {
                            for (int z = bz - 2; z <= bz + 2; z++) {
                                setBlock.invoke(world, x, by - 1, z, ground, 0);
                            }
                        }
                        // Beside the floor, which is where reeds look for it.
                        setBlock.invoke(world, bx + 2, by - 1, bz, WATER_STILL, 0);
                        Object using = stackCtor.newInstance(id, 64, 0);
                        try {
                            onItemUse.invoke(it, using, player, world, bx, by, bz, face);
                        } catch (Throwable ignored) {
                            // An item that needs something this scene does not
                            // have -- an entity to hit, a boat to float -- is an
                            // item that places nothing, which is the answer.
                        }
                        int[] off = faceOffset[face];
                        int landed = ((Integer) getBlockId.invoke(
                            world, bx + off[0], by + off[1], bz + off[2])).intValue();
                        if (landed != 0) {
                            alsoPlaces.add(Integer.valueOf(landed));
                            if (places == 0) {
                                places = landed;
                            }
                        }
                    }
                }

                String name = id < 256
                    ? blockNames.get(Integer.valueOf(id))
                    : (id - 256 < ITEM_NAMES_FROM_256.length
                        ? ITEM_NAMES_FROM_256[id - 256] : null);
                if (name == null) {
                    name = ITEM_NAMES_ABOVE_THE_RUN.get(Integer.valueOf(id));
                }
                if (name == null) {
                    // Anything a later version adds above the run. Named by id
                    // so the row is still readable.
                    name = "item_" + id;
                }

                // 0 helmet, 1 chestplate, 2 leggings, 3 boots; -1 for
                // anything that is not armour at all.
                final int armour = armorClass.isInstance(items[id])
                    ? armorType.getInt(items[id]) : -1;
                // -2 for "not a bucket", which is the one value the field
                // itself can never hold: see core/item/item_def.hpp.
                final int bucket = bucketClass.isInstance(items[id])
                    ? bucketFill.getInt(items[id]) : -2;

                // Which entity this item puts into the world, as a name rather
                // than a class: the obfuscated one means nothing outside this
                // jar and the next version's would differ. 0 none, 1 painting,
                // 2 boat, 3 minecart, 4 arrow.
                int spawns = 0;
                int spawnVariant = 0;
                if (paintingItem.isInstance(items[id])) {
                    spawns = 1;
                } else if (boatItem.isInstance(items[id])) {
                    spawns = 2;
                } else if (minecartItem.isInstance(items[id])) {
                    spawns = 3;
                    spawnVariant = minecartType.getInt(items[id]);
                } else if (bowItem.isInstance(items[id])) {
                    spawns = 4;
                }

                // 1 when this item's icon is the tile the compass FX owns on
                // the sheet it owns it on, 0 otherwise. See compassTile above.
                final int fx = (compassTile >= 0 && icon == compassTile
                                && compassSheet == (id < 256 ? 0 : 1)) ? 1 : 0;

                // What a click with this in the hand does to what it hits. 1
                // for the great majority, because `Item`'s own method is a
                // constant and only the tools and the swords override it.
                final int hits = ((Integer) damageVsEntity.invoke(
                    items[id], new Object[]{null})).intValue();
                // Whether this item turns grass or dirt into farmland.
                final int tills = hoeItem.isInstance(items[id]) ? 1 : 0;
                // The track a music disc plays, and "" for everything else.
                records.add(recordItem.isInstance(items[id])
                    ? (String) recordName.get(items[id]) : "");

                measured.add(new int[]{id, icon, stack, damage, places, armour, bucket, fx,
                                      spawns, spawnVariant, hits, tills});
                names.add(name);
                placedByItem.add(alsoPlaces);
            }

            // Rule 1: a block placed by an item above 255 has a carried form
            // already, so its ItemBlock is not offered.
            java.util.Set<Integer> supersededBlocks = new java.util.HashSet<Integer>();
            for (int i = 0; i < measured.size(); i++) {
                if (measured.get(i)[0] >= 256) {
                    supersededBlocks.addAll(placedByItem.get(i));
                }
            }
            java.util.Set<Integer> hidden = new java.util.HashSet<Integer>();
            for (int id : PALETTE_HIDDEN) {
                hidden.add(Integer.valueOf(id));
            }

            for (int i = 0; i < measured.size(); i++) {
                int[] row = measured.get(i);
                int id = row[0];
                // **Everything the version defines, minus the two kinds of
                // duplicate.** The palette used to require `places != 0`, on
                // the argument that a sword does nothing this build can
                // perform. That was the wrong test: a Creative hand is also
                // how you put a sword, an ingot or a helmet into a chest, into
                // a save, or on the ground -- and a catalogue that silently
                // omits two thirds of the item table reads as a table with
                // holes in it, which is what it was reported as. The two
                // exclusions that remain are both about the *same* thing
                // appearing twice, and neither hides anything a player could
                // otherwise not reach.
                boolean inPalette = !hidden.contains(Integer.valueOf(id))
                    && !(id < 256 && supersededBlocks.contains(Integer.valueOf(id)));
                rows.add("    {\"id\": " + id + ", \"name\": \"" + names.get(i) + "\""
                         + ", \"sheet\": \"" + (id < 256 ? "terrain" : "items") + "\""
                         + ", \"icon\": " + row[1]
                         + ", \"stack\": " + row[2]
                         + ", \"durability\": " + row[3]
                         + ", \"damageVsEntity\": " + row[10]
                         + ", \"places\": " + row[4]
                         + ", \"armour\": " + row[5]
                         + ", \"bucket\": " + row[6]
                         + ", \"fx\": " + row[7]
                         + ", \"spawns\": \"" + SPAWN_NAMES[row[8]] + "\""
                         + ", \"spawnVariant\": " + row[9]
                         + ", \"tills\": " + (row[11] != 0)
                         + (records.get(i).isEmpty()
                            ? "" : ", \"record\": \"" + records.get(i) + "\"")
                         + ", \"palette\": " + inPalette + "}");
            }

            p("{");
            p("  \"$comment\": [");
            p("    \"GENERATED by tools/genref.java --items from a running a1.1.2 client jar.\",");
            p("    \"Do not edit by hand -- regenerate instead. Every column but `name` was read\",");
            p("    \"or measured out of the jar; see that tool's emitItems for how, and for why\",");
            p("    \"`places` is measured by using each item in a real world rather than read.\",");
            p("    \"`sheet` is which image the icon indexes: RenderItem picks terrain.png for\",");
            p("    \"ids below 256 and gui/items.png above, so the column is that rule applied.\",");
            p("    \"`places` is the block id the item puts down, or 0 for one that places none.\",");
            p("    \"`armour` is ItemArmor.armorType -- 0 helmet, 1 chestplate, 2 leggings,\",");
            p("    \"3 boots -- and -1 for everything that is not armour. It is the field\",");
            p("    \"SlotArmor.isItemValid compares against, so it is the game's own answer to\",");
            p("    \"which of the four slots a piece goes in rather than a reading of its name.\",");
            p("    \"`bucket` is ItemBucket.isFull: 0 for the empty bucket, the flowing block id\",");
            p("    \"for a full one (8 and 10, not the still 9 and 11), -1 for milk, and -2 for\",");
            p("    \"everything that is not a bucket at all.\",");
            p("    \"`damageVsEntity` is Item.getDamageVsEntity(null), the number\",");
            p("    \"attackTargetEntityWithCurrentItem passes to attackEntityFrom: 1 for\",");
            p("    \"everything that is neither a tool nor a sword, and a constructed field\",");
            p("    \"on the two classes that are. Null is safe -- no override reads the\",");
            p("    \"entity -- which is why it can be a column at all.\",");
            p("    \"`tills` is whether the item is an ItemHoe, asked as a class: a hoe\",");
            p("    \"writes farmland into the struck cell rather than into the face's\",");
            p("    \"offset, so `places` measures 0 for all five and cannot tell them from\",");
            p("    \"an item that does nothing.\",");
            p("    \"`fx` is 1 for an item whose icon tile a TextureFX rewrites every tick --\",");
            p("    \"the compass, and nothing else in this version. It is read off the FX\",");
            p("    \"class rather than matched by name; see emitItems.\",");
            p("    \"`spawns` is the entity this item puts into the world instead of a block:\",");
            p("    \"painting, boat, minecart or arrow, and `none` for everything else. It is\",");
            p("    \"why four items measure `places` as 0 and still do something. `spawnVariant`\",");
            p("    \"is ItemMinecart's own type field -- 0 cart, 1 chest, 2 furnace.\",");
            p("    \"`record` is ItemRecord's own name field -- the track a music disc\",");
            p("    \"plays -- and is present only on the two discs. It is the string handed\",");
            p("    \"to World.playRecord and the key streaming/13.mus files itself under.\",");
            p("    \"`palette` is whether the Creative hand offers it: everything this version\",");
            p("    \"defines except an ItemBlock whose block already has a carried form, and a\",");
            p("    \"handful of engine-only ids named in emitItems.\"");
            p("  ],");
            p("  \"version\": \"a1.1.2\",");
            p("  \"items\": [");
            for (int i = 0; i < rows.size(); i++) {
                p(rows.get(i) + (i + 1 < rows.size() ? "," : ""));
            }
            p("  ]");
            p("}");
        } catch (Throwable t) {
            t.printStackTrace();
            System.exit(1);
        }
    }

    // Everything in the 5x4x5 box the placement probe uses, back to air.
    private static void clearScene(java.lang.reflect.Method setBlock, Object world,
                                   int bx, int by, int bz) throws Exception {
        for (int x = bx - 2; x <= bx + 2; x++) {
            for (int z = bz - 2; z <= bz + 2; z++) {
                for (int y = by - 2; y <= by + 3; y++) {
                    setBlock.invoke(world, x, y, z, 0, 0);
                }
            }
        }
    }

    // The block names out of data/a1.1.2/blocks.json, read as text rather than
    // parsed: the two keys wanted sit next to each other on their own lines and
    // pulling in a JSON library for them would be the tail wagging the dog. The
    // names are ours in the first place -- see the note above.
    private static java.util.Map<Integer, String> blockNamesFromJson() throws Exception {
        java.util.Map<Integer, String> out = new java.util.HashMap<Integer, String>();
        java.io.File file = new java.io.File("data/a1.1.2/blocks.json");
        if (!file.isFile()) {
            return out;
        }
        String text = new String(java.nio.file.Files.readAllBytes(file.toPath()), "UTF-8");
        java.util.regex.Matcher m = java.util.regex.Pattern.compile(
            "\"id\"\\s*:\\s*(\\d+),\\s*\"name\"\\s*:\\s*\"([a-z0-9_]+)\"").matcher(text);
        while (m.find()) {
            out.put(Integer.valueOf(m.group(1)), m.group(2));
        }
        return out;
    }
}
