#pragma once

// **A DNS resolver of our own**, because the console's is not reliably one.
//
// `TcpSocket::connect` used to resolve a name with `gethostbyname` alone --
// libctru's, which is one IPC command to SOC's `GetHostByName` -- on the
// argument that it is "what has always worked there". It is not: the first
// hardware report of multiplayer past the loopback was *DNS does not work*,
// with a numeric address connecting fine and a name failing every time. SOC's
// resolver is a service call into a stack that is only ever exercised by
// Nintendo's own titles, and there is nothing underneath it a homebrew process
// can fix.
//
// What the console *does* have, and hands out freely, is **the list of name
// servers DHCP gave it** -- which on a home network is the router. That is the
// answer to "can it not just use the 3DS's or the router's DNS": it can, and
// this is the query that does it. Two hundred lines of UDP and a wire format
// frozen since 1987 beat a service call that cannot be debugged.
//
// **The order `connect` tries is four deep**, cheapest and most certain first:
//
//   1. a numeric address, which needs no resolver at all;
//   2. `getaddrinfo`, SOC's other resolver and the one most 3DS homebrew uses;
//   3. `gethostbyname`, SOC's first;
//   4. this -- an A query to each of the console's own name servers in turn.
//
// Anything that answers wins. The host build never reaches step 4 because its
// libc resolves at step 2, which is also why the wire code below is tested
// against canned bytes rather than against a live server.
//
// **Only A records and only IPv4.** `TcpSocket` connects over `sockaddr_in`
// and a1.1.2 servers are IPv4; a AAAA query would produce an address nothing
// here can use. A CNAME chain is followed by reading whatever A record the
// server put in the same answer, which is what every resolver on a home
// network sends unasked.

#include "core/util/types.hpp"

#include <string>
#include <string_view>

namespace mc::net {

// Port 53, and the cap on how many servers are worth asking. Four is what a
// DHCP lease offers at the very most; two is the usual.
inline constexpr u16 kDnsPort = 53;
inline constexpr int kMaxNameServers = 4;

// **Where the console's name servers come from is the platform's business.**
// Core has no idea what a `SOCU_GetNetworkOpt` is, so the 3DS layer installs a
// source and everything else sees a list of IPv4 addresses in host byte order.
// A build that installs none resolves through steps 1-3 above and no further,
// which is what the host does.
//
// `out` takes up to `max` addresses; the return is how many were written.
using NameServerSource = int (*)(void* ctx, u32* out, int max);

void setNameServerSource(NameServerSource source, void* ctx);

// The installed source's answer, or none. Never fails: no source and no
// servers are the same thing to the caller.
int nameServers(u32* out, int max);

// **One A query, built.** `out` takes the packet and `*size` its length;
// false when the name will not fit the format -- a label over 63 bytes, a name
// over 255, or an empty one. `id` is the query id the answer must carry back.
//
// Exposed rather than hidden inside the send so that it can be tested without
// a socket, which is the only way to test it at all on a host whose own
// resolver answers first.
inline constexpr usize kMaxDnsMessage = 512;
bool buildDnsQuery(std::string_view host, u16 id, u8* out, usize* size);

// **The first A record in an answer**, as a host-order IPv4 address. False for
// a malformed message, one whose id does not match, one that carries an error
// code, or one with no A record in it at all.
//
// It walks the answer section by name-skipping rather than by parsing names,
// because the only thing wanted out of it is four bytes of RDATA: a compressed
// pointer, a chain of CNAMEs and an out-of-order section all come out the same
// way. **Every offset is bounds-checked against the message**, because this
// parses a packet from a machine on the network that nothing here has any
// reason to trust.
bool parseDnsAnswer(const u8* message, usize size, u16 id, u32* address);

// Asks each of the console's name servers for `host`, in order, giving up on
// each after `timeoutMs`. True with `*address` in host byte order.
//
// Opens and closes one UDP socket per attempt. It runs on the session thread,
// once, before a connection -- there is nothing to keep around.
bool resolveViaDns(std::string_view host, int timeoutMs, u32* address);

}  // namespace mc::net
