#include "core/settings/online_privacy.hpp"

namespace mc::settings {

bool onlinePrivacyNeedsFriendList(OnlinePrivacy privacy)
{
    return privacy != OnlinePrivacy::CodeOnly;
}

bool onlinePrivacyLocked(OnlinePrivacy privacy)
{
    (void)privacy;
    return true;
}

const char* onlinePrivacyToken(OnlinePrivacy privacy)
{
    switch (privacy) {
    case OnlinePrivacy::FriendsOnly:    return "friends";
    case OnlinePrivacy::FriendsAndCode: return "friends_and_code";
    case OnlinePrivacy::CodeOnly:       break;
    }
    return "code";
}

bool onlinePrivacyFromToken(std::string_view token, OnlinePrivacy* out)
{
    if (token == "code") {
        *out = OnlinePrivacy::CodeOnly;
        return true;
    }
    if (token == "friends") {
        *out = OnlinePrivacy::FriendsOnly;
        return true;
    }
    if (token == "friends_and_code") {
        *out = OnlinePrivacy::FriendsAndCode;
        return true;
    }
    return false;
}

const char* onlinePrivacyLabel(OnlinePrivacy privacy)
{
    switch (privacy) {
    case OnlinePrivacy::FriendsOnly:    return "Friends only";
    case OnlinePrivacy::FriendsAndCode: return "Friends and code";
    case OnlinePrivacy::CodeOnly:       break;
    }
    return "Code only";
}

const char* onlinePrivacyNote(OnlinePrivacy privacy)
{
    switch (privacy) {
    case OnlinePrivacy::FriendsOnly:    return "only your friends, with no code to pass on";
    case OnlinePrivacy::FriendsAndCode: return "your friends, and anybody you give the code to";
    case OnlinePrivacy::CodeOnly:       break;
    }
    return "anybody you read the join code out to";
}

}  // namespace mc::settings
