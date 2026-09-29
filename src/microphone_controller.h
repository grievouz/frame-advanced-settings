#pragma once
#include "microphone_command.h"
#include "voice_clip_linux.h"
#include "panel.h"
#include "session_log.h"
#include <future>

class MicrophoneController {
    std::future<microphone::State> pending_;
    microphone::State state_;
    voiceclip::Player voice_{voiceclip::run};
    Command queued_ = Command::None;
    bool writing_ = false, wasVisible_ = false;
    std::string writeError_, lastLog_;
    std::chrono::steady_clock::time_point refreshAt_{};

    void publishVoice(PanelState &panel) const {
        const auto mode = voice_.mode();
        panel.voiceBusy = mode != voiceclip::Mode::Idle;
        panel.voiceReady = voice_.ready();
        panel.voiceStatus = mode == voiceclip::Mode::Recording
                                ? "Recording... " +
                                      std::to_string(std::max(1, voice_.secondsLeft())) +
                                      " s remaining"
                            : mode == voiceclip::Mode::Playing  ? "Playing..."
                            : mode == voiceclip::Mode::Stopping ? "Stopping..."
                                                                : voice_.error();
    }
    void publish(PanelState &panel) const {
        publishVoice(panel);
        panel.micAvailable = state_.available();
        panel.micBusy = writing_ || queued_ != Command::None;
        panel.micEcho = state_.echo;
        panel.micNoise = state_.noise;
        panel.micStatus = writeError_.empty() ? state_.message : writeError_;
        if (!state_.available() && pending_.valid() && panel.micStatus.empty())
            panel.micStatus = "Reading microphone settings...";
    }
    void start(Command command, PanelState &panel) {
        writing_ = command != Command::None;
        try {
            pending_ = std::async(std::launch::async, [command] {
                microphone::Service service([](const std::vector<std::string> &args) {
                    auto result = microphone::runCommand(args);
                    if (!result.ok)
                        sessionlog::warn("microphone",
                                         "wpctl failed: " + result.output.substr(0, 1024));
                    return result;
                });
                switch (command) {
                case Command::MicEchoOff:
                case Command::MicEchoOn:
                    sessionlog::info("microphone", command == Command::MicEchoOn
                                                       ? "Echo cancellation on"
                                                       : "Echo cancellation off");
                    return service.set(microphone::Filter::Echo, command == Command::MicEchoOn);
                case Command::MicNoiseOff:
                case Command::MicNoiseOn:
                    sessionlog::info("microphone", command == Command::MicNoiseOn
                                                       ? "Noise suppression on"
                                                       : "Noise suppression off");
                    return service.set(microphone::Filter::Noise, command == Command::MicNoiseOn);
                case Command::MicReset:
                    sessionlog::info("microphone", "Reset filters to SteamOS defaults");
                    return service.reset();
                default:
                    return service.read();
                }
            });
        } catch (const std::exception &e) {
            writing_ = false;
            state_.message = "Could not start microphone update. Try again.";
            sessionlog::error("microphone", e.what());
        }
        refreshAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        publish(panel);
    }

  public:
    void command(Command command, PanelState &panel) {
        if (command == Command::MicResetEcho) command = Command::MicEchoOn;
        if (command == Command::MicResetNoise) command = Command::MicNoiseOn;
        if (command == Command::VoiceStop) {
            voice_.stop();
            publishVoice(panel);
            return;
        }
        if (command == Command::VoiceRecord || command == Command::VoicePlay) {
            if (!writing_ && queued_ == Command::None) {
                if (command == Command::VoiceRecord)
                    voice_.record();
                else
                    voice_.play();
            }
            publishVoice(panel);
            return;
        }
        if (voice_.mode() != voiceclip::Mode::Idle)
            return;
        if (command < Command::MicEchoOff || command > Command::MicReset || !state_.available() ||
            writing_ || queued_ != Command::None)
            return;
        if (pending_.valid()) {
            queued_ = command;
            publish(panel);
        } else
            start(command, panel);
    }
    void poll(PanelState &panel, bool visible) {
        voice_.poll(visible);
        publishVoice(panel);
        if (pending_.valid() &&
            pending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                state_ = pending_.get();
            } catch (const std::exception &e) {
                sessionlog::error("microphone", e.what());
                state_ = {};
                state_.message = "Could not update microphone settings. See the app log.";
            }
            if (writing_)
                writeError_ = state_.message;
            writing_ = false;
            const auto detail = "backend=" + std::to_string(int(state_.backend)) +
                                " echo=" + std::to_string(state_.echo) +
                                " noise=" + std::to_string(state_.noise) + " " + state_.message;
            if (detail != lastLog_) {
                sessionlog::info("microphone", detail);
                lastLog_ = detail;
            }
            publish(panel);
            if (queued_ != Command::None) {
                const auto command = queued_;
                queued_ = Command::None;
                start(command, panel);
            }
        }
        if (!pending_.valid() && visible &&
            (!wasVisible_ || std::chrono::steady_clock::now() >= refreshAt_))
            start(Command::None, panel);
        wasVisible_ = visible;
    }
};
