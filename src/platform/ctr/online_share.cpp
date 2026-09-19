// A world shared over the internet, from the console's side. See
// online_share.hpp.

#include "platform/ctr/online_share.hpp"

#include "core/util/worker.hpp"
#include "platform/ctr/online.hpp"

#include <cstring>

namespace mc::ctr {

namespace {

const std::string kEmpty;

}  // namespace

OnlineShare::OnlineShare(io::FileSystem& fs) : fs_(fs) {}

OnlineShare::~OnlineShare()
{
    cancel();
    if (thread_ != nullptr) {
        // Bounded: every wait inside the job is cut into tenths of a second and
        // checks the cancel, and the one that is not -- a connect -- gives up
        // after `net::share::kConnectMs`.
        const WorkerJoin join = workerJoin();
        if (join != nullptr) {
            join(thread_);
        }
        thread_ = nullptr;
    }
}

void OnlineShare::exportWorld(const std::string& worldDir, const std::string& worldName)
{
    exporting_ = true;
    worldDir_ = worldDir;
    worldName_ = worldName;
}

void OnlineShare::importWorld(const std::string& savesDir, const std::string& targetName,
                              const std::string& code)
{
    exporting_ = false;
    savesDir_ = savesDir;
    targetName_ = targetName;
    code_ = code;
}

bool OnlineShare::spawn()
{
    workerDone_.store(false, std::memory_order_release);
    const WorkerSpawn spawnWorker = workerSpawn();
    if (spawnWorker != nullptr) {
        thread_ = spawnWorker(&OnlineShare::entry, this, WorkerRole::Net);
    }
    return thread_ != nullptr;
}

bool OnlineShare::start(const Online& online, std::string* error)
{
    if (job_ != nullptr) {
        return true;
    }
    if (!online.ready()) {
        *error = "Not connected to the server yet.";
        return false;
    }
    transferPort_ = online.client().transferPort();
    if (transferPort_ == 0) {
        *error = "This server does not share worlds.";
        return false;
    }
    serverAddress_ = online.serverAddress();
    std::memcpy(token_, online.client().token(), sizeof(token_));

    if (exporting_) {
        upload_ = std::make_unique<net::share::Upload>(fs_, worldDir_, worldName_, token_);
        job_ = upload_.get();
    } else {
        download_ = std::make_unique<net::share::Download>(fs_, savesDir_, targetName_, code_,
                                                           token_);
        job_ = download_.get();
    }

    // **No inline fallback.** Doing this on the frame would be a menu that
    // stops for as long as a world takes to cross the internet, which is not
    // slow-but-working, it is a hang. A console with no thread to spare is told.
    task_ = Task::Run;
    if (!spawn()) {
        upload_.reset();
        download_.reset();
        job_ = nullptr;
        task_ = Task::None;
        *error = "There is no thread free to send the world.";
        return false;
    }
    return true;
}

void OnlineShare::entry(void* arg)
{
    auto* self = static_cast<OnlineShare*>(arg);
    self->work();
    self->workerDone_.store(true, std::memory_order_release);
}

void OnlineShare::work()
{
    if (task_ == Task::Withdraw) {
        std::string error;
        net::TcpSocket socket;
        if (net::share::connectTransfer(socket, serverAddress_, transferPort_, &error)) {
            net::share::TcpStream stream(socket);
            net::share::stopSharing(stream, token_, &error);
        }
        // A withdrawal that did not happen costs a minute at most: the server
        // drops the world when this login lapses, which leaving the screen
        // makes it do. So its failure is not put in front of the player.
        return;
    }

    if (exporting_) {
        // The world's shape first, so one that cannot be read is refused
        // without a connection being spent on it.
        if (!upload_->prepare()) {
            return;
        }
    }
    std::string error;
    if (!net::share::connectTransfer(socket_, serverAddress_, transferPort_, &error)) {
        job_->abandon(error);
        return;
    }
    net::share::TcpStream stream(socket_);
    if (exporting_) {
        upload_->run(stream);
    } else {
        download_->run(stream);
    }
    socket_.close();
}

void OnlineShare::pump()
{
    if (thread_ != nullptr && workerDone_.load(std::memory_order_acquire)) {
        const WorkerJoin join = workerJoin();
        if (join != nullptr) {
            join(thread_);
        }
        thread_ = nullptr;
        if (task_ == Task::Withdraw) {
            withdrawn_ = true;
        }
        task_ = Task::None;
    }
}

void OnlineShare::cancel()
{
    if (job_ != nullptr) {
        job_->cancel();
    }
}

void OnlineShare::withdraw(const Online& online)
{
    if (thread_ != nullptr || withdrawn_ || job_ == nullptr || !exporting_
        || job_->stage() != net::share::Stage::Shared) {
        return;
    }
    // **The token the share was made under**, not whatever the login holds now:
    // a login that has changed is one the server has already dropped the world
    // for, and there is nothing left to withdraw.
    if (loginChanged(online) || !online.ready()) {
        withdrawn_ = true;
        return;
    }
    task_ = Task::Withdraw;
    if (!spawn()) {
        task_ = Task::None;
        withdrawn_ = true;
    }
}

net::share::Progress OnlineShare::progress() const
{
    return job_ != nullptr ? job_->progress() : net::share::Progress();
}

net::share::Stage OnlineShare::stage() const
{
    return job_ != nullptr ? job_->stage() : net::share::Stage::Connecting;
}

bool OnlineShare::finished() const
{
    return job_ != nullptr && job_->finished();
}

const std::string& OnlineShare::code() const
{
    return job_ != nullptr ? job_->code() : kEmpty;
}

const std::string& OnlineShare::sourceName() const
{
    return job_ != nullptr ? job_->worldName() : kEmpty;
}

const std::string& OnlineShare::error() const
{
    return job_ != nullptr ? job_->error() : kEmpty;
}

bool OnlineShare::loginChanged(const Online& online) const
{
    if (job_ == nullptr) {
        return false;
    }
    return !online.ready()
           || std::memcmp(online.client().token(), token_, sizeof(token_)) != 0;
}

}  // namespace mc::ctr
