#pragma once
#include "voice_clip.h"
#include "session_log.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>

namespace voiceclip {
enum class Mode { Idle, Recording, Playing, Stopping };
struct Result {
    Audio samples;
    std::string error;
};
struct Control {
    std::atomic<bool> stop{false};
    std::atomic<size_t> captured{0};
};

using Runner = std::function<Result(bool, const std::shared_ptr<Control> &,
                                    const std::shared_ptr<const Audio> &)>;
class Player {
    Runner run_;
    std::future<Result> pending_;
    std::shared_ptr<Control> control_;
    std::shared_ptr<const Audio> clip_;
    Mode mode_ = Mode::Idle;
    std::string error_;
    void start(bool record) {
        if (pending_.valid() || (!record && !ready()))
            return;
        error_.clear();
        if (record)
            clip_.reset();
        control_ = std::make_shared<Control>();
        try {
            pending_ = std::async(std::launch::async, run_, record, control_, clip_);
            mode_ = record ? Mode::Recording : Mode::Playing;
            sessionlog::info("microphone", record ? "Six-second recording requested (memory only)"
                                                  : "Clip playback requested");
        } catch (const std::exception &e) {
            mode_ = Mode::Idle;
            error_ = "Could not start voice check.";
            sessionlog::error("microphone", e.what());
        }
    }

  public:
    explicit Player(Runner run) : run_(std::move(run)) {}
    ~Player() {
        stop();
        if (pending_.valid())
            pending_.wait();
    }
    void record() { start(true); }
    void play() { start(false); }
    void stop() {
        if (!pending_.valid())
            return;
        control_->stop = true;
        mode_ = Mode::Stopping;
    }
    void poll(bool visible) {
        if (!visible)
            stop();
        if (!pending_.valid() ||
            pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        try {
            auto result = pending_.get();
            if (!control_->stop) {
                error_ = result.error;
                if (mode_ == Mode::Recording && result.samples.size() == clipBytes)
                    clip_ = std::make_shared<const Audio>(std::move(result.samples));
            }
        } catch (const std::exception &e) {
            error_ = "Voice check failed. Try again.";
            sessionlog::error("microphone", e.what());
        }
        sessionlog::info("microphone",
                         "Voice check stopped; completed_clip=" + std::to_string(ready()));
        mode_ = Mode::Idle;
    }
    bool ready() const { return clip_ && clip_->size() == clipBytes; }
    Mode mode() const { return mode_; }
    const std::string &error() const { return error_; }
    int secondsLeft() const {
        const auto bytes = control_ ? std::min(clipBytes, control_->captured.load()) : size_t(0);
        return int((clipBytes - bytes + bytesPerSecond - 1) / bytesPerSecond);
    }
};
} // namespace voiceclip
