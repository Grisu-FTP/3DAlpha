#include "core/settings/ini.hpp"

namespace mc::settings {

std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty()
           && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

bool parseInt(std::string_view text, int* out)
{
    const bool negative = !text.empty() && text.front() == '-';
    if (negative) {
        text.remove_prefix(1);
    }
    if (text.empty() || text.size() > 9) {
        return false;
    }
    int value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    *out = negative ? -value : value;
    return true;
}

bool nextEntry(std::string_view* text, std::string_view* key, std::string_view* value)
{
    while (!text->empty()) {
        const usize newline = text->find('\n');
        std::string_view line =
            newline == std::string_view::npos ? *text : text->substr(0, newline);
        *text = newline == std::string_view::npos ? std::string_view()
                                                  : text->substr(newline + 1);

        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const usize equals = line.find('=');
        if (equals == std::string_view::npos) {
            // A line with no `=` is a line somebody mistyped. Skipping it beats
            // failing the whole load: the other settings in the file are still
            // good, and the card is editable on a PC.
            continue;
        }
        *key = trim(line.substr(0, equals));
        *value = trim(line.substr(equals + 1));
        return true;
    }
    return false;
}

}  // namespace mc::settings
