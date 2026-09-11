#include "core/gui/chat_log.hpp"

#include "core/texture/font.hpp"

#include <cstring>

namespace mc::gui {

int chatOpacity(int age)
{
    if (age >= kChatLineTicks) {
        return 0;
    }
    // In doubles, as the class file does it: `ko.b / 200.0`, `1.0 - that`,
    // `* 10.0`, clamped, squared, `(int)(255.0 * that)`.
    double t = double(age) / double(kChatLineTicks);
    t = 1.0 - t;
    t *= 10.0;
    if (t < 0.0) {
        t = 0.0;
    }
    if (t > 1.0) {
        t = 1.0;
    }
    t *= t;
    return int(255.0 * t);
}

void ChatLog::pushLine(std::string_view line)
{
    // A code point is never split: cut back to the start of the last one that
    // fits whole.
    usize length = line.size();
    if (length > usize(kChatLineBytes - 1)) {
        length = usize(kChatLineBytes - 1);
        while (length > 0 && (u8(line[length]) & 0xC0) == 0x80) {
            --length;
        }
    }
    head_ = (head_ + kChatShownLines - 1) % kChatShownLines;
    ChatLine& slot = lines_[head_];
    std::memcpy(slot.text, line.data(), length);
    slot.text[length] = '\0';
    slot.age = 0;
    if (count_ < kChatShownLines) {
        ++count_;
    }
}

int ChatLog::addLines(const u8* widths, std::string_view message)
{
    // `while (width(s) > 320)`: the longest prefix that fits goes in, and the
    // rest is measured again. At least one character is taken each time, as
    // the original's `i = 1` start does, so a glyph wider than the whole strip
    // cannot stall it.
    int lines = 0;
    while (widths != nullptr && texture::textWidth(widths, message) > kChatWrapWidth) {
        usize cut = texture::fitBytes(widths, message, kChatWrapWidth);
        if (cut == 0) {
            texture::nextCodepoint(message, &cut);
        }
        pushLine(message.substr(0, cut));
        message.remove_prefix(cut);
        ++lines;
    }
    pushLine(message);
    return lines + 1;
}

void ChatLog::add(const u8* widths, std::string_view message)
{
    addLines(widths, message);
    posted_[0] = '\0';
    postedLines_ = 0;
}

void ChatLog::post(const u8* widths, std::string_view message)
{
    const bool fits = message.size() < sizeof(posted_);
    if (fits && postedLines_ > 0 && std::strlen(posted_) == message.size()
        && std::memcmp(posted_, message.data(), message.size()) == 0
        && (*this)[0].age < kChatLineTicks) {
        for (int i = 0; i < postedLines_; ++i) {
            lines_[(head_ + i) % kChatShownLines].age = 0;
        }
        return;
    }

    const int lines = addLines(widths, message);
    if (!fits) {
        posted_[0] = '\0';
        postedLines_ = 0;
        return;
    }
    // A message longer than the ring keeps only its last ten lines, and those
    // are the ones a repeat restarts.
    postedLines_ = lines < kChatShownLines ? lines : kChatShownLines;
    std::memcpy(posted_, message.data(), message.size());
    posted_[message.size()] = '\0';
}

void ChatLog::tick(int ticks)
{
    for (int i = 0; i < count_; ++i) {
        ChatLine& line = lines_[(head_ + i) % kChatShownLines];
        // Saturating, so a line left for a very long session cannot wrap
        // round to young again.
        if (line.age < kChatLineTicks) {
            line.age += ticks;
        }
    }
}

void ChatLog::clear()
{
    head_ = 0;
    count_ = 0;
    posted_[0] = '\0';
    postedLines_ = 0;
}

}  // namespace mc::gui
