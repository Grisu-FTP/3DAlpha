#include "core/net/ac_client.hpp"

#include "core/util/ed25519.hpp"

#include <cstring>

namespace mc::net::ac {

namespace {

// The game tag a session is filed under. Cross-play is allowed, and a player
// should still be able to see what they are joining.
constexpr char kGame[] = "3ds";

}  // namespace

Client::Client()
{
    scratch_.reserve(kMaxDatagram);
}

void Client::begin(const Endpoint& server, const Login& login, UdpTransport* transport,
                   u32 nowMs)
{
    transport_ = transport;
    server_ = server;
    login_ = login;

    state_ = State::Connecting;
    message_.clear();
    displayName_.clear();
    accountHandle_.clear();
    keyFingerprint_.clear();
    firstClaim_ = false;
    reflexive_ = Endpoint();
    sessionId_ = 0;
    joinCode_.clear();
    linkCode_.clear();
    linkCodeSeconds_ = 0;
    sessions_.clear();
    event_ = Event::None;
    queued_ = Pending::None;
    lastHeardMs_ = nowMs;
    clockMs_ = nowMs;
    helloSends_ = 0;
    authSends_ = 0;
    handshakeRestarted_ = false;
    staleAuthFails_ = 0;
    relogged_ = false;

    startRequest(Pending::Hello, nowMs);
}

void Client::end()
{
    state_ = State::Idle;
    pending_ = Pending::None;
    queued_ = Pending::None;
    transport_ = nullptr;
    std::memset(token_, 0, sizeof(token_));
}

Event Client::takeEvent()
{
    const Event event = event_;
    event_ = Event::None;
    return event;
}

bool Client::transmit(const ClientMsg& msg)
{
    if (transport_ == nullptr) {
        return false;
    }
    if (!encodeClient(msg, &scratch_)) {
        return false;
    }
    return transport_->send(server_, scratch_.data(), scratch_.size());
}

void Client::fail(const std::string& reason)
{
    state_ = State::Failed;
    pending_ = Pending::None;
    queued_ = Pending::None;
    message_ = reason;
    event_ = Event::Refused;
}

void Client::startRequest(Pending pending, u32 nowMs)
{
    pending_ = pending;
    startedAtMs_ = nowMs;
    sentAtMs_ = nowMs;
    sendPending(nowMs);
}

// A request pressed before the login has landed is remembered rather than
// refused. The Profile screen opens and the player presses Link on the next
// frame, which is a long way before a 3DS on wireless has finished saying hello.
void Client::queue(Pending pending)
{
    if (state_ == State::Idle || state_ == State::Failed) {
        return;
    }
    // **Timed from now, not from the last send.** `sentAtMs_` is when the
    // previous request went out -- the login, for a join typed a minute later --
    // and a request started from it had run out before it left.
    if (pending_ == Pending::None && state_ == State::Ready) {
        startRequest(pending, clockMs_);
        return;
    }
    queued_ = pending;
}

void Client::sendPending(u32 nowMs)
{
    (void)nowMs;
    ClientMsg msg;
    std::memcpy(msg.token, token_, kTokenSize);

    switch (pending_) {
    case Pending::None:
        return;
    case Pending::Hello:
        ++helloSends_;
        msg.kind = ClientKind::Hello;
        msg.protocol = kProtocol;
        msg.identity = login_.identity;
        std::memcpy(msg.publicKey, login_.publicKey, kPublicKeySize);
        break;
    case Pending::Auth: {
        ++authSends_;
        msg.kind = ClientKind::Auth;
        std::vector<u8> payload;
        authPayload(login_.identity, nonce_, &payload);
        util::ed25519Sign(login_.seed, login_.publicKey, payload.data(), payload.size(),
                          msg.signature);
        msg.hasPlatformName = login_.hasPlatformName;
        msg.platformName = login_.platformName;
        break;
    }
    case Pending::LinkCode:
        msg.kind = ClientKind::RequestLinkCode;
        break;
    case Pending::Unlink:
        msg.kind = ClientKind::Unlink;
        break;
    case Pending::Host:
        msg.kind = ClientKind::HostSession;
        msg.worldName = hostWorldName_;
        msg.game = kGame;
        msg.maxPlayers = hostMaxPlayers_;
        msg.locked = hostLocked_;
        if (transport_ != nullptr) {
            msg.local = transport_->localEndpoint();
            msg.hasLocal = msg.local.valid();
        }
        break;
    case Pending::Close:
        msg.kind = ClientKind::CloseSession;
        break;
    case Pending::List:
        msg.kind = ClientKind::ListSessions;
        msg.hasGameFilter = true;
        msg.game = kGame;
        break;
    case Pending::Join:
        msg.kind = ClientKind::JoinRequest;
        msg.hasJoinCode = joinByCode_;
        msg.joinCode = joinCodeWanted_;
        msg.sessionId = joinByCode_ ? 0 : joinIdWanted_;
        if (transport_ != nullptr) {
            msg.local = transport_->localEndpoint();
            msg.hasLocal = msg.local.valid();
        }
        break;
    }

    transmit(msg);
}

void Client::pump(u32 nowMs)
{
    if (state_ == State::Idle || state_ == State::Failed || transport_ == nullptr) {
        return;
    }

    // **Back from a suspension.** Nothing was read while the loop was stopped,
    // so the time away is nobody's silence: every clock that measures the
    // server moves forward by it. A request asked for during the gap was
    // started on the old clock and comes forward with them. The keep-alive
    // goes now, because a NAT may have dropped the mapping meanwhile and the
    // server has to learn the new one before it can answer anything.
    const u32 gap = nowMs - clockMs_;
    clockMs_ = nowMs;
    if (gap > kSuspendGapMs) {
        lastHeardMs_ += gap;
        startedAtMs_ += gap;
        sentAtMs_ += gap;
        if (state_ == State::Ready) {
            keepAliveAtMs_ = nowMs;
            ClientMsg msg;
            msg.kind = ClientKind::Keepalive;
            std::memcpy(msg.token, token_, kTokenSize);
            transmit(msg);
        }
    }

    // A console that has heard nothing for this long has lost the server --
    // wireless off, a walk out of range, or a server that stopped. Said plainly
    // rather than left as a screen that never changes.
    if (state_ == State::Ready && nowMs - lastHeardMs_ > kServerTimeoutMs) {
        fail("Lost the connection to the server.");
        return;
    }

    if (pending_ != Pending::None) {
        if (nowMs - startedAtMs_ > kRequestTimeoutMs) {
            // A login that never landed is a different sentence from an errand
            // that did not: one means the server cannot be reached at all.
            fail(state_ == State::Ready ? "The server did not answer."
                                        : "Could not reach the server.");
            return;
        }
        if (nowMs - sentAtMs_ >= kRetryMs) {
            sentAtMs_ = nowMs;
            sendPending(nowMs);
        }
        return;
    }

    if (state_ != State::Ready) {
        return;
    }

    if (queued_ != Pending::None) {
        const Pending next = queued_;
        queued_ = Pending::None;
        startRequest(next, nowMs);
        return;
    }

    // **The keep-alive is not politeness.** It is what holds the NAT mapping
    // open and what re-teaches the server this console's address after the
    // mapping moves, which is every time a lid closes and opens.
    if (nowMs - keepAliveAtMs_ >= kKeepAliveMs) {
        keepAliveAtMs_ = nowMs;
        ClientMsg msg;
        msg.kind = ClientKind::Keepalive;
        std::memcpy(msg.token, token_, kTokenSize);
        transmit(msg);
    }
}

bool Client::onDatagram(const u8* data, usize size, u32 nowMs)
{
    ServerMsg msg;
    if (!decodeServer(data, size, &msg)) {
        return false;
    }
    lastHeardMs_ = nowMs;

    switch (msg.kind) {
    case ServerKind::Challenge:
        // Only ever the answer to a Hello. A second one arriving because the
        // first Hello was merely slow would otherwise restart the exchange with
        // a nonce the server has already forgotten.
        if (pending_ != Pending::Hello) {
            return true;
        }
        std::memcpy(nonce_, msg.nonce, kNonceSize);
        reflexive_ = msg.reflexive;
        authSends_ = 0;
        startRequest(Pending::Auth, nowMs);
        return true;

    case ServerKind::AuthOk:
        if (state_ == State::Ready && pending_ != Pending::Auth) {
            return true;
        }
        std::memcpy(token_, msg.token, kTokenSize);
        displayName_ = msg.displayName;
        accountHandle_ = msg.hasAccount ? msg.accountHandle : std::string();
        firstClaim_ = msg.firstClaim;
        keyFingerprint_ = msg.keyFingerprint;
        state_ = State::Ready;
        message_.clear();
        pending_ = Pending::None;
        keepAliveAtMs_ = nowMs;
        event_ = Event::LoggedIn;
        // Every Auth after the first that reached the server is answered with
        // a refusal that is on its way. A lost Auth leaves one fewer, so the
        // allowance also runs out on the clock rather than lasting for ever.
        staleAuthFails_ = authSends_ > 1 ? authSends_ - 1 : 0;
        staleAuthUntilMs_ = nowMs + kRequestTimeoutMs;
        return true;

    case ServerKind::AuthFail:
        // **The answer to a copy of an Auth that already worked.** Logged in,
        // not waiting on an Auth, and inside the allowance AuthOk worked out.
        // Believed, it would take down a login that has just succeeded -- which
        // a 3DS on slow wireless, or a server slow to write its database, hits
        // whenever the first answer takes longer than `kRetryMs`.
        if (state_ == State::Ready && pending_ != Pending::Auth && staleAuthFails_ > 0
            && i32(staleAuthUntilMs_ - nowMs) > 0) {
            --staleAuthFails_;
            return true;
        }
        // **The rest of the expired token's refusals.** A console back from the
        // keyboard has usually sent two things on the old token -- the keep-alive
        // the resume sends and the join it was typing -- and the server refuses
        // each. The first started this re-login; the others arrive while it is
        // still waiting on its Hello. The server answers in order, so all of them
        // are in before the Challenge is, and nothing a Hello can earn is a
        // refusal for an identity that logged in a minute ago.
        if (state_ == State::Connecting && relogged_ && pending_ == Pending::Hello) {
            return true;
        }
        // **A refusal of a handshake that was retransmitted is started over,
        // once.** A repeated Hello replaced the challenge the Auth was signed
        // over, or the AuthOk was lost and the repeated Auth found the
        // challenge already spent. Either way a fresh Hello settles it; a real
        // refusal comes back the same the second time and is believed then.
        if (state_ == State::Connecting && !handshakeRestarted_
            && (helloSends_ > 1 || authSends_ > 1)) {
            handshakeRestarted_ = true;
            helloSends_ = 0;
            authSends_ = 0;
            startRequest(Pending::Hello, nowMs);
            return true;
        }
        // **Logged in and refused: the server has forgotten this console.** It
        // drops a login it has not heard from for a minute, and a console hears
        // nothing and says nothing while the keyboard applet or HOME has the
        // loop stopped -- so a player who takes their time typing a join code
        // comes back to "unknown or expired session". That is not a reason to
        // give up: this logs in again from Hello and sends whatever was being
        // asked for once it has. The identity and its key are unchanged, so the
        // server lets it straight back in. Once per `kReloginGuardMs`: a second
        // refusal that soon is believed rather than looped on.
        if (state_ == State::Ready
            && (!relogged_ || i32(nowMs - reloggedAtMs_) >= i32(kReloginGuardMs))) {
            relogged_ = true;
            reloggedAtMs_ = nowMs;
            const Pending resume = pending_ != Pending::None ? pending_ : queued_;
            state_ = State::Connecting;
            queued_ = resume;
            helloSends_ = 0;
            authSends_ = 0;
            handshakeRestarted_ = false;
            staleAuthFails_ = 0;
            startRequest(Pending::Hello, nowMs);
            return true;
        }
        // **Otherwise this is the one refusal that is not a transient.**
        // "Already bound to a different key" means somebody else holds this
        // identity, and trying again will say the same thing until an operator
        // intervenes.
        fail(msg.reason);
        return true;

    case ServerKind::KeepaliveAck:
        reflexive_ = msg.reflexive;
        return true;

    case ServerKind::LinkCode:
        if (pending_ != Pending::LinkCode) {
            return true;
        }
        pending_ = Pending::None;
        linkCode_ = msg.code;
        linkCodeSeconds_ = msg.expiresInS;
        event_ = Event::LinkCodeIssued;
        return true;

    case ServerKind::Unlinked:
        if (pending_ != Pending::Unlink) {
            return true;
        }
        pending_ = Pending::None;
        accountHandle_.clear();
        displayName_ = msg.displayName;
        linkCode_.clear();
        linkCodeSeconds_ = 0;
        event_ = Event::Unlinked;
        return true;

    case ServerKind::SessionOpened:
        if (pending_ != Pending::Host) {
            return true;
        }
        pending_ = Pending::None;
        sessionId_ = msg.sessionId;
        joinCode_ = msg.joinCode;
        event_ = Event::SessionOpened;
        return true;

    case ServerKind::SessionList:
        if (pending_ != Pending::List) {
            return true;
        }
        pending_ = Pending::None;
        sessions_ = msg.sessions;
        event_ = Event::SessionsListed;
        return true;

    case ServerKind::PunchNow:
        // **Unsolicited on the host's side**, which is the whole shape of this:
        // a guest's JoinRequest produces a PunchNow at both ends at the same
        // moment, and the host never asked for one.
        if (pending_ == Pending::Join) {
            pending_ = Pending::None;
        }
        introduction_ = msg;
        sessionId_ = msg.sessionId;
        event_ = Event::PunchNow;
        return true;

    case ServerKind::RelayAllocated:
        introduction_ = msg;
        event_ = Event::RelayAllocated;
        return true;

    case ServerKind::Error:
        // An errand was refused. The login stands: this is "no such session" or
        // "this console is not linked", not a reason to log in again.
        pending_ = Pending::None;
        queued_ = Pending::None;
        message_ = msg.reason;
        event_ = Event::Refused;
        return true;
    }
    return false;
}

void Client::requestLinkCode()
{
    linkCode_.clear();
    linkCodeSeconds_ = 0;
    queue(Pending::LinkCode);
}

void Client::unlink()
{
    queue(Pending::Unlink);
}

void Client::hostSession(const std::string& worldName, u8 maxPlayers, bool locked)
{
    hostWorldName_ = worldName;
    hostMaxPlayers_ = maxPlayers;
    hostLocked_ = locked;
    sessionId_ = 0;
    joinCode_.clear();
    queue(Pending::Host);
}

void Client::closeSession()
{
    if (state_ != State::Ready || sessionId_ == 0) {
        return;
    }
    sessionId_ = 0;
    joinCode_.clear();
    queue(Pending::Close);
}

void Client::listSessions()
{
    queue(Pending::List);
}

void Client::joinByCode(const std::string& code)
{
    joinByCode_ = true;
    joinCodeWanted_ = code;
    joinIdWanted_ = 0;
    queue(Pending::Join);
}

void Client::joinById(u64 sessionId)
{
    joinByCode_ = false;
    joinCodeWanted_.clear();
    joinIdWanted_ = sessionId;
    queue(Pending::Join);
}

void Client::reportPunch(u64 sessionId, bool connected)
{
    if (state_ != State::Ready) {
        return;
    }
    // Sent once and not waited on: the server's answer to it is either a
    // RelayAllocated or nothing at all, and neither is an acknowledgement this
    // end can hang a retry off.
    ClientMsg msg;
    msg.kind = ClientKind::PunchResult;
    std::memcpy(msg.token, token_, kTokenSize);
    msg.sessionId = sessionId;
    msg.connected = connected;
    transmit(msg);
}

}  // namespace mc::net::ac
