#pragma once
#include "drag.h"
#include "gravity.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace drag {
constexpr uint64_t temporaryUniverse = 424242;
struct Space {
    Transform pose;
    std::optional<Transform> seated;
    // Diagnostic metadata only. Moving the origin changes the coordinates in
    // which Frame reports these walls; it is not evidence of a failed write.
    std::vector<std::array<Vec, 4>> bounds;
    uint64_t universe = 0;
};
struct RuntimeBoundsOrigins {
    Transform before, after;
};
inline bool sameOrigin(const Transform &a, const Transform &b) {
    if (!a.valid() || !b.valid())
        return false;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (std::abs(a.r[i][j] - b.r[i][j]) > .0001)
                return false;
    // Float readback error increases with the translation's magnitude.
    const Vec d = a.t - b.t;
    return std::abs(d.x) <= .0001 * (1 + std::abs(b.t.x)) &&
           std::abs(d.y) <= .0001 * (1 + std::abs(b.t.y)) &&
           std::abs(d.z) <= .0001 * (1 + std::abs(b.t.z));
}
inline bool same(const Space &a, const Space &b) {
    return a.universe == b.universe && sameOrigin(a.pose, b.pose) &&
           a.seated.has_value() == b.seated.has_value() &&
           (!a.seated || sameOrigin(*a.seated, *b.seated));
}
inline bool liveNear(const Transform &a, const Transform &b) {
    if (!a.valid() || !b.valid() || length(a.t - b.t) > .01)
        return false;
    double rotationError = 0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            rotationError += std::pow(a.r[i][j] - b.r[i][j], 2);
    return rotationError <= 8 * std::pow(std::sin(.01 / 2), 2);
}
inline bool onlyHeightDiffers(Transform a, const Transform &b) {
    if (!a.valid() || std::abs(a.t.y - b.t.y) <= .01)
        return false;
    a.t.y = b.t.y;
    return liveNear(a, b);
}
inline const char *changeReason(const Space &expected, const Space &actual) {
    if (expected.universe != actual.universe)
        return "Tracking universe changed.";
    return "Standing or seated origin changed.";
}
enum class SpaceEvent { Reset, DataChanged, FlushCache, SetupStarted, SetupFinished };
struct SpaceAccess {
    virtual ~SpaceAccess() = default;
    virtual bool read(Space &result) = 0;
    virtual std::string readProblem() const { return "Playspace data unavailable"; }
    virtual bool write(const Space &target) = 0;
    virtual bool saveBaseline(const Space &baseline) = 0;
    virtual void cancelPreview() = 0;
    virtual void reportMismatch(const char *, const Space &, const Space &) {}
    virtual void recoveryLog(const std::string &) {}
    virtual double seconds() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    virtual uint64_t currentUniverse() {
        Space s;
        return read(s) ? s.universe : 0;
    }
    virtual bool headTracked() { return true; }
    virtual bool liveStanding(Transform &) { return false; }
    // Must back up the actual saved configuration before a disk commit and
    // refuse if the working copy is no longer this saved space.
    virtual bool reanchor(const Space &) { return false; }
};

class Session {
    enum class Recovery { None, Debouncing, Restoring, RoomSetup };
    SpaceAccess &access_;
    Recovery recovery_ = Recovery::None;
    bool movementRequested_ = false, startupEnablePending_ = false;
    bool roomProcessActive_ = false;
    bool keepOffset_ = false, repairFloor_ = false, reanchorTried_ = false;
    Vec savedRawOffset_;
    uint64_t universe_ = 0;
    double due_ = 0, lastApply_ = -100, ownCommitUntil_ = -100;
    double candidateSince_ = 0, backoffUntil_ = 0;
    std::optional<Space> removedPreview_, candidate_;
    std::vector<double> heals_;
    unsigned liveMismatchReads_ = 0;

    void message(const std::string &text, bool attention = false) {
        if (status != text)
            access_.recoveryLog(text);
        status = text;
        statusAttention = attention;
    }
    void schedule(bool dropOffset, const char *reason, double delay = .3) {
        if (!enabled && !startupEnablePending_ && !owned)
            return;
        if (!resynchronizing) {
            savedRawOffset_ = baseline.pose.rotate(engine.offset);
            keepOffset_ = true;
        }
        if (dropOffset) {
            keepOffset_ = false;
            repairFloor_ = false;
        }
        resynchronizing = true;
        // Late events delay capture but must not restart cleanup or let the old
        // preview become the new baseline.
        if (recovery_ != Recovery::Restoring)
            recovery_ = Recovery::Debouncing;
        due_ = access_.seconds() + delay;
        candidate_.reset();
        engine.release();
        gravity.stop();
        message(std::string(reason) + " Recovering playspace.");
    }
    void hidePreview() {
        if (owned && !sameOrigin(expected.pose, baseline.pose))
            removedPreview_ = expected;
        access_.cancelPreview();
        owned = false;
        lastApply_ = -100;
        liveMismatchReads_ = 0;
    }
    bool readySpace(const Space &space) {
        return space.universe != 0 && space.universe != temporaryUniverse && space.pose.valid() &&
               (!space.seated || space.seated->valid()) && access_.headTracked();
    }
    bool capture() {
        const double now = access_.seconds();
        if (now < due_)
            return false;
        if (recovery_ == Recovery::RoomSetup)
            recovery_ = Recovery::Debouncing;
        if (recovery_ == Recovery::Debouncing) {
            hidePreview();
            recovery_ = Recovery::Restoring;
            due_ = now + .05;
            candidate_.reset();
            return false;
        }
        Space actual;
        if (!access_.read(actual) || !readySpace(actual)) {
            candidate_.reset();
            message("Waiting for a tracked, saved playspace.");
            return false;
        }
        // HideWorkingSetPreview is asynchronous. Even a stable, readable old
        // preview is not proof that the saved calibration has been restored.
        if (removedPreview_ && sameOrigin(actual.pose, removedPreview_->pose)) {
            candidate_.reset();
            message("Waiting for SteamVR to restore the saved playspace.");
            return false;
        }
        if (!candidate_ || !same(actual, *candidate_)) {
            candidate_ = actual;
            candidateSince_ = now;
            return false;
        }
        if (now - candidateSince_ < .2)
            return false;
        if (!access_.saveBaseline(actual)) {
            message("Could not save the playspace backup; retrying.", true);
            due_ = now + 2;
            return false;
        }
        if (repairFloor_) {
            repairFloor_ = false;
            const bool repaired = access_.reanchor(actual);
            if (repaired) {
                ownCommitUntil_ = now + 3;
                candidate_.reset();
                due_ = now + .3;
            }
            access_.recoveryLog(
                repaired ? "Re-anchored the saved floor; restoring the offset."
                         : "Floor repair unavailable; leaving saved calibration unchanged.");
            if (repaired)
                return false;
        }
        baseline = expected = actual;
        universe_ = actual.universe;
        engine.reset();
        engine.offset = keepOffset_ ? baseline.pose.unrotate(savedRawOffset_) : Vec{};
        // A held hand gets a fresh anchor in the new coordinate system.
        engine.reanchor();
        gravity.stop();
        removedPreview_.reset();
        candidate_.reset();
        resynchronizing = false;
        recovery_ = Recovery::None;
        startupEnablePending_ = false;
        message("Playspace ready.");
        if (length(engine.offset) > .00001)
            return apply();
        return true;
    }
    void checkLive(double now) {
        if (!owned || engine.held() || now - lastApply_ < .6 || now < backoffUntil_)
            return;
        Transform live;
        if (!access_.liveStanding(live) || !live.valid()) {
            liveMismatchReads_ = 0;
            return;
        }
        if (liveNear(live, expected.pose)) {
            liveMismatchReads_ = 0;
            return;
        }
        // Require repeated evidence. A gesture or gravity already writes the
        // offset; this repair handles previews dropped after movement stops.
        if (++liveMismatchReads_ < 3)
            return;
        liveMismatchReads_ = 0;
        Space observed = expected;
        observed.pose = live;
        access_.reportMismatch("live-preview", expected, observed);
        if (onlyHeightDiffers(live, expected.pose) && !reanchorTried_) {
            reanchorTried_ = true;
            schedule(false, "SteamVR restored the saved floor height.");
            repairFloor_ = true;
            return;
        }
        heals_.erase(std::remove_if(heals_.begin(), heals_.end(),
                                    [now](double time) { return now - time > 20; }),
                     heals_.end());
        if (heals_.size() >= 3) {
            backoffUntil_ = now + 30;
            heals_.clear();
            message("SteamVR keeps replacing the offset. Recovery waits 30 seconds.", true);
            return;
        }
        heals_.push_back(now);
        access_.recoveryLog("SteamVR dropped the live preview; reapplying the offset.");
        apply();
    }

  public:
    explicit Session(SpaceAccess &access) : access_(access) {}
    Engine engine;
    Gravity gravity;
    Space baseline, expected;
    bool enabled = false, owned = false, resynchronizing = false;
    bool statusAttention = false;
    std::string status = "Ready. Enable to test native playspace movement.";
    bool needsRestore() const { return owned; }
    bool movementRequested() const { return movementRequested_; }
    bool startupEnablePending() const { return startupEnablePending_; }

    void restoreMovementPreference(bool requested) {
        movementRequested_ = startupEnablePending_ = requested;
        if (requested)
            message("Restoring saved movement setting; waiting for playspace.");
    }
    bool requestMovement(bool requested) {
        movementRequested_ = requested;
        startupEnablePending_ = false;
        if (requested)
            return enabled || enable(true);
        disable();
        return true;
    }
    bool enable(bool waitForPlayspace = false) {
        if (roomProcessActive_) {
            startupEnablePending_ = waitForPlayspace;
            message("Waiting for room setup to finish.");
            return false;
        }
        if (removedPreview_) {
            enabled = true;
            schedule(true, "Waiting for the previous preview to close.", 0);
            return false;
        }
        Space actual;
        if (!access_.read(actual) || !readySpace(actual)) {
            startupEnablePending_ = waitForPlayspace;
            message("Waiting for a tracked, saved playspace.", true);
            return false;
        }
        if (!access_.saveBaseline(actual)) {
            startupEnablePending_ = false;
            message("Cannot save original playspace. Movement stays paused.", true);
            return false;
        }
        baseline = expected = actual;
        universe_ = actual.universe;
        engine.reset();
        gravity.stop();
        enabled = true;
        startupEnablePending_ = false;
        resynchronizing = false;
        recovery_ = Recovery::None;
        message("Close the dashboard; hold your drag binding and move that controller.");
        return true;
    }
    void event(SpaceEvent event) {
        if (!enabled && !owned)
            return;
        const double now = access_.seconds();
        if (event == SpaceEvent::SetupStarted) {
            schedule(true, "Room setup opened.", 90);
            hidePreview();
            engine.reset();
            recovery_ = Recovery::RoomSetup;
            return;
        }
        if (event == SpaceEvent::FlushCache) {
            watch();
            return;
        }
        if (recovery_ == Recovery::RoomSetup && event != SpaceEvent::SetupFinished)
            return;
        if (event == SpaceEvent::Reset && owned &&
            (now - lastApply_ < .5 || length(engine.offset) < .00001)) {
            Space working;
            if (access_.read(working) && same(working, expected))
                return;
        }
        const bool drop = (event == SpaceEvent::Reset || event == SpaceEvent::SetupFinished) &&
                          now >= ownCommitUntil_;
        schedule(drop, drop ? "Playspace reset." : "Playspace data reloaded.");
    }
    void roomSetupActive(bool active) {
        if (roomProcessActive_ == active)
            return;
        roomProcessActive_ = active;
        event(active ? SpaceEvent::SetupStarted : SpaceEvent::SetupFinished);
    }
    bool watch() {
        if (roomProcessActive_)
            return false;
        if (startupEnablePending_ && !enabled)
            return enable(true);
        if (!enabled)
            return true;
        const double now = access_.seconds();
        const uint64_t id = access_.currentUniverse();
        if (id && id != universe_ && recovery_ != Recovery::RoomSetup) {
            const bool newWorld = id == temporaryUniverse || universe_ == temporaryUniverse;
            access_.recoveryLog("Tracking universe " + std::to_string(universe_) + " -> " +
                                std::to_string(id));
            universe_ = id;
            if (newWorld)
                reanchorTried_ = false;
            schedule(newWorld, newWorld ? "Tracking world changed." : "Tracking map changed.");
        }
        if (resynchronizing)
            return capture();
        if (!access_.headTracked() || id == 0 || id == temporaryUniverse)
            return false;
        Space actual;
        if (!access_.read(actual)) {
            schedule(false, "Playspace temporarily unavailable.");
            return false;
        }
        if (!readySpace(actual) || actual.universe != id) {
            schedule(false, "Tracking changed during playspace read.");
            return false;
        }
        if (!owned) {
            baseline = expected = actual;
            return true;
        }
        if (now - lastApply_ < .6)
            return true;
        if (!same(actual, expected)) {
            access_.reportMismatch("working-copy", expected, actual);
            schedule(true, "Playspace edited externally.", 3);
            return false;
        }
        checkLive(now);
        return !resynchronizing;
    }
    void setDistanceLimit(bool active, double meters) {
        engine.distanceLimit.enabled = active;
        engine.distanceLimit.setMeters(meters);
        engine.release();
        gravity.stop();
    }
    bool apply() {
        if (!enabled || resynchronizing || !finite(engine.offset))
            return false;
        if (!owned && !access_.saveBaseline(baseline)) {
            schedule(false, "Cannot save original playspace.", 2);
            return false;
        }
        Space target = baseline;
        target.pose = withOffset(baseline.pose, engine.offset);
        // Move the standing viewpoint only. Retain the seated reference used
        // by Frame's room-space overlays instead of translating it with us.
        if (!target.pose.valid() || (target.seated && !target.seated->valid()))
            return false;
        owned = true;
        expected = target;
        if (!access_.write(target)) {
            schedule(false, "Preview submission failed.");
            return false;
        }
        lastApply_ = access_.seconds();
        liveMismatchReads_ = 0;
        return true;
    }
    bool updateMotion(bool allowed, bool pressed, bool tracked, Vec hand, double dt) {
        if (!enabled || resynchronizing || !allowed || !tracked || !finite(hand) ||
            !std::isfinite(dt) || dt <= 0 || dt > .1) {
            engine.release();
            gravity.stop();
            return false;
        }
        const bool wasHeld = engine.held();
        const bool dragged = engine.sample(pressed, tracked, hand, baseline.pose, dt);
        if (wasHeld && !pressed)
            gravity.launch(engine.releasedVelocity());
        const bool fell =
            gravity.step(engine.offset, dt, pressed || engine.held(), engine.distanceLimit);
        return (dragged || fell) && apply();
    }
    bool reset() {
        engine.reset();
        gravity.stop();
        if (!enabled) {
            hidePreview();
            return true;
        }
        if (!owned && !resynchronizing) {
            message("Offset reset.");
            return true;
        }
        schedule(true, "Offset reset.", 0);
        return capture();
    }
    void abandon(const char *reason) {
        disable();
        message(reason, true);
    }
    void disable() {
        hidePreview();
        enabled = startupEnablePending_ = resynchronizing = false;
        recovery_ = Recovery::None;
        repairFloor_ = keepOffset_ = false;
        candidate_.reset();
        engine.reset();
        gravity.stop();
        message("Paused.");
    }
};
} // namespace drag
