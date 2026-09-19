#include "core/net/link.hpp"

#include "core/net/wire.hpp"

#include <cstring>

namespace mc::net::link {

namespace {

constexpr u8 kFlagReliable = 0x01;
constexpr u8 kFlagHasAck = 0x02;

// Sequence numbers wrap at 65536, so "newer" is the sign of the difference
// rather than the order of the numbers. Everything in this file that compares
// two sequences goes through these.
bool seqAfter(u16 a, u16 b)
{
    return i16(u16(a - b)) > 0;
}

u16 seqDistance(u16 a, u16 b)
{
    return u16(a - b);
}

void putU16(u8* out, u16 value)
{
    out[0] = u8(value >> 8);
    out[1] = u8(value);
}

void putU32(u8* out, u32 value)
{
    out[0] = u8(value >> 24);
    out[1] = u8(value >> 16);
    out[2] = u8(value >> 8);
    out[3] = u8(value);
}

u16 readU16(const u8* in)
{
    return u16(u16(in[0]) << 8 | in[1]);
}

u32 readU32(const u8* in)
{
    return u32(in[0]) << 24 | u32(in[1]) << 16 | u32(in[2]) << 8 | in[3];
}

void appendMessage(std::vector<u8>* out, Msg kind, const u8* body, usize size)
{
    out->push_back(u8(kind));
    out->push_back(u8(size >> 8));
    out->push_back(u8(size));
    if (size != 0) {
        out->insert(out->end(), body, body + size);
    }
}

}  // namespace

const char* nameOf(Msg kind)
{
    switch (kind) {
    case Msg::Hello:      return "Hello";
    case Msg::Welcome:    return "Welcome";
    case Msg::Rules:      return "Rules";
    case Msg::Reject:     return "Reject";
    case Msg::Chat:       return "Chat";
    case Msg::Join:       return "Join";
    case Msg::Leave:      return "Leave";
    case Msg::Pose:       return "Pose";
    case Msg::Bye:            return "Bye";
    case Msg::TerrainRequest: return "TerrainRequest";
    case Msg::TerrainPart:    return "TerrainPart";
    case Msg::Away:       return "Away";
    case Msg::GamePacket: return "GamePacket";
    case Msg::WorldOffer:  return "WorldOffer";
    case Msg::WorldAnswer: return "WorldAnswer";
    case Msg::WorldFile:   return "WorldFile";
    case Msg::WorldData:   return "WorldData";
    case Msg::WorldResult: return "WorldResult";
    }
    return "?";
}

Peer::Peer()
{
    // Both rings are taken once, here, and never resized: this runs beside a
    // game loop and the one thing a link must not do is allocate while the
    // world is streaming.
    window_.resize(usize(kWindow) * kMaxDatagram);
    holding_.resize(usize(kWindow) * kMaxPayload);
    outgoing_.resize(kMaxDatagram);
    pending_.reserve(kMaxPayload);
    unreliable_.reserve(kMaxPayload);
}

void Peer::reset(u32 nowMs)
{
    for (Slot& slot : slots_) {
        slot = Slot{};
    }
    for (int i = 0; i < kWindow; ++i) {
        held_[i] = false;
        holdingSize_[i] = 0;
    }
    pending_.clear();
    unreliable_.clear();
    nextSeq_ = 0;
    expected_ = 0;
    ackLatest_ = 0;
    ackBits_ = 0;
    sawAny_ = false;
    rttMs_ = kInitialRttMs;
    lastHeardMs_ = nowMs;
    lastSentMs_ = nowMs;
    awayUntilMs_ = nowMs;
    away_ = false;
    sent_ = 0;
    received_ = 0;
    retransmits_ = 0;
    dropped_ = 0;
}

u32 Peer::retryMs() const
{
    // Twice the round trip plus a tick of slack, which is the classic answer
    // and the right one here: it is long enough that an acknowledgement still
    // in the air is not mistaken for a loss, and short enough that a lost
    // frame is back on the wire inside a game tick or two.
    u32 retry = rttMs_ * 2 + 20;
    if (retry < kMinRetryMs) {
        retry = kMinRetryMs;
    }
    if (retry > kMaxRetryMs) {
        retry = kMaxRetryMs;
    }
    return retry;
}

bool Peer::timedOut(u32 nowMs) const
{
    if (u32(nowMs - lastHeardMs_) < kTimeoutMs) {
        return false;
    }
    // **A console that warned us is not gone yet.** See `Msg::Away`: the grace
    // is a stated interval rather than a switch, so one that never comes back
    // is still dropped, just later.
    return !away_ || i32(nowMs - awayUntilMs_) >= 0;
}

void Peer::grantAway(u32 nowMs, u32 ms)
{
    const u32 grace = ms > kMaxAwayMs ? kMaxAwayMs : ms;
    away_ = true;
    awayUntilMs_ = nowMs + grace;
}

void Peer::forgive(u32 gapMs)
{
    lastHeardMs_ += gapMs;
    lastSentMs_ += gapMs;
    awayUntilMs_ += gapMs;
    for (Slot& slot : slots_) {
        if (slot.inUse) {
            slot.sentMs += gapMs;
        }
    }
}

int Peer::inFlight() const
{
    int count = 0;
    for (const Slot& slot : slots_) {
        if (slot.inUse) {
            ++count;
        }
    }
    return count;
}

bool Peer::stalled() const
{
    return inFlight() >= kWindow;
}

bool Peer::queue(Msg kind, const u8* body, usize size, bool reliable)
{
    if (size > kMaxMessage) {
        return false;
    }
    const usize need = kMessageHeader + size;

    if (!reliable) {
        // An unreliable message that will not fit beside the ones already
        // waiting is dropped rather than made to wait, because waiting is the
        // one thing it must not do -- by the time there is room, a newer pose
        // exists and this one is worthless.
        if (unreliable_.size() + need > kMaxPayload) {
            ++dropped_;
            return true;
        }
        appendMessage(&unreliable_, kind, body, size);
        return true;
    }

    if (pending_.size() + need > kMaxPayload && !sealPending(lastSentMs_)) {
        return false;  // the window is full; the caller tries again
    }
    appendMessage(&pending_, kind, body, size);
    return true;
}

bool Peer::sealPending(u32 nowMs)
{
    if (pending_.empty()) {
        return true;
    }
    const int index = nextSeq_ % kWindow;
    if (slots_[index].inUse) {
        return false;  // the window has wrapped onto an unacknowledged slot
    }

    Slot& slot = slots_[index];
    slot.seq = nextSeq_++;
    slot.size = kHeaderSize + pending_.size();
    slot.sentMs = nowMs;
    slot.tries = 0;
    slot.inUse = true;

    u8* bytes = window_.data() + usize(index) * kMaxDatagram;
    writeHeader(bytes, true, slot.seq);
    std::memcpy(bytes + kHeaderSize, pending_.data(), pending_.size());
    pending_.clear();
    return true;
}

void Peer::writeHeader(u8* out, bool reliable, u16 seq) const
{
    out[0] = u8((reliable ? kFlagReliable : 0) | (sawAny_ ? kFlagHasAck : 0));
    putU16(out + 1, seq);
    putU16(out + 3, ackLatest_);
    putU32(out + 5, ackBits_);
}

bool Peer::nextDatagram(u32 nowMs, u8* out, usize capacity, usize* size)
{
    if (capacity < kHeaderSize) {
        return false;
    }

    // **Retransmission first, and the oldest first within that.** A peer that
    // is missing one datagram is stalled behind it: everything after it is
    // being held rather than delivered, so putting the gap back on the wire
    // ahead of new work is what shortens the stall.
    const u32 retry = retryMs();
    int oldest = -1;
    for (int i = 0; i < kWindow; ++i) {
        const Slot& slot = slots_[i];
        if (!slot.inUse || slot.tries == 0) {
            continue;
        }
        if (u32(nowMs - slot.sentMs) < retry) {
            continue;
        }
        if (oldest < 0 || seqAfter(slots_[oldest].seq, slot.seq)) {
            oldest = i;
        }
    }
    if (oldest >= 0) {
        Slot& slot = slots_[oldest];
        u8* bytes = window_.data() + usize(oldest) * kMaxDatagram;
        // The acknowledgements are this moment's, not the ones the datagram
        // carried the first time.
        writeHeader(bytes, true, slot.seq);
        if (slot.size > capacity) {
            return false;
        }
        std::memcpy(out, bytes, slot.size);
        *size = slot.size;
        slot.sentMs = nowMs;
        if (slot.tries < 255) {
            ++slot.tries;
        }
        ++retransmits_;
        ++sent_;
        lastSentMs_ = nowMs;
        return true;
    }

    // Then anything queued that has not gone out once, oldest first again, so
    // the peer is not made to reorder datagrams this end could have sent in
    // order for nothing.
    int fresh = -1;
    for (int i = 0; i < kWindow; ++i) {
        const Slot& slot = slots_[i];
        if (!slot.inUse || slot.tries != 0) {
            continue;
        }
        if (fresh < 0 || seqAfter(slots_[fresh].seq, slot.seq)) {
            fresh = i;
        }
    }
    if (fresh >= 0) {
        Slot& slot = slots_[fresh];
        u8* bytes = window_.data() + usize(fresh) * kMaxDatagram;
        writeHeader(bytes, true, slot.seq);
        if (slot.size > capacity) {
            return false;
        }
        std::memcpy(out, bytes, slot.size);
        *size = slot.size;
        slot.sentMs = nowMs;
        slot.tries = 1;
        ++sent_;
        lastSentMs_ = nowMs;
        return true;
    }

    if (!pending_.empty() && sealPending(nowMs)) {
        return nextDatagram(nowMs, out, capacity, size);
    }

    if (!unreliable_.empty()) {
        const usize total = kHeaderSize + unreliable_.size();
        if (total > capacity) {
            unreliable_.clear();
            return false;
        }
        writeHeader(out, false, 0);
        std::memcpy(out + kHeaderSize, unreliable_.data(), unreliable_.size());
        *size = total;
        unreliable_.clear();
        ++sent_;
        lastSentMs_ = nowMs;
        return true;
    }

    // Nothing to say, but silence is not free: an empty datagram is this end's
    // acknowledgements and its proof of life, and it is nine bytes.
    if (u32(nowMs - lastSentMs_) >= kKeepAliveMs) {
        writeHeader(out, false, 0);
        *size = kHeaderSize;
        ++sent_;
        lastSentMs_ = nowMs;
        return true;
    }
    return false;
}

bool Peer::flush(u32 nowMs, Datagrams& datagrams, u16 node)
{
    usize size = 0;
    int burst = 0;
    while (burst < kFlushBurst
           && nextDatagram(nowMs, outgoing_.data(), outgoing_.size(), &size)) {
        ++burst;
        if (!datagrams.send(node, outgoing_.data(), size)) {
            return false;
        }
    }
    return true;
}

void Peer::noteRtt(u32 sample)
{
    // The usual exponential average, at 1/8 -- slow enough that one late
    // acknowledgement does not move the retransmission timer much, quick
    // enough to follow a link that has genuinely got worse.
    rttMs_ = (rttMs_ * 7 + sample) / 8;
}

void Peer::ackDatagram(u16 ack, u32 ackBits, u32 nowMs)
{
    for (Slot& slot : slots_) {
        if (!slot.inUse) {
            continue;
        }
        bool acked = slot.seq == ack;
        if (!acked && seqAfter(ack, slot.seq)) {
            const u16 back = seqDistance(ack, slot.seq);
            acked = back <= 32 && (ackBits & (1u << (back - 1))) != 0;
        }
        if (!acked) {
            continue;
        }
        // **Only a datagram that went out once gives a round trip.** After a
        // retransmission there is no telling which copy is being answered, and
        // taking the sample anyway is what makes a lossy link's timer collapse.
        if (slot.tries == 1) {
            noteRtt(u32(nowMs - slot.sentMs));
        }
        slot.inUse = false;
    }
}

bool Peer::deliver(const u8* body, usize size, MessageSink sink, void* ctx)
{
    usize pos = 0;
    while (pos < size) {
        if (size - pos < kMessageHeader) {
            return false;
        }
        const Msg kind = Msg(body[pos]);
        const usize length = usize(readU16(body + pos + 1));
        pos += kMessageHeader;
        if (size - pos < length) {
            return false;
        }
        if (sink != nullptr) {
            sink(ctx, kind, body + pos, length);
        }
        pos += length;
    }
    return true;
}

bool Peer::receive(const u8* data, usize size, u32 nowMs, MessageSink sink, void* ctx)
{
    if (size < kHeaderSize || size > kMaxDatagram) {
        return false;
    }
    const u8 flags = data[0];
    const u16 seq = readU16(data + 1);
    const u16 ack = readU16(data + 3);
    const u32 ackBits = readU32(data + 5);
    const u8* body = data + kHeaderSize;
    const usize bodySize = size - kHeaderSize;

    lastHeardMs_ = nowMs;
    ++received_;

    if ((flags & kFlagHasAck) != 0) {
        ackDatagram(ack, ackBits, nowMs);
    }

    if ((flags & kFlagReliable) == 0) {
        return deliver(body, bodySize, sink, ctx);
    }

    // Acknowledge it whether or not it can be delivered yet: a duplicate that
    // is not acknowledged is a duplicate that arrives again.
    if (!sawAny_) {
        sawAny_ = true;
        ackLatest_ = seq;
        ackBits_ = 0;
    } else if (seqAfter(seq, ackLatest_)) {
        const u16 shift = seqDistance(seq, ackLatest_);
        ackBits_ = shift >= 32 ? 0u : ((ackBits_ << shift) | (1u << (shift - 1)));
        ackLatest_ = seq;
    } else {
        const u16 back = seqDistance(ackLatest_, seq);
        if (back >= 1 && back <= 32) {
            ackBits_ |= 1u << (back - 1);
        }
    }

    if (seq == expected_) {
        if (!deliver(body, bodySize, sink, ctx)) {
            return false;
        }
        ++expected_;
        // Whatever arrived early and has been waiting behind this one.
        for (;;) {
            const int index = expected_ % kWindow;
            if (!held_[index]) {
                break;
            }
            const u8* bytes = holding_.data() + usize(index) * kMaxPayload;
            const usize length = holdingSize_[index];
            held_[index] = false;
            holdingSize_[index] = 0;
            if (!deliver(bytes, length, sink, ctx)) {
                return false;
            }
            ++expected_;
        }
        return true;
    }

    if (!seqAfter(seq, expected_)) {
        return true;  // already delivered; the acknowledgement above is the answer
    }

    // Early. The sender's window is `kWindow` wide, so anything further ahead
    // than that is not a reordering -- it is a peer that has lost track of
    // where this end is, and the link is finished.
    if (seqDistance(seq, expected_) >= u16(kWindow)) {
        return false;
    }
    if (bodySize > kMaxPayload) {
        return false;
    }
    const int index = seq % kWindow;
    if (!held_[index]) {
        std::memcpy(holding_.data() + usize(index) * kMaxPayload, body, bodySize);
        holdingSize_[index] = bodySize;
        held_[index] = true;
    }
    return true;
}

// ---- message bodies --------------------------------------------------------

void encode(const Hello& value, std::vector<u8>* out)
{
    ByteWriter writer(out);
    writer.putI16(i16(value.protocol));
    writer.putString(value.name);
    writer.putString(value.generator);
}

void encode(const Welcome& value, std::vector<u8>* out)
{
    ByteWriter writer(out);
    writer.putI16(i16(value.protocol));
    writer.putU8(value.playerId);
    writer.putString(value.worldName);
    writer.putString(value.hostName);
    writer.putI64(value.world.seed);
    writer.putU8(value.world.options);
    writer.putString(value.world.version);
    writer.putU8(value.rules.gamemode);
    writer.putU8(value.rules.difficulty);
}

void encode(const WorldRules& value, std::vector<u8>* out)
{
    ByteWriter writer(out);
    writer.putU8(value.gamemode);
    writer.putU8(value.difficulty);
}

void encode(const GeneratorId& value, std::vector<u8>* out)
{
    ByteWriter writer(out);
    writer.putI64(value.seed);
    writer.putU8(value.options);
    writer.putString(value.version);
}

void encode(const Reject& value, std::vector<u8>* out)
{
    ByteWriter writer(out);
    writer.putString(value.reason);
}

void encode(const Pose& value, std::vector<u8>* out)
{
    ByteWriter writer(out);
    writer.putU8(value.playerId);
    writer.putI16(i16(value.tick));
    writer.putI32(value.x);
    writer.putI32(value.y);
    writer.putI32(value.z);
    writer.putU8(u8(value.yaw));
    writer.putU8(u8(value.pitch));
    writer.putU8(value.flags);
}

void encode(const Chat& value, std::vector<u8>* out)
{
    ByteWriter writer(out);
    writer.putU8(value.playerId);
    writer.putString(value.text);
}

void encode(const Player& value, std::vector<u8>* out)
{
    ByteWriter writer(out);
    writer.putU8(value.playerId);
    writer.putString(value.name);
}

namespace {

bool readString(ByteReader& reader, std::string* out)
{
    bool malformed = false;
    return reader.getString(out, &malformed) && !malformed;
}

}  // namespace

bool decode(const u8* body, usize size, Hello* out)
{
    ByteReader reader(body, size);
    i16 protocol = 0;
    std::string name;
    std::string generator;
    if (!reader.getI16(&protocol) || !readString(reader, &name)
        || !readString(reader, &generator)) {
        return false;
    }
    out->protocol = u16(protocol);
    out->name = std::move(name);
    out->generator = std::move(generator);
    return true;
}

bool decode(const u8* body, usize size, Welcome* out)
{
    ByteReader reader(body, size);
    i16 protocol = 0;
    u8 playerId = 0;
    std::string worldName;
    std::string hostName;
    i64 seed = 0;
    u8 options = 0;
    std::string version;
    u8 gamemode = 0;
    u8 difficulty = 0;
    if (!reader.getI16(&protocol) || !reader.getU8(&playerId)
        || !readString(reader, &worldName) || !readString(reader, &hostName)
        || !reader.getI64(&seed) || !reader.getU8(&options) || !readString(reader, &version)
        || !reader.getU8(&gamemode) || !reader.getU8(&difficulty)) {
        return false;
    }
    out->protocol = u16(protocol);
    out->playerId = playerId;
    out->worldName = std::move(worldName);
    out->hostName = std::move(hostName);
    out->world.seed = seed;
    out->world.options = options;
    out->world.version = std::move(version);
    out->rules.gamemode = gamemode;
    out->rules.difficulty = difficulty;
    return true;
}

bool decode(const u8* body, usize size, WorldRules* out)
{
    ByteReader reader(body, size);
    u8 gamemode = 0;
    u8 difficulty = 0;
    if (!reader.getU8(&gamemode) || !reader.getU8(&difficulty)) {
        return false;
    }
    out->gamemode = gamemode;
    out->difficulty = difficulty;
    return true;
}

bool decode(const u8* body, usize size, Reject* out)
{
    ByteReader reader(body, size);
    std::string reason;
    if (!readString(reader, &reason)) {
        return false;
    }
    out->reason = std::move(reason);
    return true;
}

bool decode(const u8* body, usize size, Pose* out)
{
    ByteReader reader(body, size);
    u8 playerId = 0;
    i16 tick = 0;
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    u8 yaw = 0;
    u8 pitch = 0;
    u8 flags = 0;
    if (!reader.getU8(&playerId) || !reader.getI16(&tick) || !reader.getI32(&x)
        || !reader.getI32(&y) || !reader.getI32(&z) || !reader.getU8(&yaw)
        || !reader.getU8(&pitch) || !reader.getU8(&flags)) {
        return false;
    }
    out->playerId = playerId;
    out->tick = u16(tick);
    out->x = x;
    out->y = y;
    out->z = z;
    out->yaw = i8(yaw);
    out->pitch = i8(pitch);
    out->flags = flags;
    return true;
}

bool decode(const u8* body, usize size, Chat* out)
{
    ByteReader reader(body, size);
    u8 playerId = 0;
    std::string text;
    if (!reader.getU8(&playerId) || !readString(reader, &text)) {
        return false;
    }
    out->playerId = playerId;
    out->text = std::move(text);
    return true;
}

bool decode(const u8* body, usize size, GeneratorId* out)
{
    ByteReader reader(body, size);
    i64 seed = 0;
    u8 options = 0;
    std::string version;
    if (!reader.getI64(&seed) || !reader.getU8(&options) || !readString(reader, &version)) {
        return false;
    }
    out->seed = seed;
    out->options = options;
    out->version = std::move(version);
    return true;
}

bool decode(const u8* body, usize size, Player* out)
{
    ByteReader reader(body, size);
    u8 playerId = 0;
    std::string name;
    if (!reader.getU8(&playerId) || !readString(reader, &name)) {
        return false;
    }
    out->playerId = playerId;
    out->name = std::move(name);
    return true;
}

}  // namespace mc::net::link
