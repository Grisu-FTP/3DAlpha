#pragma once

// A world shared over the internet, from the console's side: the worker that
// runs a core/net/world_share.hpp job, and the little the menu needs to watch
// it.
//
// **The job runs on a worker, all of it.** Walking the world, deflating it,
// inflating the other end and writing it to the card are all things CONTRIBUTING
// keeps off the frame, and the socket blocks besides. The worker is the `Net`
// role -- core 0 under the main thread, where the multiplayer session's already
// is (see `workerSpawn` in main.cpp) -- so the menu keeps drawing while it
// works, and the frame reads an atomic to know how far it has got. Nothing on
// the main thread ever waits for it except the destructor, which is bounded by
// the connect timeout.
//
// **The share lives as long as the login.** AlphaComputer deletes a shared
// world when its owner stops sending keep-alives, so the menu goes on pumping
// `Online` for as long as the code is on screen, and a login that was replaced
// -- the client logs in again on its own after a long suspension -- is a share
// that has gone. `loginChanged` is how the screen notices.

#include "core/io/file_system.hpp"
#include "core/net/tcp_socket.hpp"
#include "core/net/world_share.hpp"
#include "core/util/types.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace mc::ctr {

class Online;

class OnlineShare {
public:
    explicit OnlineShare(io::FileSystem& fs);
    ~OnlineShare();

    OnlineShare(const OnlineShare&) = delete;
    OnlineShare& operator=(const OnlineShare&) = delete;

    // What this one is for, decided before the login exists. Neither touches
    // the card or the network; `start` does, on the worker.
    void exportWorld(const std::string& worldDir, const std::string& worldName);
    void importWorld(const std::string& savesDir, const std::string& targetName,
                     const std::string& code);

    bool exporting() const { return exporting_; }

    // Starts the job, once `online` is logged in. False with `*error` when it
    // cannot start at all -- no worker, or a server that does not share worlds.
    bool start(const Online& online, std::string* error);
    bool started() const { return job_ != nullptr; }

    // Once a frame: collects a worker that has finished.
    void pump();

    // Asks a running job to stop. The worker notices within a tenth of a
    // second, or at the end of a connect.
    void cancel();

    // **Withdraws a finished share**: one round trip to the server, on the
    // worker, so AlphaComputer drops the world now rather than a minute after
    // the login lapses. Does nothing unless the upload ended `Shared`.
    void withdraw(const Online& online);

    // The worker is still running -- the job, or a withdrawal.
    bool busy() const { return thread_ != nullptr; }
    bool withdrawn() const { return withdrawn_; }

    net::share::Progress progress() const;
    net::share::Stage stage() const;
    bool finished() const;

    // Only once the stage says they exist: see `net::share::Job`.
    const std::string& code() const;
    const std::string& sourceName() const;
    const std::string& error() const;

    // Whether `online` is on a different login from the one the share was made
    // under, which means the server has already dropped it.
    bool loginChanged(const Online& online) const;

private:
    static void entry(void* arg);
    void work();
    bool spawn();

    enum class Task : u8 { None, Run, Withdraw };

    io::FileSystem& fs_;
    bool exporting_ = false;
    std::string worldDir_;
    std::string worldName_;
    std::string savesDir_;
    std::string targetName_;
    std::string code_;

    std::unique_ptr<net::share::Upload> upload_;
    std::unique_ptr<net::share::Download> download_;
    net::share::Job* job_ = nullptr;

    // Read by the worker only while it runs, and set before it is spawned.
    Task task_ = Task::None;
    u8 token_[net::share::kTokenSize] = {};
    u32 serverAddress_ = 0;
    u16 transferPort_ = 0;
    net::TcpSocket socket_;

    void* thread_ = nullptr;
    std::atomic<bool> workerDone_{false};
    bool withdrawn_ = false;
};

}  // namespace mc::ctr
