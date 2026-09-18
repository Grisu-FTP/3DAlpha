#include "core/util/ed25519.hpp"

#include "core/util/sha512.hpp"

#include <cstring>

namespace mc::util {

namespace {

// ---------------------------------------------------------------------------
// The field, GF(2^255 - 19).
//
// Eight little-endian 32-bit limbs holding a fully reduced value in [0, p).
// The usual radix-2^51 form wants a 128-bit product and an ARM11 has none, so
// this is plain schoolbook: sixty-four 32x32->64 multiplies per field
// multiplication, then the 2^256 = 38 (mod p) fold. That is *fewer* multiplies
// than the classic radix-2^25.5 form and considerably easier to be sure of.
// ---------------------------------------------------------------------------

constexpr int kLimbs = 8;

struct Fe {
    u32 v[kLimbs];
};

constexpr Fe kZero = {{0, 0, 0, 0, 0, 0, 0, 0}};
constexpr Fe kOne = {{1, 0, 0, 0, 0, 0, 0, 0}};

// d = -121665/121666, and 2d, which is what the addition law actually wants.
constexpr Fe kD = {{0x135978a3, 0x75eb4dca, 0x4141d8ab, 0x00700a4d, 0x7779e898, 0x8cc74079,
                    0x2b6ffe73, 0x52036cee}};
constexpr Fe kD2 = {{0x26b2f159, 0xebd69b94, 0x8283b156, 0x00e0149a, 0xeef3d130, 0x198e80f2,
                     0x56dffce7, 0x2406d9dc}};

// The base point, the generator of the prime-order subgroup.
constexpr Fe kBx = {{0x8f25d51a, 0xc9562d60, 0x9525a7b2, 0x692cc760, 0xfdd6dc5c, 0xc0a4e231,
                     0xcd6e53fe, 0x216936d3}};
constexpr Fe kBy = {{0x66666658, 0x66666666, 0x66666666, 0x66666666, 0x66666666, 0x66666666,
                     0x66666666, 0x66666666}};

// p itself, for the conditional subtractions.
constexpr u32 kP[kLimbs] = {0xffffffed, 0xffffffff, 0xffffffff, 0xffffffff,
                            0xffffffff, 0xffffffff, 0xffffffff, 0x7fffffff};

// Subtracts p from `v` if it is there to subtract. Branch-free only by
// accident: see the header on why timing is not in the threat model.
void feWeakReduce(u32* v)
{
    u32 t[kLimbs];
    u64 borrow = 0;
    for (int i = 0; i < kLimbs; ++i) {
        const u64 diff = u64(v[i]) - u64(kP[i]) - borrow;
        t[i] = u32(diff);
        borrow = (diff >> 32) & 1;
    }
    if (borrow == 0) {
        std::memcpy(v, t, sizeof(t));
    }
}

void feAdd(Fe* r, const Fe& a, const Fe& b)
{
    u64 carry = 0;
    for (int i = 0; i < kLimbs; ++i) {
        const u64 sum = u64(a.v[i]) + u64(b.v[i]) + carry;
        r->v[i] = u32(sum);
        carry = sum >> 32;
    }
    // Both inputs are below p < 2^255, so the sum is below 2^256 and the carry
    // out is always zero; one subtraction is enough to land back under p.
    feWeakReduce(r->v);
}

void feSub(Fe* r, const Fe& a, const Fe& b)
{
    u64 borrow = 0;
    for (int i = 0; i < kLimbs; ++i) {
        const u64 diff = u64(a.v[i]) - u64(b.v[i]) - borrow;
        r->v[i] = u32(diff);
        borrow = (diff >> 32) & 1;
    }
    if (borrow != 0) {
        u64 carry = 0;
        for (int i = 0; i < kLimbs; ++i) {
            const u64 sum = u64(r->v[i]) + u64(kP[i]) + carry;
            r->v[i] = u32(sum);
            carry = sum >> 32;
        }
    }
}

void feMul(Fe* r, const Fe& a, const Fe& b)
{
    u64 t[2 * kLimbs] = {0};
    for (int i = 0; i < kLimbs; ++i) {
        u64 carry = 0;
        for (int j = 0; j < kLimbs; ++j) {
            const u64 acc = t[i + j] + u64(a.v[i]) * u64(b.v[j]) + carry;
            t[i + j] = acc & 0xffffffffULL;
            carry = acc >> 32;
        }
        t[i + kLimbs] += carry;
    }

    // 2^256 = 2 * 2^255 = 2 * (p + 19) = 38 (mod p), so the top half folds down
    // onto the bottom one multiplied by 38. The carry out of that stays under
    // 40, so a second fold is an addition of a three-digit number.
    u32 lo[kLimbs];
    u64 carry = 0;
    for (int i = 0; i < kLimbs; ++i) {
        const u64 acc = t[i] + 38ULL * t[i + kLimbs] + carry;
        lo[i] = u32(acc);
        carry = acc >> 32;
    }
    while (carry != 0) {
        u64 add = 38ULL * carry;
        for (int i = 0; i < kLimbs && add != 0; ++i) {
            const u64 acc = u64(lo[i]) + add;
            lo[i] = u32(acc);
            add = acc >> 32;
        }
        carry = add;
    }

    // Below 2^256 = 2p + 38, so at most two subtractions remain.
    feWeakReduce(lo);
    feWeakReduce(lo);
    std::memcpy(r->v, lo, sizeof(lo));
}

void feSquare(Fe* r, const Fe& a)
{
    feMul(r, a, a);
}

// a^(p-2), which is 1/a for everything but zero. Plain square-and-multiply over
// the exponent's bits: about five hundred field multiplications, once per
// signature, on a console that is not doing anything else at the time.
void feInvert(Fe* r, const Fe& a)
{
    // p - 2 = 2^255 - 21, as little-endian bytes. Its lowest byte is 0xed - 2,
    // and no other byte changes, so there is no borrow to carry.
    u8 exponent[32];
    for (int i = 0; i < kLimbs; ++i) {
        for (int b = 0; b < 4; ++b) {
            exponent[i * 4 + b] = u8(kP[i] >> (8 * b));
        }
    }
    exponent[0] = u8(exponent[0] - 2);

    Fe result = kOne;
    for (int bit = 254; bit >= 0; --bit) {
        feSquare(&result, result);
        if (((exponent[bit >> 3] >> (bit & 7)) & 1) != 0) {
            feMul(&result, result, a);
        }
    }
    *r = result;
}

void feFromBytes(Fe* r, const u8 in[32])
{
    for (int i = 0; i < kLimbs; ++i) {
        r->v[i] = u32(in[i * 4]) | (u32(in[i * 4 + 1]) << 8) | (u32(in[i * 4 + 2]) << 16)
                  | (u32(in[i * 4 + 3]) << 24);
    }
}

void feToBytes(u8 out[32], const Fe& a)
{
    for (int i = 0; i < kLimbs; ++i) {
        out[i * 4] = u8(a.v[i]);
        out[i * 4 + 1] = u8(a.v[i] >> 8);
        out[i * 4 + 2] = u8(a.v[i] >> 16);
        out[i * 4 + 3] = u8(a.v[i] >> 24);
    }
}

// ---------------------------------------------------------------------------
// The curve, in extended coordinates: x = X/Z, y = Y/Z, and T with XY = ZT.
//
// One addition law, used for doubling as well. It is complete for a = -1 with
// a non-square d, so there is no case where it silently returns the wrong
// answer -- which is worth more here than the handful of multiplications a
// dedicated doubling would save.
// ---------------------------------------------------------------------------

struct Point {
    Fe x;
    Fe y;
    Fe z;
    Fe t;
};

const Point kIdentity = {kZero, kOne, kOne, kZero};

void pointAdd(Point* r, const Point& p1, const Point& p2)
{
    Fe a;
    Fe b;
    Fe c;
    Fe d;
    Fe e;
    Fe f;
    Fe g;
    Fe h;
    Fe tmp1;
    Fe tmp2;

    feSub(&tmp1, p1.y, p1.x);
    feSub(&tmp2, p2.y, p2.x);
    feMul(&a, tmp1, tmp2);

    feAdd(&tmp1, p1.y, p1.x);
    feAdd(&tmp2, p2.y, p2.x);
    feMul(&b, tmp1, tmp2);

    feMul(&tmp1, p1.t, p2.t);
    feMul(&c, tmp1, kD2);

    feMul(&tmp1, p1.z, p2.z);
    feAdd(&d, tmp1, tmp1);

    feSub(&e, b, a);
    feSub(&f, d, c);
    feAdd(&g, d, c);
    feAdd(&h, b, a);

    feMul(&r->x, e, f);
    feMul(&r->y, g, h);
    feMul(&r->t, e, h);
    feMul(&r->z, f, g);
}

// `scalar` is 32 bytes, little-endian, and its top bit is never set for any
// scalar this file produces.
void pointScalarMul(Point* r, const Point& base, const u8 scalar[32])
{
    Point result = kIdentity;
    for (int bit = 255; bit >= 0; --bit) {
        pointAdd(&result, result, result);
        if (((scalar[bit >> 3] >> (bit & 7)) & 1) != 0) {
            pointAdd(&result, result, base);
        }
    }
    *r = result;
}

// RFC 8032 section 5.1.2: y in the low 255 bits, the low bit of x on top.
void pointEncode(u8 out[32], const Point& p)
{
    Fe inverse;
    Fe x;
    Fe y;
    feInvert(&inverse, p.z);
    feMul(&x, p.x, inverse);
    feMul(&y, p.y, inverse);

    feToBytes(out, y);
    u8 xBytes[32];
    feToBytes(xBytes, x);
    out[31] = u8(out[31] | ((xBytes[0] & 1) << 7));
}

Point basePoint()
{
    Point b;
    b.x = kBx;
    b.y = kBy;
    b.z = kOne;
    feMul(&b.t, kBx, kBy);
    return b;
}

// ---------------------------------------------------------------------------
// Scalars, modulo the group order L.
//
// Reduction is a bitwise double-and-add rather than the unrolled 23-limb
// routine the reference implementation uses. It is five hundred iterations of
// a shift and a conditional subtract -- microseconds, once per signature -- and
// it is short enough to read and be sure of, which the unrolled form is not.
// ---------------------------------------------------------------------------

constexpr u8 kL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
                       0xa2, 0xde, 0xf9, 0xde, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};

// True when the 32-byte little-endian `a` is at least L.
bool atLeastOrder(const u8 a[32])
{
    for (int i = 31; i >= 0; --i) {
        if (a[i] != kL[i]) {
            return a[i] > kL[i];
        }
    }
    return true;
}

void subtractOrder(u8 a[32])
{
    int borrow = 0;
    for (int i = 0; i < 32; ++i) {
        const int diff = int(a[i]) - int(kL[i]) - borrow;
        a[i] = u8(diff);
        borrow = diff < 0 ? 1 : 0;
    }
}

// r = (2r + bit) mod L. r stays below L, so 2r + 1 is below 2L and one
// subtraction is always enough.
void scDoubleAdd(u8 r[32], int bit)
{
    int carry = bit;
    for (int i = 0; i < 32; ++i) {
        const int value = (int(r[i]) << 1) | carry;
        r[i] = u8(value);
        carry = value >> 8;
    }
    if (carry != 0 || atLeastOrder(r)) {
        subtractOrder(r);
    }
}

// Any little-endian number, however long, reduced modulo L.
void scReduce(u8 out[32], const u8* in, usize size)
{
    std::memset(out, 0, 32);
    for (usize i = size; i > 0; --i) {
        const u8 byte = in[i - 1];
        for (int bit = 7; bit >= 0; --bit) {
            scDoubleAdd(out, (byte >> bit) & 1);
        }
    }
}

// s = (a * b + c) mod L, the last line of a signature.
void scMulAdd(u8 out[32], const u8 a[32], const u8 b[32], const u8 c[32])
{
    // The product first, as 64 bytes, then reduced. Schoolbook on bytes: 1,024
    // 8x8 multiplies, which is nothing next to the scalar multiplication above.
    u32 wide[64] = {0};
    for (int i = 0; i < 32; ++i) {
        u32 carry = 0;
        for (int j = 0; j < 32; ++j) {
            const u32 acc = wide[i + j] + u32(a[i]) * u32(b[j]) + carry;
            wide[i + j] = acc & 0xff;
            carry = acc >> 8;
        }
        int k = i + 32;
        while (carry != 0) {
            const u32 acc = wide[k] + carry;
            wide[k] = acc & 0xff;
            carry = acc >> 8;
            ++k;
        }
    }

    u8 product[64];
    for (int i = 0; i < 64; ++i) {
        product[i] = u8(wide[i]);
    }

    u8 reduced[32];
    scReduce(reduced, product, 64);

    u8 addend[32];
    scReduce(addend, c, 32);

    int carry = 0;
    for (int i = 0; i < 32; ++i) {
        const int sum = int(reduced[i]) + int(addend[i]) + carry;
        out[i] = u8(sum);
        carry = sum >> 8;
    }
    if (carry != 0 || atLeastOrder(out)) {
        subtractOrder(out);
    }
}

// RFC 8032 section 5.1.5: the low half of the seed's hash, with the bits that
// force a multiple of the cofactor and a fixed high bit.
void clampScalar(u8 a[32])
{
    a[0] = u8(a[0] & 248);
    a[31] = u8(a[31] & 127);
    a[31] = u8(a[31] | 64);
}

}  // namespace

void ed25519PublicKey(const u8 seed[kEd25519SeedSize], u8 publicKey[kEd25519PublicKeySize])
{
    u8 hash[kSha512DigestSize];
    sha512(seed, kEd25519SeedSize, hash);
    clampScalar(hash);

    Point a;
    pointScalarMul(&a, basePoint(), hash);
    pointEncode(publicKey, a);
}

void ed25519Sign(const u8 seed[kEd25519SeedSize], const u8 publicKey[kEd25519PublicKeySize],
                 const u8* message, usize size, u8 signature[kEd25519SignatureSize])
{
    u8 hash[kSha512DigestSize];
    sha512(seed, kEd25519SeedSize, hash);
    clampScalar(hash);

    // r = H(prefix || message) mod L, where the prefix is the hash's upper
    // half. Deterministic: the same message signed twice gives the same
    // signature, which is the specification's own answer to a console with no
    // trustworthy entropy at signing time.
    u8 rHash[kSha512DigestSize];
    {
        Sha512 sha;
        sha.update(hash + 32, 32);
        sha.update(message, size);
        sha.finish(rHash);
    }
    u8 r[32];
    scReduce(r, rHash, kSha512DigestSize);

    Point rPoint;
    pointScalarMul(&rPoint, basePoint(), r);
    pointEncode(signature, rPoint);

    // k = H(R || A || message) mod L, and S = r + k*a.
    u8 kHash[kSha512DigestSize];
    {
        Sha512 sha;
        sha.update(signature, 32);
        sha.update(publicKey, kEd25519PublicKeySize);
        sha.update(message, size);
        sha.finish(kHash);
    }
    u8 k[32];
    scReduce(k, kHash, kSha512DigestSize);

    scMulAdd(signature + 32, k, hash, r);
}

}  // namespace mc::util
