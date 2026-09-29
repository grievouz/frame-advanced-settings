#include "voice_clip_player.h"
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace voiceclip;
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
template <typename Predicate> void waitFor(Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!ready()) {
        check(std::chrono::steady_clock::now() < deadline, "Worker did not settle");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
int main() {
    Capture capture;
    Audio warmup(warmupBytes, 0x17);
    capture.append(warmup.data(), 1);
    capture.append(warmup.data() + 1, warmup.size() - 1);
    check(capture.size() == 0 && capture.secondsLeft() == 6, "Filter warmup is excluded from clip");
    Audio data(clipBytes + 913, 0x83);
    capture.append(data.data(), bytesPerSecond + 1);
    check(capture.secondsLeft() == 5 && !capture.full(), "Capture duration follows samples");
    capture.append(data.data(), data.size());
    check(capture.full() && capture.size() == 576000, "Exactly six seconds despite excess input");
    capture.append(data.data(), data.size());
    auto audio = capture.take();
    check(audio.size() == clipBytes && audio.front() == 0x83 && audio.back() == 0x83,
          "Warmup and excess bytes are never retained");
    Capture crossing;
    Audio combined(warmupBytes + 37, 0x11);
    std::fill(combined.begin() + warmupBytes, combined.end(), 0x92);
    crossing.append(combined.data(), combined.size());
    check(crossing.size() == 37 && crossing.take().front() == 0x92,
          "Odd pipe chunks crossing warmup");

    std::atomic<bool> finish{false}, fail{false}, stopped{false};
    std::atomic<int> starts{0};
    Runner runner = [&](bool record, const std::shared_ptr<Control> &control,
                        const std::shared_ptr<const Audio> &clip) {
        ++starts;
        if (!record)
            check(clip && clip->size() == clipBytes, "Playback receives complete in-memory clip");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!finish && !control->stop && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (control->stop)
            stopped = true;
        if (fail)
            return Result{{}, "Fixture audio error"};
        // Deliberately return complete audio even after cancellation to test
        // the owner's rejection of a late result when the tab has closed.
        return Result{record ? Audio(clipBytes, 0x42) : Audio{}, {}};
    };
    Player player(runner);
    player.play();
    check(player.mode() == Mode::Idle && starts == 0, "Cannot play without a clip");
    player.record();
    waitFor([&] { return starts == 1; });
    player.record();
    player.play();
    check(starts == 1 && player.mode() == Mode::Recording && !player.ready(),
          "No overlapping jobs");
    finish = true;
    waitFor([&] {
        player.poll(true);
        return player.mode() == Mode::Idle;
    });
    check(player.ready() && player.error().empty(), "Completed recording enables Play");
    finish = false;
    player.play();
    waitFor([&] { return starts == 2; });
    player.poll(false);
    waitFor([&] {
        player.poll(false);
        return player.mode() == Mode::Idle;
    });
    check(stopped && player.ready(), "Hiding page stops playback and retains the completed clip");
    stopped = false;
    player.record();
    check(!player.ready(), "Starting another recording replaces the previous clip");
    waitFor([&] { return starts == 3; });
    player.poll(false);
    waitFor([&] {
        player.poll(false);
        return player.mode() == Mode::Idle;
    });
    check(stopped && !player.ready(), "Closing page discards even a late completed capture result");
    fail = true;
    finish = true;
    player.record();
    waitFor([&] {
        player.poll(true);
        return player.mode() == Mode::Idle;
    });
    check(!player.ready() && !player.error().empty(),
          "Capture failure is visible and leaves no playable clip");
    fail = false;
    finish = false;
    stopped = false;
    {
        Player closing(runner);
        const int previous = starts;
        closing.record();
        waitFor([&] { return starts > previous; });
    }
    check(stopped, "App destruction cancels audio before its worker is destroyed");
    std::cout << "Six-second bounds, warmup, capture/play sequencing, cancellation and lifetime "
                 "passed.\n";
}
