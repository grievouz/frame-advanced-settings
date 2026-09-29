#pragma once
#include "drag.h"
#include "gravity.h"
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace drag {
struct Space {
    Transform pose;
    // Boundary metadata in the baseline representation. The OpenVR adapter may
    // normalize a verified coordinate re-expression; it never writes bounds.
    std::vector<std::array<Vec, 4>> bounds;
    uint64_t universe = 0;
};

inline bool sameBounds(const Space &a, const Space &b) {
    if (a.bounds.size() != b.bounds.size())
        return false;
    for (size_t i = 0; i < a.bounds.size(); ++i)
        for (size_t j = 0; j < 4; ++j)
            if (!finite(a.bounds[i][j]) || !finite(b.bounds[i][j]) ||
                length(a.bounds[i][j] - b.bounds[i][j]) > .002)
                return false;
    return true;
}
inline bool same(const Space &a, const Space &b) {
    return a.universe == b.universe && near(a.pose, b.pose) && sameBounds(a, b);
}

// Frame can expose a runtime origin with our horizontal preview translation
// but the original floor height. Derive that specific alternative from known
// poses; do not fit a transform to unknown boundary data or ignore its Y axis.
inline Transform previewAtBaselineFloor(const Transform &baseline, const Transform &preview) {
    Vec translation = baseline.unrotate(preview.t - baseline.t);
    translation.y = 0;
    Transform result = preview;
    result.t = baseline.point(translation);
    return result;
}
inline bool knownPreviewOrigin(const Transform &origin, const Transform &baseline,
                               const Transform &preview) {
    return near(origin, baseline) || near(origin, preview) ||
           near(origin, previewAtBaselineFloor(baseline, preview));
}
struct RuntimeBoundsOrigins {
    // Standing-to-raw transforms sampled immediately before/after live bounds.
    Transform before, after;
};
enum class BoundsMatch { Unchanged, Reexpressed, RuntimeReexpressed, Different };
inline bool samePhysicalBounds(const Space &actual, const Space &baseline,
                               const Transform &boundsOrigin) {
    if (!boundsOrigin.valid())
        return false;
    if (actual.bounds.size() != baseline.bounds.size() || actual.bounds.empty())
        return false;
    for (size_t i = 0; i < actual.bounds.size(); ++i)
        for (size_t j = 0; j < 4; ++j) {
            const Vec a = actual.bounds[i][j], b = baseline.bounds[i][j];
            if (!finite(a) || !finite(b) ||
                length(boundsOrigin.point(a) - baseline.pose.point(b)) > .002)
                return false;
        }
    return true;
}
// Match every physical corner through a reported origin, retaining the working
// pose for write verification. No fitted translation or increased tolerance.
inline BoundsMatch normalizePreviewBounds(Space &actual, const Space &baseline,
                                          const RuntimeBoundsOrigins *runtime = nullptr) {
    if (actual.universe != baseline.universe || !actual.pose.valid() || !baseline.pose.valid())
        return BoundsMatch::Different;
    if (sameBounds(actual, baseline))
        return BoundsMatch::Unchanged;
    if (samePhysicalBounds(actual, baseline, actual.pose)) {
        actual.bounds = baseline.bounds;
        return BoundsMatch::Reexpressed;
    }
    // Only use the live-coordinate alternative when both samples agree and
    // match the baseline, current preview, or that preview at the baseline floor.
    if (runtime && near(runtime->before, runtime->after) &&
        knownPreviewOrigin(runtime->before, baseline.pose, actual.pose) &&
        knownPreviewOrigin(runtime->after, baseline.pose, actual.pose) &&
        samePhysicalBounds(actual, baseline, runtime->before) &&
        samePhysicalBounds(actual, baseline, runtime->after)) {
        actual.bounds = baseline.bounds;
        return BoundsMatch::RuntimeReexpressed;
    }
    return BoundsMatch::Different;
}

inline const char *changeReason(const Space &expected, const Space &actual) {
    if (expected.universe != actual.universe)
        return "Tracking universe changed.";
    if (!near(expected.pose, actual.pose))
        return "Standing origin changed.";
    return "Boundary geometry changed.";
}

// Keep ownership/recovery logic independent of OpenVR so failure paths can be
// exercised without moving a real user's playspace.
enum class WriteMode { Preview, Restore };
struct SpaceAccess {
    virtual ~SpaceAccess() = default;
    virtual bool read(Space &result) = 0;
    virtual std::string readProblem() const { return "Playspace data unavailable"; }
    virtual bool write(const Space &target, WriteMode mode) = 0;
    virtual bool saveBaseline(const Space &baseline) = 0;
    virtual void cancelPreview() = 0;
    virtual void reportMismatch(const char *, const Space &, const Space &) {}
};

class Session {
    SpaceAccess &access_;
    std::optional<Space> pending_;
    std::optional<Space> resyncCandidate_;
    unsigned stableReads_ = 0;
    bool previewMayRecover_ = true;
    bool movementRequested_ = false, startupEnablePending_ = false;
    // A compositor notification can arrive after a newer preview was submitted.
    // Keep a bounded set of our confirmed poses, not a time window in which all
    // recenter events are ignored. Boundary/universe checks still run every time.
    std::deque<Transform> recentPoses_;

    void rememberPose(const Transform &pose) {
        if (!recentPoses_.empty() && near(recentPoses_.back(), pose))
            return;
        recentPoses_.push_back(pose);
        if (recentPoses_.size() > 64)
            recentPoses_.pop_front();
    }

    bool pause(const char *reason) {
        enabled = false;
        startupEnablePending_ = false;
        resynchronizing = false;
        resyncCandidate_.reset();
        stableReads_ = 0;
        engine.release();
        gravity.stop();
        status = reason;
        statusAttention = true;
        return false;
    }
    void syncOffset() {
        engine.offset = owned ? baseline.pose.unrotate(baseline.pose.t - expected.pose.t) : Vec{};
    }
    bool knownRuntimeOrigin(const Transform &origin) const {
        if (!origin.valid())
            return false;
        if (knownPreviewOrigin(origin, baseline.pose, expected.pose))
            return true;
        for (const auto &pose : recentPoses_)
            if (knownPreviewOrigin(origin, baseline.pose, pose))
                return true;
        return false;
    }
    void beginResync(const char *reason) {
        if (!enabled) {
            abandon(reason);
            return;
        }
        // A transient read or universe switch must not hide the preview in the
        // middle of a held drag. Stop writes first, retaining our last confirmed
        // offset. A persistent change is discarded only after a valid release.
        resynchronizing = true;
        previewMayRecover_ = true;
        resyncCandidate_.reset();
        stableReads_ = 0;
        syncOffset();
        engine.release();
        gravity.stop();
        status = std::string(reason) + " Movement suspended; checking playspace.";
        statusAttention = true;
    }
    void discardForResync() {
        access_.cancelPreview();
        owned = false;
        pending_.reset();
        recentPoses_.clear();
        engine.reset();
        gravity.stop();
        resyncCandidate_.reset();
        stableReads_ = 0;
        status = "Old preview discarded. Waiting for a stable live playspace.";
    }
    bool resynchronize(bool dragReleased) {
        Space actual;
        if (!access_.read(actual)) {
            resyncCandidate_.reset();
            stableReads_ = 0;
            status = "Waiting for playspace: " + access_.readProblem();
            return false;
        }
        if (!resyncCandidate_ || !same(actual, *resyncCandidate_))
            stableReads_ = 0;
        resyncCandidate_ = actual;
        // Only watch() advances this counter, normally at 100 ms intervals.
        // No movement writes or preview cleanup occur while checking stability.
        stableReads_ = std::min(stableReads_ + 1, 3u);
        if (stableReads_ < 3)
            return false;
        if (needsRestore()) {
            if (previewMayRecover_ &&
                (same(actual, expected) || (pending_ && same(actual, *pending_)))) {
                expected = actual;
                rememberPose(actual.pose);
                owned = true;
                pending_.reset();
                syncOffset();
                engine.release();
                resynchronizing = false;
                resyncCandidate_.reset();
                stableReads_ = 0;
                status =
                    "Confirmed playspace returned; kept offset. Release and regrab to continue.";
                statusAttention = false;
                return true;
            }
            if (!dragReleased) {
                status = "Playspace changed. Release drag to reset the old offset and synchronize.";
                return false;
            }
            discardForResync();
            return false;
        }
        if (!access_.saveBaseline(actual))
            return pause("Cannot save new playspace. Movement stays paused.");
        baseline = expected = actual;
        recentPoses_.clear();
        rememberPose(actual.pose);
        engine.reset();
        gravity.stop();
        resynchronizing = false;
        resyncCandidate_.reset();
        stableReads_ = 0;
        status = "Playspace synchronized. Release and hold your drag binding to continue.";
        statusAttention = false;
        return true;
    }
    bool acceptKnown(const Space &actual) {
        if (same(actual, expected)) {
            pending_.reset();
            syncOffset();
            return true;
        }
        if (pending_ && same(actual, *pending_)) {
            expected = actual;
            rememberPose(actual.pose);
            owned = true;
            pending_.reset();
            syncOffset();
            return true;
        }
        access_.reportMismatch("observed-state", expected, actual);
        beginResync(changeReason(expected, actual));
        return false;
    }
    bool observe() {
        if (resynchronizing)
            return false;
        Space actual;
        if (!access_.read(actual)) {
            if (enabled)
                beginResync(("Playspace unavailable: " + access_.readProblem()).c_str());
            else
                pause(("Paused: " + access_.readProblem()).c_str());
            return false;
        }
        if (!needsRestore()) {
            if (actual.universe != baseline.universe) {
                access_.reportMismatch("idle-universe", expected, actual);
                beginResync(changeReason(expected, actual));
                return false;
            }
            // With no preview or unconfirmed write, we own no runtime state.
            // Follow the live reference instead of treating idle submillimetre
            // floor/boundary drift as a failed movement write.
            baseline = expected = actual;
            recentPoses_.clear();
            rememberPose(actual.pose);
            return true;
        }
        return acceptKnown(actual);
    }

  public:
    explicit Session(SpaceAccess &access) : access_(access) {}
    Engine engine;
    Gravity gravity;
    Space baseline, expected;
    bool enabled = false, owned = false, resynchronizing = false;
    bool statusAttention = false;
    std::string status = "Ready. Enable to test native playspace movement.";

    bool needsRestore() const { return owned || pending_.has_value(); }
    bool movementRequested() const { return movementRequested_; }
    bool startupEnablePending() const { return startupEnablePending_; }

    // Loading preferences only schedules activation. watch() reads and backs
    // up the current calibration before enabling; no saved offset is restored.
    void restoreMovementPreference(bool requested) {
        movementRequested_ = startupEnablePending_ = requested;
        if (requested)
            status = "Restoring saved movement setting; waiting for playspace.";
    }
    bool requestMovement(bool requested) {
        movementRequested_ = requested;
        startupEnablePending_ = false;
        if (requested)
            return enabled || enable();
        disable();
        return !needsRestore();
    }

    bool enable(bool waitForPlayspace = false) {
        Space actual;
        if (!access_.read(actual)) {
            if (waitForPlayspace) {
                status = "Waiting to restore movement: " + access_.readProblem();
                statusAttention = true;
                return false;
            }
            return pause(("Cannot enable: " + access_.readProblem()).c_str());
        }
        if (needsRestore() && !acceptKnown(actual))
            return false;
        if (!owned) {
            baseline = expected = actual;
            recentPoses_.clear();
            rememberPose(actual.pose);
            engine.reset();
        }
        if (!access_.saveBaseline(baseline))
            return pause("Cannot save original playspace. Movement stays paused.");
        enabled = true;
        startupEnablePending_ = false;
        resynchronizing = false;
        resyncCandidate_.reset();
        stableReads_ = 0;
        status = "Close the dashboard; hold your drag binding and move that controller.";
        statusAttention = false;
        engine.release();
        gravity.stop();
        return true;
    }
    void
    abandon(const char *reason = "Standing playspace changed. Paused; check floor, then enable.") {
        access_.cancelPreview();
        enabled = owned = false;
        startupEnablePending_ = false;
        resynchronizing = false;
        resyncCandidate_.reset();
        stableReads_ = 0;
        pending_.reset();
        recentPoses_.clear();
        engine.reset();
        gravity.stop();
        status = reason;
        statusAttention = true;
    }
    // dragReleased must come from a successful active input read outside the
    // dashboard; inactive/error/default-false input is not a physical release.
    bool watch(bool dragReleased = false) {
        if (startupEnablePending_)
            return enable(true);
        if (resynchronizing)
            return resynchronize(dragReleased);
        if (!enabled && !needsRestore())
            return true;
        return observe();
    }
    bool recenter(const Transform &rawToStanding) {
        if ((enabled || needsRestore()) && !rawToStanding.valid()) {
            abandon("Runtime standing origin invalid. Movement paused.");
            return false;
        }
        if (resynchronizing) {
            // A known working copy alone cannot prove that a live-origin
            // recenter has recovered. Require a known runtime notification too.
            previewMayRecover_ = knownRuntimeOrigin(rawToStanding.inverse());
            return false;
        }
        if (!enabled && !needsRestore())
            return true;
        // An event alone does not establish a change to the standing playspace.
        // In particular, seated resets need not affect our standing origin.
        if (!observe()) {
            if (resynchronizing)
                previewMayRecover_ = knownRuntimeOrigin(rawToStanding.inverse());
            return false;
        }
        // Readback of the working copy can lag a live recenter, so also check
        // the runtime transform. Some runtimes expose the live baseline here,
        // others include the preview. Both must match data we actually know.
        const Transform runtimeOrigin = rawToStanding.inverse();
        if (knownRuntimeOrigin(runtimeOrigin))
            return true;
        Space runtime = expected;
        runtime.pose = rawToStanding.inverse();
        access_.reportMismatch("runtime-origin", expected, runtime);
        beginResync("Runtime standing origin changed.");
        previewMayRecover_ = false;
        return false;
    }

  private:
    // apply/reset verify the current state before building this target. Keeping
    // those steps ordered avoids a stale target when an idle baseline refreshes.
    bool commit(const Space &target, WriteMode mode = WriteMode::Preview) {
        const Space previous = expected;
        // A missing readback can mean either the old state or our target is live.
        // Keep both candidates and reconcile them on the next successful read.
        pending_ = target;
        const bool written = access_.write(target, mode);
        Space actual;
        const bool readable = access_.read(actual);
        if (mode == WriteMode::Restore && written) {
            // Restore removes our preview; it does not submit the old baseline
            // as a new live calibration. Frame may now expose a different live
            // origin. Reacquire it without treating the reset as a failed drag.
            pending_.reset();
            owned = false;
            engine.reset();
            gravity.stop();
            if (!readable || !same(actual, target)) {
                beginResync("Offset reset. Refreshing the live playspace.");
                return false;
            }
            baseline = expected = actual;
            recentPoses_.clear();
            rememberPose(actual.pose);
            return true;
        }
        if (!readable)
            return pause("Write unconfirmed. Paused; reset after tracking returns.");
        if (same(actual, target)) {
            expected = actual;
            rememberPose(actual.pose);
            owned = true;
            pending_.reset();
            syncOffset();
            if (written)
                return true;
            return pause("Runtime reported a write failure. Paused; reset offset.");
        }
        if (same(actual, previous)) {
            pending_.reset();
            syncOffset();
            return pause("Playspace write rejected. Movement paused.");
        }
        access_.reportMismatch("write-readback", target, actual);
        // Do not roll back over a changed calibration or hide the preview in
        // mid-drag. Retain both possible submitted states and suspend writes,
        // just as for an unexpected state observed before the write.
        beginResync(changeReason(target, actual));
        return false;
    }

  public:
    void setDistanceLimit(bool enabled, double meters) {
        engine.distanceLimit.enabled = enabled;
        engine.distanceLimit.setMeters(meters);
        engine.release();
        gravity.stop();
    }
    bool apply() {
        const Vec requestedOffset = engine.offset;
        if (!observe())
            return false;
        if (!needsRestore() && !access_.saveBaseline(baseline))
            return pause("Cannot save original playspace. Movement stays paused.");
        Space target = baseline;
        target.pose = withOffset(baseline.pose, requestedOffset);
        return commit(target);
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
        engine.release();
        gravity.stop();
        if (resynchronizing) {
            if (needsRestore())
                discardForResync();
            return false;
        }
        if (!needsRestore()) {
            engine.reset();
            return true;
        }
        if (!observe()) {
            // An explicit reset authorizes discarding even a changed preview;
            // never write the stale baseline over a new runtime calibration.
            if (resynchronizing && needsRestore())
                discardForResync();
            return false;
        }
        if (!commit(baseline, WriteMode::Restore))
            return false;
        owned = false;
        engine.reset();
        status = "Offset reset.";
        statusAttention = false;
        return true;
    }
    void disable() {
        if (resynchronizing) {
            abandon("Paused.");
            statusAttention = false;
            return;
        }
        enabled = false;
        startupEnablePending_ = false;
        resynchronizing = false;
        resyncCandidate_.reset();
        stableReads_ = 0;
        if (reset()) {
            status = "Paused.";
            statusAttention = false;
        }
    }
};
} // namespace drag
