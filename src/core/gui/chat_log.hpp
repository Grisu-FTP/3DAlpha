#pragma once

// **The lines of text in the bottom left of the game screen** -- `lu`'s chat
// list (`GuiIngame.chatMessageList`, field `e`) and the part of
// `lu.a(FZII)V` that draws it. It is how this port tells the player something
// happened in the world, starting with a spawn the heap would not hold.
//
// Read out of the class file rather than remembered:
//
//   * `lu.a(String)` **wraps at 320 GUI pixels** by `kd.a(String)`, the font's
//     own width, character by character rather than by word -- the longest
//     prefix that fits, then the rest again. Each piece goes in at index 0, so
//     the newest line is first and a wrapped message still reads top to
//     bottom. The list is trimmed to 50.
//   * `lu.a()` -- updateTick -- **ages every line by one a tick.**
//   * The draw walks the first ten lines (twenty with the chat screen open),
//     skips any aged 200 or more, and fades the rest by
//     `t = clamp((1 - age / 200) * 10, 0, 1); alpha = 255 * t * t` -- fully
//     there for nine seconds, gone over the tenth. Line `i` sits `i * 9`
//     pixels above `height - 48`, on a `drawRect(2, y - 1, 322, y + 8)` in
//     black at half that alpha, with `drawStringWithShadow(line, 2, y,
//     0xFFFFFF + (alpha << 24))` over it.
//
// That strip of dimmed black with white shadowed text is also what Legacy
// Console Edition kept for its own messages, including the one this exists
// for -- "The maximum number of Minecarts in a world has been reached." -- so
// porting the original's overlay is porting that look too.
//
// Two differences, both stated:
//
//   * **Only the ten lines that can be shown are kept.** The other forty are
//     there in the original for the chat screen, which this port does not
//     have; keeping them would be memory nothing reads.
//   * **Posting the message that is already newest, while it is still
//     showing, restarts its fade instead of adding it again.** The placement
//     buttons repeat every five ticks while held, and ten copies of one
//     sentence would push everything else off the screen. The original never
//     posts one message twice in a row from the game itself, so it never had
//     to decide this.
//
// Fixed storage and no allocation: posting happens on a button press and
// ageing on the tick, and both are inside the frame.

#include "core/util/types.hpp"

#include <string_view>

namespace mc::gui {

// `sipush 320` in `lu.a(String)`.
inline constexpr int kChatWrapWidth = 320;

// The first ten lines are the ones the closed-chat draw walks.
inline constexpr int kChatShownLines = 10;

// A line aged this many ticks is not drawn at all.
inline constexpr int kChatLineTicks = 200;

// Where the newest line sits, how far apart lines are, and the strip behind
// them -- all in GUI pixels, which on the top screen are screen pixels.
inline constexpr int kChatBottomOffset = 48;
inline constexpr int kChatLineSpacing = 9;
inline constexpr int kChatLeft = 2;
inline constexpr int kChatStripWidth = 320;

// Bytes one line can hold, terminator included. A line is at most 320 pixels
// wide and a drawn glyph advances at least two, so ordinary text wraps long
// before this; only a run of empty cells (which advance one) or long UTF-8 can
// reach it, and then the line is cut at a code point.
inline constexpr int kChatLineBytes = 192;

struct ChatLine {
    char text[kChatLineBytes] = {};
    int age = 0;
};

// `255 * t * t` for the line's age, 0 for one that is not drawn.
int chatOpacity(int age);

class ChatLog {
public:
    // `lu.a(String)`: wrapped by the font's advances and added newest first.
    // A null `widths` -- no font -- wraps nothing, and nothing can draw it
    // anyway.
    void add(const u8* widths, std::string_view message);

    // The same, unless `message` is what was last posted and is still the
    // newest thing showing, in which case its lines start their ten seconds
    // over. See the header.
    void post(const u8* widths, std::string_view message);

    // `lu.a()`, `ticks` times.
    void tick(int ticks = 1);

    void clear();

    // Index 0 is the newest.
    int count() const { return count_; }
    const ChatLine& operator[](int i) const
    {
        return lines_[(head_ + i) % kChatShownLines];
    }

private:
    void pushLine(std::string_view line);
    // `add` without touching what `post` remembers. Returns how many lines
    // the message became.
    int addLines(const u8* widths, std::string_view message);

    ChatLine lines_[kChatShownLines];
    int head_ = 0;
    int count_ = 0;

    // What `post` compares against: the last message it added, and how many
    // lines that made. Cleared by any `add`, because then it is not newest.
    char posted_[kChatLineBytes * 2] = {};
    int postedLines_ = 0;
};

}  // namespace mc::gui
