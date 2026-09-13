// The background thread behind the main menu's previews: wanting replaces,
// results come back as data, and quiesce forgets a world. See
// core/preview/preview_worker.hpp.

#include "framework.hpp"

#include "core/preview/preview_worker.hpp"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace mc;
using namespace mc::preview;

namespace {

struct Log {
    std::vector<std::string> ran;
    int stopEarly = 0;
};

void recordJob(void* context, PreviewWorker& worker, const PreviewJob& job)
{
    Log* log = static_cast<Log*>(context);
    log->ran.push_back(job.key);
    PreviewResult result;
    result.kind = job.kind;
    result.index = job.index;
    result.key = job.key;
    result.ok = worker.stillWanted(job);
    worker.post(std::move(result));
}

PreviewJob job(const char* key, int index)
{
    PreviewJob out;
    out.key = key;
    out.index = index;
    return out;
}

}  // namespace

TEST(preview_worker_runs_wanted_jobs_in_the_order_given)
{
    Log log;
    PreviewWorker worker;
    // runOne calls the handler the worker was started with, so start and stop
    // it first -- with nothing queued, so the thread has nothing to take --
    // and from then on runOne is the whole of the scheduling.
    CHECK(worker.start(&recordJob, &log, WorkerRole::Io));
    worker.stop();

    std::vector<PreviewJob> jobs;
    jobs.push_back(job("b", 1));
    jobs.push_back(job("c", 2));
    jobs.push_back(job("a", 0));
    worker.setWanted(0, jobs);

    while (worker.runOne()) {
    }
    CHECK_EQ(log.ran.size(), usize(3));
    CHECK(log.ran[0] == "b");
    CHECK(log.ran[1] == "c");
    CHECK(log.ran[2] == "a");

    PreviewResult result;
    CHECK(worker.poll(&result));
    CHECK(result.key == "b");
    CHECK(result.ok);
}

TEST(preview_worker_forgets_what_is_no_longer_wanted)
{
    Log log;
    PreviewWorker worker;
    CHECK(worker.start(&recordJob, &log, WorkerRole::Io));
    worker.stop();

    std::vector<PreviewJob> first;
    first.push_back(job("old", 0));
    first.push_back(job("keep", 1));
    worker.setWanted(1, first);

    std::vector<PreviewJob> second;
    second.push_back(job("keep", 1));
    worker.setWanted(1, second);

    PreviewJob probe;
    probe.kind = 1;
    probe.key = "old";
    CHECK(!worker.stillWanted(probe));
    probe.key = "keep";
    CHECK(worker.stillWanted(probe));

    while (worker.runOne()) {
    }
    CHECK_EQ(log.ran.size(), usize(1));
    CHECK(log.ran[0] == "keep");

    // Another kind's list is left alone by this one's.
    worker.setWanted(2, std::vector<PreviewJob>{job("other", 0)});
    CHECK(worker.stillWanted(probe));
}

TEST(preview_worker_quiesce_drops_a_world_from_every_queue)
{
    Log log;
    PreviewWorker worker;
    CHECK(worker.start(&recordJob, &log, WorkerRole::Io));
    worker.stop();

    worker.setWanted(0, std::vector<PreviewJob>{job("world", 0), job("next", 1)});
    worker.setWanted(1, std::vector<PreviewJob>{job("world", 0)});
    worker.quiesce("world");

    PreviewJob probe;
    probe.key = "world";
    probe.kind = 0;
    CHECK(!worker.stillWanted(probe));
    while (worker.runOne()) {
    }
    CHECK_EQ(log.ran.size(), usize(1));
    CHECK(log.ran[0] == "next");
}

TEST(preview_worker_on_a_thread_delivers_results_and_stops)
{
    Log log;
    PreviewWorker worker;
    CHECK(worker.start(&recordJob, &log, WorkerRole::Io));
    worker.setWanted(0, std::vector<PreviewJob>{job("x", 3), job("y", 4)});

    int received = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received < 2 && std::chrono::steady_clock::now() < deadline) {
        PreviewResult result;
        if (worker.poll(&result)) {
            CHECK(result.index == 3 || result.index == 4);
            ++received;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    CHECK_EQ(received, 2);
    worker.quiesce("x");
    worker.stop();
    CHECK(!worker.running());
}
