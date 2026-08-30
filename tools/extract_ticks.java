// Which blocks tick, and how often -- read out of an original client jar.
//
// MAINTAINER TOOL. Run by hand, never by the build and never by a player; its
// output is merged into data/<version>/blocks.json and committed. Same
// arrangement as tools/extract_blocks.py and tools/genref.java: derive once
// from the authoritative source, keep the derivation runnable, argue with it
// later. See CONTRIBUTING.md.
//
//     java tools/extract_ticks.java <client.jar> [blockClass] [tickOnLoadField] [tickRateMethod] [materialClass] [canBurnMethod]
//
// Five facts per block, and the tick system is unbuildable without them:
//
//   * tickRandomly  -- Block.tickOnLoad[id], the boolean the world's random
//     tick loop consults 80 times per chunk per tick before it dispatches.
//   * tickRate      -- Block.tickRate(), the delay scheduleBlockUpdate adds to
//     worldTime, which is what makes water spread at 5 ticks and lava at 30.
//   * burnEncourage -- BlockFire.chanceToEncourageFire[id]. Non-zero is what
//     "this can catch fire" means to a fire block next to it, and the value is
//     how strongly it invites fire into the air nearby.
//   * burnCatch     -- BlockFire.abilityToCatchFire[id], rolled against a
//     per-direction chance to decide whether the block itself is consumed.
//   * canBurn       -- Material.getCanBurn(), which is a *different* set from
//     the two above: fourteen blocks have a burnable material and only six of
//     them are in the fire tables. Lava's ignition search reads this one.
//
// **Why a real JVM rather than reading the bytecode.** Both values are set by
// constructors, and constructors branch. `setTickRandomly` is called twice in
// BlockStationary -- false, then true again only for lava -- and BlockRedstoneOre
// is constructed twice from one class with a flag that decides it, so the
// unlit ore does not tick randomly and the lit one does. Interpreting that
// statically means guessing which branch each of the 70 constructions took;
// running the class initialiser means asking. The two answers disagree for
// three block ids, and all three disagreements are invisible until a world is
// left running.
//
// **How the two members were identified**, since the defaults are obfuscated
// names and a wrong guess here is a silent wrong world. Both come out of one
// method, `cn.h()` -- World.tickBlocks -- which is the only place either is
// read. Its inner loop, after picking a position from the update LCG, is:
//
//     getstatic  Field ly.o:[Z          // tickOnLoad
//     iload      11                     // the block id it just read
//     baload
//     ifeq       810
//     getstatic  Field ly.n:[Lly;        // blocksList
//     ...
//     invokevirtual Method ly.a:(Lcn;IIILjava/util/Random;)V   // updateTick
//
// So `o` is the gate and `n` is the dispatch table. `ly.b(Z)`, which writes
// `o[blockID]`, is setTickRandomly. `ly.a()I` is tickRate, read by
// `cn.h(IIII)` -- scheduleBlockUpdate -- as the delay it adds to worldTime.
// `ly` has four static boolean arrays, so the gate has to be named; it has
// exactly one static Block[], so the dispatch table does not.
//
// **The fire tables need no name at all.** BlockFire is the only block in the
// game holding two `int[]` as long as the block table, so it is found by that
// shape; which of the two is which is then settled by their contents, since
// `abilityToCatchFire` is larger than `chanceToEncourageFire` for every block
// in them. `Material.getCanBurn` does need naming, and `gb.e()` is identified
// by its one call site: `hn.a(Lcn;IIILjava/util/Random;)V` -- still lava
// looking for something above it to set alight -- asks exactly that question.
//
// Nothing here is version-specific except those default names, which are for
// a1.1.2_01. Pass others on the command line.

import java.io.File;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.net.URL;
import java.net.URLClassLoader;

public class extract_ticks {
    public static void main(String[] args) throws Exception {
        if (args.length < 1) {
            System.err.println("usage: java tools/extract_ticks.java <client.jar> [blockClass]");
            System.exit(2);
        }
        String blockClassName = args.length > 1 ? args[1] : "ly";
        String tickOnLoadName = args.length > 2 ? args[2] : "o";
        String tickRateName = args.length > 3 ? args[3] : "a";
        String materialClassName = args.length > 4 ? args[4] : "gb";
        String canBurnName = args.length > 5 ? args[5] : "e";

        URLClassLoader loader = new URLClassLoader(
                new URL[] { new File(args[0]).toURI().toURL() },
                extract_ticks.class.getClassLoader());
        Class<?> block = Class.forName(blockClassName, true, loader);

        // blocksList is found by type -- there is exactly one static Block[]
        // and requiring that keeps a renamed field from going unnoticed. The
        // gate array cannot be, because Block carries four static boolean[].
        Object[] blocksList = (Object[]) uniqueStaticArray(block, block.getName() + "[]");
        boolean[] tickOnLoad = (boolean[]) namedStatic(block, tickOnLoadName, boolean[].class);
        Method tickRate = block.getDeclaredMethod(tickRateName);
        if (tickRate.getReturnType() != int.class) {
            throw new IllegalStateException(blockClassName + "." + tickRateName
                    + "() does not return int; it is not tickRate");
        }
        tickRate.setAccessible(true);
        if (tickOnLoad.length != blocksList.length) {
            throw new IllegalStateException("tickOnLoad is " + tickOnLoad.length
                    + " long and blocksList is " + blocksList.length
                    + "; they index the same ids and must match");
        }

        // BlockFire's two tables, found by shape rather than by name.
        int[][] fireTables = findFireTables(blocksList);
        int[] burnEncourage = fireTables[0];
        int[] burnCatch = fireTables[1];

        Class<?> material = Class.forName(materialClassName, true, loader);
        Method canBurn = material.getDeclaredMethod(canBurnName);
        if (canBurn.getReturnType() != boolean.class) {
            throw new IllegalStateException(materialClassName + "." + canBurnName
                    + "() does not return boolean; it is not getCanBurn");
        }
        canBurn.setAccessible(true);
        Field materialField = uniqueFieldOfType(block, material);
        materialField.setAccessible(true);

        System.out.println("{");
        boolean first = true;
        for (int id = 0; id < blocksList.length; id++) {
            if (blocksList[id] == null) continue;
            if (!first) System.out.println(",");
            first = false;
            System.out.printf("  \"%d\": {\"class\": \"%s\", \"tickRandomly\": %b, \"tickRate\": %d,"
                            + " \"burnEncourage\": %d, \"burnCatch\": %d, \"canBurn\": %b}",
                    id, blocksList[id].getClass().getName(), tickOnLoad[id],
                    (Integer) tickRate.invoke(blocksList[id]),
                    burnEncourage[id], burnCatch[id],
                    (Boolean) canBurn.invoke(materialField.get(blocksList[id])));
        }
        System.out.println();
        System.out.println("}");
    }

    // The two int[] on BlockFire. Identified by shape -- one block, two
    // **instance** arrays exactly as long as the block table -- and then told
    // apart by content:
    // abilityToCatchFire is greater than chanceToEncourageFire wherever either
    // is set, for every block in a1.1.2. Returns {encourage, catch}.
    private static int[][] findFireTables(Object[] blocksList) throws Exception {
        int[][] found = null;
        for (Object b : blocksList) {
            if (b == null) continue;
            java.util.List<int[]> tables = new java.util.ArrayList<>();
            for (Field f : b.getClass().getDeclaredFields()) {
                if (f.getType() != int[].class) continue;
                // Static ones are Block's own tables -- lightOpacity and its
                // neighbours -- which every block inherits and none owns.
                if (java.lang.reflect.Modifier.isStatic(f.getModifiers())) continue;
                f.setAccessible(true);
                int[] v = (int[]) f.get(b);
                if (v != null && v.length == blocksList.length) tables.add(v);
            }
            if (tables.size() != 2) continue;
            if (found != null) {
                throw new IllegalStateException("more than one block carries two block-sized "
                        + "int[]; the fire tables have to be named by hand");
            }
            found = new int[][] { tables.get(0), tables.get(1) };
        }
        if (found == null) throw new IllegalStateException("no block carries the two fire tables");

        int a = 0, b = 0;
        for (int i = 0; i < found[0].length; i++) { a += found[0][i]; b += found[1][i]; }
        return b >= a ? found : new int[][] { found[1], found[0] };
    }

    private static Field uniqueFieldOfType(Class<?> owner, Class<?> type) {
        Field found = null;
        for (Field f : owner.getDeclaredFields()) {
            if (f.getType() != type) continue;
            if (found != null) {
                throw new IllegalStateException("more than one " + type.getName()
                        + " field on " + owner.getName());
            }
            found = f;
        }
        if (found == null) throw new IllegalStateException("no " + type.getName() + " field");
        return found;
    }

    private static Object namedStatic(Class<?> owner, String name, Class<?> type) throws Exception {
        Field f = owner.getDeclaredField(name);
        if (f.getType() != type) {
            throw new IllegalStateException(owner.getName() + "." + name + " is a "
                    + f.getType().getCanonicalName() + ", not a " + type.getCanonicalName());
        }
        f.setAccessible(true);
        return f.get(null);
    }

    private static Object uniqueStaticArray(Class<?> owner, String typeName) throws Exception {
        Field found = null;
        for (Field f : owner.getDeclaredFields()) {
            if (!f.getType().isArray()) continue;
            if (!f.getType().getCanonicalName().equals(typeName)) continue;
            if (found != null) {
                throw new IllegalStateException("more than one static " + typeName
                        + " on " + owner.getName() + "; name it by hand");
            }
            found = f;
        }
        if (found == null) throw new IllegalStateException("no static " + typeName);
        found.setAccessible(true);
        return found.get(null);
    }

}
