#include "core/settings/control_scheme.hpp"

namespace mc::settings {

Stick moveStick(ControlScheme scheme)
{
    switch (scheme) {
    case ControlScheme::Old3DS: return Stick::Dpad;
    case ControlScheme::New3DS:
    case ControlScheme::Old3DSAlt:
    default:                    return Stick::CirclePad;
    }
}

Stick lookStick(ControlScheme scheme)
{
    switch (scheme) {
    case ControlScheme::Old3DS:    return Stick::CirclePad;
    case ControlScheme::Old3DSAlt: return Stick::Dpad;
    case ControlScheme::New3DS:
    default:                       return Stick::CStick;
    }
}

const char* controlSchemeToken(ControlScheme scheme)
{
    switch (scheme) {
    case ControlScheme::Old3DS:    return "old3ds";
    case ControlScheme::Old3DSAlt: return "old3ds-alt";
    case ControlScheme::New3DS:
    default:                       return "new3ds";
    }
}

const char* controlSchemeLabel(ControlScheme scheme)
{
    switch (scheme) {
    case ControlScheme::Old3DS:    return "Old 3DS";
    case ControlScheme::Old3DSAlt: return "Old 3DS Alt";
    case ControlScheme::New3DS:
    default:                       return "New 3DS";
    }
}

const char* stickLabel(Stick stick)
{
    switch (stick) {
    case Stick::Dpad:   return "D-Pad";
    // Nintendo's own spelling on the console and in its manual, so it is the
    // one a player reads off the row and finds under their thumb.
    case Stick::CStick: return "C Stick";
    case Stick::CirclePad:
    default:            return "Circle Pad";
    }
}

bool controlSchemeFromToken(std::string_view token, ControlScheme* out)
{
    for (int i = 0; i < kControlSchemeCount; ++i) {
        const ControlScheme scheme = ControlScheme(i);
        if (token == controlSchemeToken(scheme)) {
            *out = scheme;
            return true;
        }
    }
    return false;
}

ControlScheme defaultControlScheme(bool isNew3DS)
{
    return isNew3DS ? ControlScheme::New3DS : ControlScheme::Old3DS;
}

}  // namespace mc::settings
