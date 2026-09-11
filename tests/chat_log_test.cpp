// The chat lines on the top screen -- `lu`'s list and its draw -- and the
// sentence a refused spawn puts there.
//
// The numbers are the class file's: a 320-pixel wrap, ten lines shown, 200
// ticks to live with the last twenty spent fading, nine pixels between lines
// and the newest 48 above the bottom. What is tested is that each of them is
// that number, and that the one thing added here -- a repeated post restarting
// its line instead of stacking copies -- does only that.

#include "core/gui/chat_log.hpp"
#include "core/item/use.hpp"
#include "core/render/chat_mesh.hpp"
#include "core/texture/font.hpp"
#include "core/world/sign_store.hpp"
#include "framework.hpp"
#include "low_heap.hpp"

#include <cstring>
#include <string>

using namespace mc;
using mc::gui::ChatLog;

namespace {

// Every glyph six pixels wide, so a line is 53 characters and the arithmetic
// is easy to follow.
struct EvenWidths {
    u8 widths[256];
    EvenWidths()
    {
        for (int i = 0; i < 256; ++i) {
            widths[i] = 6;
        }
    }
};

texture::FontImage standInFont()
{
    texture::FontImage font;
    font.rgba.assign(texture::kFontBytes, 0xFF);
    for (int i = 0; i < 256; ++i) {
        font.widths[i] = 6;
    }
    return font;
}

}  // namespace

TEST(chat_opacity_is_full_for_nine_seconds_and_gone_by_ten)
{
    CHECK_EQ(gui::chatOpacity(0), 255);
    CHECK_EQ(gui::chatOpacity(170), 255);
    // `(1 - 180 / 200) * 10` is a hair under 1 in doubles, and the original
    // truncates: 254, exactly as the class file computes it.
    CHECK_EQ(gui::chatOpacity(180), 254);
    CHECK_EQ(gui::chatOpacity(181), 230);
    // Half way through the last second: 0.5 squared.
    CHECK_EQ(gui::chatOpacity(190), 63);
    CHECK_EQ(gui::chatOpacity(199), 0);
    CHECK_EQ(gui::chatOpacity(200), 0);
    CHECK_EQ(gui::chatOpacity(5000), 0);
}

TEST(chat_lines_go_in_newest_first)
{
    EvenWidths w;
    ChatLog log;
    log.add(w.widths, "first");
    log.add(w.widths, "second");
    CHECK_EQ(log.count(), 2);
    CHECK(std::strcmp(log[0].text, "second") == 0);
    CHECK(std::strcmp(log[1].text, "first") == 0);
}

TEST(a_long_message_wraps_at_320_pixels_and_reads_top_to_bottom)
{
    EvenWidths w;
    ChatLog log;
    // Sixty characters at six pixels is 360; 53 fit in 320.
    log.add(w.widths, std::string(53, 'a') + std::string(7, 'b'));
    CHECK_EQ(log.count(), 2);
    // The piece that fits went in first, so it is the older, upper line.
    CHECK_EQ(int(std::strlen(log[1].text)), 53);
    CHECK_EQ(log[1].text[0], 'a');
    CHECK(std::strcmp(log[0].text, "bbbbbbb") == 0);
}

TEST(the_limit_message_wraps_into_two_lines)
{
    EvenWidths w;
    ChatLog log;
    log.add(w.widths, item::limitMessage(item::LimitedEntity::Minecart));
    CHECK_EQ(log.count(), 2);
    const std::string joined = std::string(log[1].text) + log[0].text;
    CHECK(joined == "The maximum number of Minecarts in a world has been reached.");
}

TEST(only_the_ten_lines_that_can_be_shown_are_kept)
{
    EvenWidths w;
    ChatLog log;
    for (int i = 0; i < 25; ++i) {
        log.add(w.widths, std::to_string(i));
    }
    CHECK_EQ(log.count(), gui::kChatShownLines);
    CHECK(std::strcmp(log[0].text, "24") == 0);
    CHECK(std::strcmp(log[9].text, "15") == 0);
}

TEST(chat_lines_age_on_the_tick_and_stop_at_the_limit)
{
    EvenWidths w;
    ChatLog log;
    log.add(w.widths, "hello");
    log.tick();
    log.tick(4);
    CHECK_EQ(log[0].age, 5);
    log.tick(10000);
    CHECK(log[0].age >= gui::kChatLineTicks);
    log.tick(1);
    CHECK(log[0].age >= gui::kChatLineTicks);
}

TEST(posting_the_newest_message_again_restarts_it_instead_of_stacking)
{
    EvenWidths w;
    ChatLog log;
    const char* message = item::limitMessage(item::LimitedEntity::Boat);
    log.post(w.widths, message);
    const int lines = log.count();
    log.tick(150);
    log.post(w.widths, message);
    CHECK_EQ(log.count(), lines);
    for (int i = 0; i < lines; ++i) {
        CHECK_EQ(log[i].age, 0);
    }

    // A different message is added as usual...
    log.post(w.widths, "something else");
    CHECK_EQ(log.count(), lines + 1);
    // ...and the first one is no longer newest, so it goes in again.
    log.post(w.widths, message);
    CHECK_EQ(log.count(), 2 * lines + 1);
}

TEST(a_message_that_has_faded_is_posted_again_rather_than_revived)
{
    EvenWidths w;
    ChatLog log;
    log.post(w.widths, "gone");
    log.tick(gui::kChatLineTicks);
    log.post(w.widths, "gone");
    CHECK_EQ(log.count(), 2);
}

TEST(a_line_too_long_for_its_buffer_is_cut_at_a_code_point)
{
    // No font: nothing wraps, so the whole message is one line and has to be
    // cut to fit. Two-byte characters, so an even cut is a split one.
    ChatLog log;
    std::string message;
    for (int i = 0; i < gui::kChatLineBytes; ++i) {
        message += "\xC3\xA9";  // U+00E9
    }
    log.add(nullptr, message);
    const usize length = std::strlen(log[0].text);
    CHECK(length < usize(gui::kChatLineBytes));
    CHECK_EQ(int(length % 2), 0);
}

TEST(chat_text_is_a_shadow_then_the_text_for_each_visible_line)
{
    const texture::FontImage font = standInFont();
    ChatLog log;
    log.add(font.widths, "old");
    log.tick(gui::kChatLineTicks);  // faded: not drawn
    log.add(font.widths, "ab");
    log.add(font.widths, "cde");

    mesh::DetailVertex verts[256];
    render::ChatSpan spans[gui::kChatShownLines];
    const int count = render::buildChatText(log, font, 240, verts, 256, spans,
                                            gui::kChatShownLines);
    CHECK_EQ(count, 2);

    // Newest at `height - 48`, the next nine pixels above it.
    CHECK_EQ(spans[0].y, 240 - 48);
    CHECK_EQ(spans[1].y, 240 - 48 - 9);
    CHECK_EQ(spans[0].alpha, 255);

    // Two quads a character: shadow and text.
    CHECK_EQ(spans[0].vertices, 3 * 2 * 4);
    CHECK_EQ(spans[1].vertices, 2 * 2 * 4);
    CHECK_EQ(spans[1].firstVertex, spans[0].vertices);

    // The shadow is one pixel right and down, in the colour divided by four;
    // the text is white at x = 2.
    const mesh::DetailVertex& shadow = verts[spans[0].firstVertex];
    const mesh::DetailVertex& text = verts[spans[0].firstVertex + 3 * 4];
    CHECK_EQ(int(shadow.x), (gui::kChatLeft + 1) * render::kChatUnitsPerPixel);
    CHECK_EQ(int(shadow.y), (240 - 48 + 1) * render::kChatUnitsPerPixel);
    CHECK_EQ(int(shadow.r), 0x3F);
    CHECK_EQ(int(text.x), gui::kChatLeft * render::kChatUnitsPerPixel);
    CHECK_EQ(int(text.r), 0xFF);

    // 'c' is glyph 99: column 3, row 6 of the sheet.
    CHECK_EQ(int(text.u), 3 * 8 * mesh::kUvUnitsPerAtlas / texture::kFontEdge);
    CHECK_EQ(int(text.v), 6 * 8 * mesh::kUvUnitsPerAtlas / texture::kFontEdge);
}

TEST(chat_text_needs_a_font)
{
    ChatLog log;
    log.add(nullptr, "hello");
    texture::FontImage none;
    mesh::DetailVertex verts[64];
    render::ChatSpan spans[gui::kChatShownLines];
    CHECK_EQ(render::buildChatText(log, none, 240, verts, 64, spans, gui::kChatShownLines),
             0);
}

TEST(chat_text_stops_at_the_buffer_rather_than_past_it)
{
    const texture::FontImage font = standInFont();
    ChatLog log;
    log.add(font.widths, "abcdefgh");
    mesh::DetailVertex verts[20];
    render::ChatSpan spans[gui::kChatShownLines];
    const int count =
        render::buildChatText(log, font, 240, verts, 20, spans, gui::kChatShownLines);
    CHECK_EQ(count, 1);
    CHECK(spans[0].vertices <= 20);
    CHECK_EQ(spans[0].vertices % 4, 0);
}

TEST(every_limited_entity_has_a_sentence)
{
    CHECK(item::limitMessage(item::LimitedEntity::None) == nullptr);
    for (item::LimitedEntity which :
         {item::LimitedEntity::Painting, item::LimitedEntity::Arrow, item::LimitedEntity::Boat,
          item::LimitedEntity::Minecart, item::LimitedEntity::Sign,
          item::LimitedEntity::DroppedItem}) {
        const char* message = item::limitMessage(which);
        CHECK(message != nullptr);
        CHECK(std::strncmp(message, "The maximum number of ", 22) == 0);
    }
}

TEST(a_refusal_is_found_by_the_count_that_rose)
{
    world::SignStore signs;
    item::EntityPools pools;
    pools.signs = &signs;
    for (int i = 0; i < world::SignStore::kInitialCapacity; ++i) {
        CHECK(signs.put(i32(i), 64, 0, false, 0) >= 0);
    }
    const item::RefusalMark mark = item::markRefusals(pools);
    CHECK(item::refusedSince(pools, mark) == item::LimitedEntity::None);
    {
        test::LowHeap low;
        CHECK_EQ(signs.put(-1, 64, 0, false, 0), -1);
    }
    CHECK(item::refusedSince(pools, mark) == item::LimitedEntity::Sign);
}
