// The server list file, address parsing and the login name. See server_list.hpp.

#include "core/net/server_list.hpp"

#include "core/net/wire.hpp"
#include "core/settings/ini.hpp"
#include "core/texture/font.hpp"

#include <cstdio>

namespace mc::net {

namespace {

constexpr usize kMaxListBytes = 64u << 10;

// Newlines would end the entry early on the way back in.
std::string oneLine(std::string_view text)
{
    std::string out(settings::trim(text));
    for (char& c : out) {
        if (c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return out;
}

}  // namespace

bool loadServerList(io::FileSystem& fs, const char* path, std::vector<ServerEntry>* out)
{
    out->clear();
    std::vector<u8> bytes;
    if (!fs.readFile(path, &bytes, kMaxListBytes)) {
        return false;
    }

    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::string_view key;
    std::string_view value;
    while (settings::nextEntry(&text, &key, &value)) {
        if (key == "name") {
            out->push_back(ServerEntry{std::string(value), std::string(), kDefaultPort});
        } else if (key == "address") {
            // An address with no name above it still deserves a row.
            if (out->empty() || !out->back().address.empty()) {
                out->push_back(ServerEntry{std::string(value), std::string(), kDefaultPort});
            }
            // **A `host:port` written into `address=` is split here**, which is
            // what every list written before the port had a row of its own
            // holds. The two halves land in the two fields and the next save
            // writes them apart; an address that will not parse at all is kept
            // verbatim so the player can see what they typed and fix it.
            std::string host;
            u16 port = kDefaultPort;
            if (parseAddress(value, &host, &port, out->back().port)) {
                out->back().address = host;
                out->back().port = port;
            } else {
                out->back().address = std::string(value);
            }
        } else if (key == "port" && !out->empty()) {
            parsePort(value, &out->back().port);
        }
    }
    return true;
}

bool saveServerList(io::FileSystem& fs, const char* path, const std::vector<ServerEntry>& list)
{
    std::string text = "# 3DAlpha multiplayer servers. One name= line per server, then its\n"
                       "# address= (the host) and its port= (25565 when left out).\n"
                       "# A host:port written into address= is split on the way in.\n";
    for (const ServerEntry& entry : list) {
        char port[16];
        std::snprintf(port, sizeof(port), "%u", unsigned(entry.port));
        text += "\nname=";
        text += oneLine(entry.name);
        text += "\naddress=";
        text += oneLine(entry.address);
        text += "\nport=";
        text += port;
        text += '\n';
    }
    return fs.writeFileAtomic(path,
                              ConstByteSpan(reinterpret_cast<const u8*>(text.data()), text.size()));
}

bool parseAddress(std::string_view address, std::string* host, u16* port, u16 fallback)
{
    address = settings::trim(address);
    std::string_view hostPart = address;
    std::string_view portPart;
    bool hasPort = false;

    if (!address.empty() && address.front() == '[') {
        const usize close = address.find(']');
        if (close == std::string_view::npos) {
            return false;
        }
        hostPart = address.substr(1, close - 1);
        std::string_view rest = address.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') {
                return false;
            }
            portPart = rest.substr(1);
            hasPort = true;
        }
    } else {
        const usize colon = address.find(':');
        if (colon != std::string_view::npos
            && address.find(':', colon + 1) == std::string_view::npos) {
            hostPart = address.substr(0, colon);
            portPart = address.substr(colon + 1);
            hasPort = true;
        }
    }

    hostPart = settings::trim(hostPart);
    if (hostPart.empty()) {
        return false;
    }

    u16 parsedPort = fallback;
    if (hasPort) {
        int value = 0;
        if (!settings::parseInt(settings::trim(portPart), &value) || value < 1 || value > 65535) {
            return false;
        }
        parsedPort = u16(value);
    }

    *host = std::string(hostPart);
    *port = parsedPort;
    return true;
}

bool parsePort(std::string_view text, u16* out)
{
    int value = 0;
    if (!settings::parseInt(settings::trim(text), &value) || value < 1 || value > 65535) {
        return false;
    }
    *out = u16(value);
    return true;
}

std::string utf16ToUtf8(const u16* units, usize maxUnits)
{
    std::string out;
    for (usize i = 0; i < maxUnits && units[i] != 0; ++i) {
        u32 cp = units[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < maxUnits && units[i + 1] >= 0xDC00
            && units[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u32(units[i + 1]) - 0xDC00);
            ++i;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        appendUtf8(cp, &out);
    }
    return out;
}

std::string usernameFrom(std::string_view utf8, int maxChars)
{
    std::string out;
    int chars = 0;
    bool meaningful = false;

    usize pos = 0;
    while (pos < utf8.size() && chars < maxChars) {
        u32 cp = nextUtf8(utf8, &pos);
        const bool unsafe = cp <= 0x20 || cp == 0xA7 || cp == '/' || cp == '\\' || cp == ':'
                            || cp == '*' || cp == '?' || cp == '"' || cp == '<' || cp == '>'
                            || cp == '|' || cp == '.' || cp == 0x7F;
        if (unsafe || texture::fontGlyph(cp) < 0) {
            cp = '_';
        }
        if (cp != '_') {
            meaningful = true;
        }
        appendUtf8(cp, &out);
        ++chars;
    }

    // Trailing padding a screen name may carry is not part of the name.
    while (!out.empty() && out.back() == '_' && !meaningful) {
        out.pop_back();
    }
    return meaningful ? out : std::string(kFallbackUsername);
}

}  // namespace mc::net
