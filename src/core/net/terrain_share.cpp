#include "core/net/terrain_share.hpp"

#include "core/net/wire.hpp"
#include "core/util/compress.hpp"
#include "version_config.hpp"

#include <cstring>

namespace mc::net::link {

namespace {

constexpr u8 kOptSnow = 0x01;
constexpr u8 kOptFixOre = 0x02;
constexpr u8 kOptFixBedrock = 0x04;

// x, z, index, count.
constexpr usize kPartHeader = 10;
constexpr usize kPartPayload = kMaxMessage - kPartHeader;

}  // namespace

u8 packOptions(const mcver::WorldGenOptions& options)
{
    return u8((options.snowCovered ? kOptSnow : 0) | (options.fixOreVeinBounds ? kOptFixOre : 0)
              | (options.fixBedrockHole ? kOptFixBedrock : 0));
}

mcver::WorldGenOptions unpackOptions(u8 flags)
{
    mcver::WorldGenOptions options;
    options.snowCovered = (flags & kOptSnow) != 0;
    options.fixOreVeinBounds = (flags & kOptFixOre) != 0;
    options.fixBedrockHole = (flags & kOptFixBedrock) != 0;
    return options;
}

const char* generatorVersion()
{
    // The version's id, which is what selects the worldgen slot. Two builds of
    // the same version with a changed generator would both say this and are
    // exactly what the file note says this does not catch.
    return mcver::kId;
}

void encodeTerrainRequest(i32 x, i32 z, std::vector<u8>* out)
{
    ByteWriter writer(out);
    writer.putI32(x);
    writer.putI32(z);
}

bool decodeTerrainRequest(const u8* body, usize size, i32* x, i32* z)
{
    ByteReader reader(body, size);
    return reader.getI32(x) && reader.getI32(z);
}

// ---- the host's side -------------------------------------------------------

TerrainPool::TerrainPool(int capacity)
{
    slots_.resize(usize(capacity < 1 ? 1 : capacity));
    incoming_.reserve(kTerrainBytes / 4);
}

TerrainPool::~TerrainPool() = default;

TerrainPool::Slot* TerrainPool::find(i32 x, i32 z)
{
    for (Slot& slot : slots_) {
        if ((slot.asked || slot.ready) && slot.x == x && slot.z == z) {
            return &slot;
        }
    }
    return nullptr;
}

bool TerrainPool::want(i32 x, i32 z)
{
    std::lock_guard<std::mutex> guard(lock_);
    if (find(x, z) != nullptr) {
        return true;  // already asked for, or already here
    }
    for (Slot& slot : slots_) {
        if (slot.asked || slot.ready) {
            continue;
        }
        slot.x = x;
        slot.z = z;
        slot.asked = true;
        slot.sent = false;
        slot.ready = false;
        return true;
    }
    return false;
}

bool TerrainPool::nextRequest(i32* x, i32* z)
{
    std::lock_guard<std::mutex> guard(lock_);
    for (Slot& slot : slots_) {
        if (slot.asked && !slot.sent && !slot.ready) {
            slot.sent = true;
            *x = slot.x;
            *z = slot.z;
            return true;
        }
    }
    return false;
}

void TerrainPool::abandon()
{
    std::lock_guard<std::mutex> guard(lock_);
    for (Slot& slot : slots_) {
        if (!slot.ready) {
            slot.asked = false;
            slot.sent = false;
        }
    }
    incoming_.clear();
    incomingCount_ = 0;
    incomingNext_ = 0;
}

bool TerrainPool::onPart(const u8* body, usize size)
{
    if (size < kPartHeader) {
        return false;
    }
    ByteReader reader(body, size);
    i32 x = 0;
    i32 z = 0;
    u8 index = 0;
    u8 count = 0;
    if (!reader.getI32(&x) || !reader.getI32(&z) || !reader.getU8(&index)
        || !reader.getU8(&count)) {
        return false;
    }
    if (count == 0 || int(count) > kMaxTerrainParts || index >= count) {
        return false;
    }

    std::lock_guard<std::mutex> guard(lock_);

    if (index == 0) {
        incoming_.clear();
        incomingX_ = x;
        incomingZ_ = z;
        incomingNext_ = 0;
        incomingCount_ = int(count);
    } else if (incomingCount_ == 0 || x != incomingX_ || z != incomingZ_
               || int(index) != incomingNext_) {
        // A part with no beginning, which is what a guest that was dropped
        // mid-column leaves behind. Drop the column rather than the link.
        incomingCount_ = 0;
        return false;
    }
    incomingNext_ = int(index) + 1;
    incoming_.insert(incoming_.end(), body + kPartHeader, body + size);
    if (incomingNext_ < incomingCount_) {
        return false;
    }

    // The last part: inflate it into whichever slot asked for it.
    incomingCount_ = 0;
    Slot* slot = find(x, z);
    if (slot == nullptr || slot->ready) {
        // **The generator got there first.** Nothing is wrong: the column was
        // made locally while this one was in the air, and the answer is simply
        // thrown away.
        ++late_;
        return false;
    }
    slot->blocks.clear();
    if (!zip::decompress(ConstByteSpan{incoming_.data(), incoming_.size()}, slot->blocks,
                         zip::Wrapper::Zlib, kTerrainBytes)
        || slot->blocks.size() != kTerrainBytes) {
        slot->asked = false;
        slot->sent = false;
        return false;
    }
    slot->ready = true;
    slot->asked = false;
    return true;
}

bool TerrainPool::supply(void* context, i32 chunkX, i32 chunkZ, u8* blocks)
{
    TerrainPool* self = static_cast<TerrainPool*>(context);
    std::lock_guard<std::mutex> guard(self->lock_);
    for (Slot& slot : self->slots_) {
        if (!slot.ready || slot.x != chunkX || slot.z != chunkZ) {
            continue;
        }
        std::memcpy(blocks, slot.blocks.data(), kTerrainBytes);
        slot.ready = false;
        slot.asked = false;
        slot.sent = false;
        ++self->used_;
        return true;
    }
    return false;
}

int TerrainPool::waiting() const
{
    std::lock_guard<std::mutex> guard(lock_);
    int count = 0;
    for (const Slot& slot : slots_) {
        if (slot.asked || slot.ready) {
            ++count;
        }
    }
    return count;
}

// ---- the guest's side ------------------------------------------------------

TerrainResponder::TerrainResponder()
{
    blocks_.resize(kTerrainBytes);
    packed_.reserve(kTerrainBytes / 4);
}

TerrainResponder::~TerrainResponder() = default;

void TerrainResponder::open(const GeneratorId& id)
{
    id_ = id;
    provider_ = std::make_unique<mcver::WorldGen>(id.seed, unpackOptions(id.options));
    queue_.clear();
}

bool TerrainResponder::onRequest(const u8* body, usize size)
{
    if (provider_ == nullptr) {
        return false;
    }
    i32 x = 0;
    i32 z = 0;
    if (!decodeTerrainRequest(body, size, &x, &z)) {
        return false;
    }
    // A short queue on purpose: a request this console cannot get to quickly
    // is one the host will have made itself by the time it arrives.
    constexpr usize kMaxQueued = 16;
    for (const Ask& ask : queue_) {
        if (ask.x == x && ask.z == z) {
            return true;
        }
    }
    if (queue_.size() >= kMaxQueued) {
        return false;
    }
    queue_.push_back(Ask{x, z});
    return true;
}

bool TerrainResponder::generateOne(std::vector<std::vector<u8>>* parts)
{
    parts->clear();
    if (provider_ == nullptr || queue_.empty()) {
        return false;
    }
    const Ask ask = queue_.front();
    queue_.erase(queue_.begin());

    provider_->generateColumn(ask.x, ask.z, blocks_.data());

    packed_.clear();
    if (!zip::compress(ConstByteSpan{blocks_.data(), blocks_.size()}, packed_,
                       zip::Wrapper::Zlib)) {
        return false;
    }

    const usize total = packed_.size();
    usize count = (total + kPartPayload - 1) / kPartPayload;
    if (count == 0) {
        count = 1;
    }
    if (count > usize(kMaxTerrainParts)) {
        return false;  // a column this incompressible cannot happen; refuse rather than lie
    }

    for (usize i = 0; i < count; ++i) {
        const usize offset = i * kPartPayload;
        const usize length = total - offset < kPartPayload ? total - offset : kPartPayload;
        std::vector<u8> body;
        body.reserve(kPartHeader + length);
        ByteWriter writer(&body);
        writer.putI32(ask.x);
        writer.putI32(ask.z);
        writer.putU8(u8(i));
        writer.putU8(u8(count));
        writer.putBytes(packed_.data() + offset, length);
        parts->push_back(std::move(body));
    }
    ++answered_;
    return true;
}

}  // namespace mc::net::link
