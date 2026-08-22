#include "framework.hpp"

#include "impl/storage/alpha_chunkfiles/chunk_path.hpp"

#include <string>

using namespace mc;
using namespace mc::alpha;

namespace {

std::string pathOf(i32 x, i32 z)
{
    ChunkPath path;
    if (!chunkFilePath("world", x, z, &path)) {
        return "<overflow>";
    }
    return std::string(path.view());
}

std::string base36Of(i32 value)
{
    char buffer[kMaxBase36 + 1];
    const usize length = base36(value, buffer);
    return std::string(buffer, length);
}

}  // namespace

TEST(base36_matches_java_integer_tostring)
{
    CHECK_EQ(base36Of(0), std::string("0"));
    CHECK_EQ(base36Of(35), std::string("z"));
    CHECK_EQ(base36Of(36), std::string("10"));
    CHECK_EQ(base36Of(44), std::string("18"));
    CHECK_EQ(base36Of(51), std::string("1f"));
    CHECK_EQ(base36Of(64), std::string("1s"));
    CHECK_EQ(base36Of(-13), std::string("-d"));
    CHECK_EQ(base36Of(-1), std::string("-1"));

    // Integer.toString(Integer.MIN_VALUE, 36). Negating this in 32 bits would
    // overflow, which is why the encoder widens first.
    CHECK_EQ(base36Of(-2147483647 - 1), std::string("-zik0zk"));
    CHECK_EQ(base36Of(2147483647), std::string("zik0zj"));
}

TEST(chunk_paths_match_the_alpha_layout)
{
    CHECK_EQ(pathOf(0, 0), std::string("world/0/0/c.0.0.dat"));

    // The documented worked example: the directories mask to unsigned, the
    // filename stays signed.
    CHECK_EQ(pathOf(-13, 44), std::string("world/1f/18/c.-d.18.dat"));

    // -1 & 63 == 63 == "1r", so negative chunks land in the high directories.
    CHECK_EQ(pathOf(-1, -1), std::string("world/1r/1r/c.-1.-1.dat"));

    // The directory wraps every 64 chunks while the filename keeps counting.
    CHECK_EQ(pathOf(64, 0), std::string("world/0/0/c.1s.0.dat"));
    CHECK_EQ(pathOf(63, 63), std::string("world/1r/1r/c.1r.1r.dat"));
}

TEST(chunk_dir_path_is_the_prefix_of_the_file_path)
{
    ChunkPath dir;
    CHECK(chunkDirPath("world", -13, 44, &dir));
    CHECK_EQ(std::string(dir.view()), std::string("world/1f/18"));

    const std::string file = pathOf(-13, 44);
    CHECK(file.compare(0, dir.length, std::string(dir.view())) == 0);
}

TEST(file_names_round_trip_through_parsing)
{
    // Covers both signs, the 64-chunk directory wrap, and multi-digit base36.
    //
    // 784426 and 784427 are the Far Lands: the chunks either side of where the
    // terrain generator's noise coordinate overflows a 32-bit int. Those are
    // real, reachable chunks that a1.1.2 generates and therefore has to be able
    // to write, and their names are five base36 digits where the rest of the
    // world's are one to four. 2000000 is the horizontal world limit in chunks,
    // which is as long as a name can legitimately get.
    const i32 coords[] = {0,      1,      -1,      13,     -13,     63,       64,
                          -64,    1295,   1296,    -1296,  100000,  -100000,  784426,
                          784427, -784427, 2000000, -2000000};

    for (const i32 x : coords) {
        for (const i32 z : coords) {
            ChunkPath path;
            CHECK(chunkFilePath("", x, z, &path));

            const std::string_view text = path.view();
            const usize lastSlash = text.rfind('/');
            CHECK(lastSlash != std::string_view::npos);

            i32 parsedX = 0;
            i32 parsedZ = 0;
            CHECK(parseChunkFileName(text.substr(lastSlash + 1), &parsedX, &parsedZ));
            CHECK_EQ(parsedX, x);
            CHECK_EQ(parsedZ, z);
        }
    }
}

TEST(malformed_file_names_are_rejected)
{
    i32 x = 0;
    i32 z = 0;

    // A world directory holds only chunk files, but an SD card holds whatever
    // the user put there -- thumbnails, editor backups, partial writes.
    CHECK(!parseChunkFileName("", &x, &z));
    CHECK(!parseChunkFileName("c.dat", &x, &z));
    CHECK(!parseChunkFileName("c.0.dat", &x, &z));
    CHECK(!parseChunkFileName("c.0.0.txt", &x, &z));
    CHECK(!parseChunkFileName("x.0.0.dat", &x, &z));
    CHECK(!parseChunkFileName("c..0.dat", &x, &z));
    CHECK(!parseChunkFileName("c.-.0.dat", &x, &z));
    CHECK(!parseChunkFileName("c.0.0.dat.bak", &x, &z));
    // Not base36.
    CHECK(!parseChunkFileName("c.0!.0.dat", &x, &z));
    // Wider than an i32, which must not wrap into a plausible coordinate.
    CHECK(!parseChunkFileName("c.zzzzzzzzzz.0.dat", &x, &z));

    // Uppercase is accepted on read: FAT is case-insensitive, so a world
    // copied around on a PC can come back with different case.
    CHECK(parseChunkFileName("c.-D.18.dat", &x, &z));
    CHECK_EQ(x, -13);
    CHECK_EQ(z, 44);
}

TEST(overlong_world_dir_fails_instead_of_overflowing)
{
    const std::string longDir(mc::alpha::kMaxChunkPathLength, 'a');
    ChunkPath path;
    CHECK(!chunkFilePath(longDir, 0, 0, &path));
}
