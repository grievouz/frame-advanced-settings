#include "playspace.h"
#include "fixtures/frame_floor_origin.h"
#include "fixtures/frame_idle_drift.h"
#include "fixtures/frame_universe_switch.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

static int checks = 0;
static void check(bool ok, const char *name) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", name);
        std::exit(1);
    }
}
struct Runtime : drag::SpaceAccess {
    drag::Space current;
    bool readable = true, backupOk = true;
    enum class Next {
        Success,
        Reject,
        Disconnect,
        RejectDisconnect,
        Normalize
    } next = Next::Success;
    int writes = 0, cancelled = 0;
    bool reexpressBounds = false, distortBounds = false, runtimeFloorBounds = false;
    bool differentLiveOrigin = false;
    bool previewActive = false;
    drag::WriteMode lastWriteMode = drag::WriteMode::Preview;
    drag::Space saved;
    std::optional<drag::Space> liveAfterCancel;
    std::vector<std::string> mismatchStages;
    Runtime() {
        current.universe = 123;
        current.bounds.push_back(
            {drag::Vec{-1, 0, -1}, drag::Vec{-1, 2, -1}, drag::Vec{1, 2, -1}, drag::Vec{1, 0, -1}});
    }
    bool read(drag::Space &result) override {
        if (!readable)
            return false;
        result = current;
        if (reexpressBounds) {
            const auto origin = runtimeFloorBounds
                                    ? drag::previewAtBaselineFloor(saved.pose, current.pose)
                                    : current.pose;
            for (auto &quad : result.bounds)
                for (auto &corner : quad)
                    corner = origin.inversePoint(saved.pose.point(corner));
            if (distortBounds)
                result.bounds.back()[3].x += .02;
            const drag::RuntimeBoundsOrigins origins{origin, origin};
            drag::normalizePreviewBounds(result, saved, runtimeFloorBounds ? &origins : nullptr);
        }
        return true;
    }
    bool write(const drag::Space &target, drag::WriteMode writeMode) override {
        ++writes;
        lastWriteMode = writeMode;
        auto mode = next;
        next = Next::Success;
        if (mode != Next::Reject && mode != Next::RejectDisconnect) {
            current = target;
            previewActive = writeMode == drag::WriteMode::Preview;
            // Frame can expose a different live origin when the working
            // preview is hidden, even though the visible reset is correct.
            if (!previewActive && differentLiveOrigin)
                current.pose.t.y -= 1;
        }
        if (mode == Next::Normalize)
            current.bounds[0][0].y = .05;
        if (mode == Next::Disconnect || mode == Next::RejectDisconnect)
            readable = false;
        return mode != Next::Reject && mode != Next::RejectDisconnect;
    }
    bool saveBaseline(const drag::Space &space) override {
        saved = space;
        return backupOk;
    }
    void cancelPreview() override {
        ++cancelled;
        previewActive = false;
        if (liveAfterCancel)
            current = *liveAfterCancel;
    }
    void reportMismatch(const char *stage, const drag::Space &, const drag::Space &) override {
        mismatchStages.emplace_back(stage);
    }
};
struct CapturedRuntime : Runtime {
    bool useCapture = false;
    drag::Space captured = fixture::frameFloorReadback();
    drag::RuntimeBoundsOrigins origins{fixture::frameFloorRawToStanding().inverse(),
                                       fixture::frameFloorRawToStanding().inverse()};
    bool read(drag::Space &result) override {
        if (!useCapture)
            return Runtime::read(result);
        result = captured;
        drag::normalizePreviewBounds(result, saved, &origins);
        return true;
    }
};
int main() {
    using namespace drag;
    Runtime runtime;
    const Space original = runtime.current;
    Session session(runtime);
    check(!session.enabled && runtime.writes == 0, "startup does not write");
    runtime.backupOk = false;
    check(!session.enable() && runtime.writes == 0, "backup failure blocks movement");
    runtime.backupOk = true;
    check(session.enable() && runtime.writes == 0, "enable captures without writing");
    session.engine.offset = {0, .2, 0};
    check(session.apply() && session.owned, "confirmed write owns state");
    check(length(runtime.current.bounds[0][0] - original.bounds[0][0]) < 1e-9,
          "drag does not modify saved collision geometry");
    check(std::abs(session.engine.offset.y - .2) < 1e-8, "confirmed offset reported");
    runtime.next = Runtime::Next::Reject;
    session.engine.offset.y = .3;
    check(!session.apply() && !session.enabled && std::abs(session.engine.offset.y - .2) < 1e-8,
          "rejected write preserves previous confirmed offset");
    runtime.readable = false;
    check(!session.watch() && session.needsRestore(), "temporary read loss retains reset target");
    runtime.readable = true;
    check(session.reset() && same(runtime.current, original), "reset after temporary loss");

    for (auto mode : {Runtime::Next::Disconnect, Runtime::Next::RejectDisconnect}) {
        session.enable();
        session.engine.offset.y = .15;
        runtime.next = mode;
        check(!session.apply() && session.needsRestore(), "unreadable write retained for recovery");
        runtime.readable = true;
        check(session.reset() && same(runtime.current, original),
              "reset reconciles ambiguous write");
    }

    session.enable();
    session.engine.offset.y = .2;
    runtime.next = Runtime::Next::Normalize;
    const int beforeBoundaryWrite = runtime.writes;
    check(!session.apply() && session.enabled && session.resynchronizing &&
              session.needsRestore() && runtime.writes == beforeBoundaryWrite + 1 &&
              runtime.cancelled == 0,
          "unexpected boundary after write suspends without a stale rollback or cancellation");
    runtime.liveAfterCancel = original;
    session.reset();
    session.watch();
    session.watch();
    check(session.watch() && session.enabled && !session.needsRestore(),
          "explicit reset discards uncertain write and reacquires the live baseline");
    runtime.liveAfterCancel.reset();

    session.enable();
    session.engine.offset.y = .1;
    session.apply();
    runtime.current.pose.t.x += .5;
    const int beforeReset = runtime.writes;
    const int beforeCancel = runtime.cancelled;
    check(!session.reset() && runtime.writes == beforeReset && !session.needsRestore(),
          "external calibration never overwritten by reset");
    check(runtime.cancelled == beforeCancel + 1, "external change hides our preview");
    session.enable();
    const Space recalibrated = runtime.current;
    session.engine.offset.y = .1;
    session.apply();
    session.disable();
    check(!session.enabled && !session.needsRestore() && same(runtime.current, recalibrated),
          "disable restores newly captured calibration");

    Runtime frame;
    frame.current.pose.r[0][0] = frame.current.pose.r[2][2] = 0;
    frame.current.pose.r[0][2] = 1;
    frame.current.pose.r[2][0] = -1;
    frame.current.pose.t = {3, 1.6, -2};
    const Space frameBase = frame.current;
    Session frameSession(frame);
    frameSession.enable();
    check(frameSession.recenter(frameBase.pose.inverse()) && frameSession.enabled &&
              frame.writes == 0 && frame.cancelled == 0,
          "unchanged seated/duplicate reset notification does not disable movement");
    frameSession.engine.offset.y = .1;
    frameSession.apply();
    const Transform firstPreview = frame.current.pose;
    frameSession.engine.sample(false, true, {0, 1, 0}, frameBase.pose, .01);
    frameSession.engine.sample(true, true, {0, 1, 0}, frameBase.pose, .01);
    const int writesBeforeEvent = frame.writes;
    check(frameSession.recenter(firstPreview.inverse()) && frameSession.enabled &&
              frameSession.engine.held() && frameSession.needsRestore() &&
              frame.writes == writesBeforeEvent && frame.cancelled == 0,
          "own preview reset notification preserves the held drag without extra writes");
    frameSession.engine.offset.y = .2;
    frameSession.apply();
    check(frameSession.recenter(firstPreview.inverse()) && frameSession.enabled,
          "delayed notification for an earlier confirmed preview is accepted");
    check(frameSession.recenter(frameBase.pose.inverse()) && frameSession.enabled,
          "runtime may expose the live baseline while the working preview is active");
    Transform outsideOrigin = frame.current.pose;
    outsideOrigin.t.x += .4;
    const int beforeLiveRecenter = frame.writes;
    check(!frameSession.recenter(outsideOrigin.inverse()) && frameSession.enabled &&
              frameSession.resynchronizing && frameSession.needsRestore() && frame.cancelled == 0 &&
              frame.writes == beforeLiveRecenter,
          "live recenter suspends writes without immediately discarding the held offset");
    for (int i = 0; i < 4; ++i)
        frameSession.watch();
    check(frameSession.resynchronizing && frame.cancelled == 0,
          "a known working copy does not hide an unknown live-origin recenter");
    frameSession.disable();
    frame.current = frameBase;
    frameSession.enable();
    frameSession.engine.offset.y = .1;
    frameSession.apply();
    frame.current.pose.t.z += .3;
    const int beforeWorkingRecenter = frame.writes;
    check(!frameSession.recenter(frame.current.pose.inverse()) && frameSession.enabled &&
              frameSession.resynchronizing && frame.writes == beforeWorkingRecenter,
          "working-origin change suspends movement for resynchronization");
    frameSession.disable();
    frame.current = frameBase;
    frameSession.enable();
    frameSession.engine.offset.y = .1;
    frameSession.apply();
    frame.current.bounds[0][0].z += .2;
    check(!frameSession.recenter(frameBase.pose.inverse()) && frameSession.enabled &&
              frameSession.resynchronizing,
          "boundary changes are not ignored even when the runtime origin is known");
    frameSession.disable();
    frame.current = frameBase;
    frameSession.enable();
    ++frame.current.universe;
    check(!frameSession.recenter(frameBase.pose.inverse()) && frameSession.enabled &&
              frameSession.resynchronizing,
          "universe changes are not ignored even when the runtime origin is known");
    frameSession.disable();
    frame.current = frameBase;
    frameSession.enable();
    Transform invalidOrigin;
    invalidOrigin.r[0][0] = 2;
    check(!frameSession.recenter(invalidOrigin) && !frameSession.enabled,
          "invalid runtime transform cannot validate a reset notification");
    frame.current = frameBase;
    frameSession.enable();
    frame.readable = false;
    check(!frameSession.recenter(frameBase.pose.inverse()) && frameSession.enabled &&
              frameSession.resynchronizing,
          "unreadable playspace on a recenter notification suspends motion and retries");

    // A runtime may return corners relative to the previewed standing origin.
    // Verify the coordinate change directly using an independent known result:
    // under this 90-degree baseline, moving raw X by +.3 means local Z -.3.
    Space translated = frameBase;
    translated.pose.t.x += .3;
    for (auto &quad : translated.bounds)
        for (auto &corner : quad)
            corner.z -= .3;
    const Space translatedRaw = translated;
    check(normalizePreviewBounds(translated, frameBase) == BoundsMatch::Reexpressed &&
              sameBounds(translated, frameBase) && near(translated.pose, translatedRaw.pose),
          "equivalent translated corners normalize without altering the reported origin");
    check(normalizePreviewBounds(translated, frameBase) == BoundsMatch::Unchanged,
          "unchanged boundary representation is preserved");
    for (int corner = 0; corner < 4; ++corner) {
        Space changedWall = translatedRaw;
        changedWall.bounds[0][corner].x += .01;
        const Space unmodified = changedWall;
        check(normalizePreviewBounds(changedWall, frameBase) == BoundsMatch::Different &&
                  same(changedWall, unmodified),
              "one changed corner cannot be hidden by coordinate normalization");
    }
    Space unrelated = translatedRaw;
    ++unrelated.universe;
    check(normalizePreviewBounds(unrelated, frameBase) == BoundsMatch::Different,
          "equivalent geometry cannot hide a universe change");
    unrelated = translatedRaw;
    unrelated.bounds.pop_back();
    check(normalizePreviewBounds(unrelated, frameBase) == BoundsMatch::Different,
          "a missing wall is not a coordinate-only change");
    unrelated = translatedRaw;
    unrelated.bounds[0][0].y = std::numeric_limits<double>::quiet_NaN();
    check(normalizePreviewBounds(unrelated, frameBase) == BoundsMatch::Different,
          "non-finite corners cannot be normalized");

    Runtime preview;
    preview.current = frameBase;
    Session previewSession(preview);
    previewSession.enable();
    preview.reexpressBounds = true;
    previewSession.engine.offset.y = .15;
    check(previewSession.apply() && previewSession.enabled && preview.cancelled == 0 &&
              std::abs(previewSession.engine.offset.y - .15) < 1e-8,
          "first drag accepts equivalent boundary coordinates instead of rolling back");
    check(previewSession.recenter(preview.current.pose.inverse()) && previewSession.enabled,
          "recenter following the equivalent boundary readback keeps movement enabled");
    previewSession.engine.offset = {.1, .3, -.2};
    check(previewSession.apply() && previewSession.watch() && previewSession.enabled,
          "subsequent XYZ drag and watch use the same baseline representation");
    check(previewSession.reset() && !previewSession.needsRestore() &&
              same(preview.current, frameBase),
          "reset after normalized readbacks restores the baseline");
    previewSession.engine.offset.y = .15;
    previewSession.apply();
    preview.current.pose.t.z += .3;
    check(!previewSession.watch() && previewSession.enabled && previewSession.resynchronizing,
          "same physical corners do not excuse an unsubmitted origin change");
    previewSession.disable();
    preview.current = frameBase;
    preview.reexpressBounds = false;
    previewSession.enable();
    preview.reexpressBounds = preview.distortBounds = true;
    previewSession.engine.offset.y = .15;
    check(!previewSession.apply() && previewSession.resynchronizing &&
              !preview.mismatchStages.empty(),
          "a real wall change suspends writes and records diagnostic data");
    check(!runtime.mismatchStages.empty() && runtime.mismatchStages.front() == "write-readback",
          "boundary write rejection records its cause before rollback and recenter");
    Runtime falling;
    Session fallSession(falling);
    fallSession.gravity.setEnabled(true);
    check(!fallSession.updateMotion(true, false, true, {}, .01) && falling.writes == 0,
          "gravity cannot move a disabled session");
    fallSession.enable();
    fallSession.engine.offset = {.2, 1, -.1};
    fallSession.apply();
    check(fallSession.updateMotion(true, false, true, {}, .01) && fallSession.engine.offset.y < 1 &&
              fallSession.engine.offset.x == .2 && fallSession.engine.offset.z == -.1,
          "gravity writes through the session and preserves horizontal offset");
    const auto beforePause = falling.current;
    const int writesBeforePause = falling.writes;
    fallSession.updateMotion(false, false, true, {}, .05);
    check(same(falling.current, beforePause) && falling.writes == writesBeforePause &&
              fallSession.gravity.speed() == 0,
          "dashboard or inactive input freezes falling and clears velocity");
    fallSession.updateMotion(true, false, false, {}, .01);
    check(falling.writes == writesBeforePause && fallSession.gravity.speed() == 0,
          "lost tracking freezes gravity");
    fallSession.updateMotion(true, false, true, {}, .5);
    check(falling.writes == writesBeforePause, "frame stall cannot produce a fall jump");
    fallSession.updateMotion(true, false, true, {0, 1, 0}, .01);
    const double beforeGrab = fallSession.engine.offset.y;
    fallSession.updateMotion(true, true, true, {0, 1, 0}, .01);
    check(fallSession.engine.offset.y == beforeGrab && fallSession.gravity.speed() == 0,
          "grabbing pauses gravity immediately");
    fallSession.updateMotion(true, true, true, {0, .9, 0}, .01);
    check(fallSession.engine.offset.y > beforeGrab, "drag can raise user with gravity enabled");
    fallSession.updateMotion(true, false, true, {}, .01);
    check(fallSession.reset() && fallSession.enabled && fallSession.gravity.enabled() &&
              length(fallSession.engine.offset) == 0 && fallSession.gravity.speed() == 0,
          "manual reset clears fall state while keeping movement and gravity selected");
    const int landedWrites = falling.writes;
    check(!fallSession.updateMotion(true, false, true, {}, .01) && falling.writes == landedWrites,
          "reset floor remains still under gravity");
    fallSession.engine.offset.y = .5;
    fallSession.apply();
    falling.next = Runtime::Next::Reject;
    check(!fallSession.updateMotion(true, false, true, {}, .01) && !fallSession.enabled &&
              fallSession.gravity.speed() == 0 && std::abs(fallSession.engine.offset.y - .5) < 1e-8,
          "gravity write rejection pauses and preserves last confirmed offset");
    fallSession.enable();
    falling.current.pose.t.x += .4;
    const int externalWrites = falling.writes;
    check(!fallSession.updateMotion(true, false, true, {}, .01) && fallSession.enabled &&
              fallSession.resynchronizing && falling.writes == externalWrites &&
              fallSession.gravity.speed() == 0,
          "gravity cannot overwrite an outside recenter");

    Runtime landing;
    landing.differentLiveOrigin = landing.reexpressBounds = true;
    Session jump(landing);
    jump.enable();
    jump.gravity.setEnabled(true);
    jump.updateMotion(true, false, true, {}, .01);
    jump.updateMotion(true, true, true, {0, 1, 0}, .01);
    for (int i = 1; i <= 10; ++i)
        jump.updateMotion(true, true, true, {.005 * i, 1 - .01 * i, .003 * i}, .01);
    const Vec releaseOffset = jump.engine.offset;
    check(jump.updateMotion(true, false, true, {.05, .9, .03}, .01) &&
              jump.engine.offset.y > releaseOffset.y && jump.gravity.velocity().y > 0 &&
              jump.engine.offset.x == 0 && jump.engine.offset.z == 0,
          "releasing a moving height-only drag launches upward through the runtime");
    for (int i = 0; i < 100; ++i)
        jump.updateMotion(true, false, true, {}, .01);
    check(jump.engine.offset.y == 0 && jump.enabled && landing.previewActive &&
              landing.lastWriteMode == WriteMode::Preview && landing.cancelled == 0 &&
              landing.mismatchStages.empty(),
          "landing at the baseline stays in preview instead of exposing a different live origin");
    check(jump.watch() && jump.recenter(landing.current.pose.inverse()) && jump.enabled,
          "landing's own recenter notification does not disable movement");
    const int restingWrites = landing.writes;
    jump.updateMotion(true, false, true, {}, .01);
    check(landing.writes == restingWrites && jump.gravity.speed() == 0,
          "landed preview stays still without redundant writes");
    jump.updateMotion(true, true, true, {0, 1, 0}, .01);
    for (int i = 1; i <= 5; ++i)
        jump.updateMotion(true, true, true, {0, 1 - .01 * i, 0}, .01);
    for (int i = 0; i < 10; ++i)
        jump.updateMotion(true, true, true, {0, .95, 0}, .01);
    const double stillReleaseHeight = jump.engine.offset.y;
    jump.updateMotion(true, false, true, {0, .95, 0}, .01);
    check(jump.engine.offset.y < stillReleaseHeight && jump.gravity.velocity().y < 0,
          "stationary release falls from rest instead of reusing an earlier throw");
    for (int i = 0; i < 100; ++i)
        jump.updateMotion(true, false, true, {}, .01);
    check(jump.engine.offset.y == 0 && jump.enabled && landing.cancelled == 0,
          "ordinary no-fling landing also keeps movement enabled");
    // Hiding the preview can expose a different valid live origin. Reset must
    // acquire that reference without switching movement off or writing it back.
    check(!jump.reset() && landing.lastWriteMode == WriteMode::Restore && !landing.previewActive &&
              jump.enabled && jump.resynchronizing && !jump.needsRestore() &&
              jump.gravity.enabled() && jump.gravity.speed() == 0 &&
              length(jump.engine.offset) == 0 && landing.mismatchStages.empty(),
          "reset to a different live origin keeps movement enabled while synchronizing");
    const int resetWrites = landing.writes;
    const Space liveResetReference = landing.current;
    jump.watch();
    check(!jump.reset() && jump.enabled && jump.resynchronizing,
          "repeated reset during acquisition preserves the enable preference and progress");
    jump.watch();
    check(jump.watch() && jump.enabled && !jump.resynchronizing &&
              same(jump.baseline, liveResetReference) && landing.writes == resetWrites,
          "reset accepts three stable live reads without restoring the stale baseline");
    jump.updateMotion(true, true, true, {0, -.1, 0}, .01);
    check(!jump.engine.held() && landing.writes == resetWrites,
          "reset still requires release before another drag");
    jump.updateMotion(true, false, true, {}, .01);
    jump.updateMotion(true, true, true, {}, .01);
    jump.updateMotion(true, true, true, {0, -.05, 0}, .01);
    check(jump.enabled && landing.writes == resetWrites + 1 &&
              std::abs(jump.engine.offset.y - .05) < 1e-8,
          "dragging resumes after reset without toggling enable");
    landing.next = Runtime::Next::Disconnect;
    check(!jump.reset() && jump.enabled && jump.resynchronizing && !jump.needsRestore(),
          "temporary read loss after successful preview cleanup does not disable movement");
    jump.watch();
    check(jump.enabled && jump.resynchronizing,
          "reset waits for unavailable live data with movement selected");
    landing.readable = true;
    jump.watch();
    jump.watch();
    check(jump.watch() && jump.enabled && !jump.resynchronizing,
          "reset recovery completes when live data returns");
    jump.engine.offset.y = .1;
    jump.apply();
    jump.disable();
    check(!jump.enabled && !jump.resynchronizing && !jump.needsRestore() && !landing.previewActive,
          "Disable still stays off when cleanup exposes a different live origin");

    Runtime thrown;
    Session toss(thrown);
    toss.enable();
    toss.engine.xyz = true;
    toss.gravity.setEnabled(true);
    toss.updateMotion(true, false, true, {}, .01);
    toss.updateMotion(true, true, true, {}, .01);
    for (int i = 1; i <= 10; ++i)
        toss.updateMotion(true, true, true, {-.005 * i, -.01 * i, .002 * i}, .01);
    toss.updateMotion(true, false, true, {-.05, -.1, .02}, .01);
    check(toss.gravity.velocity().x > 0 && toss.gravity.velocity().y > 0 &&
              toss.gravity.velocity().z < 0,
          "XYZ mode carries momentum on all three axes");
    const auto beforeGrabThrow = toss.engine.offset;
    toss.updateMotion(true, true, true, {}, .01);
    check(length(toss.engine.offset - beforeGrabThrow) < 1e-9 && toss.gravity.speed() == 0,
          "grabbing during a throw immediately stops all flight motion");
    for (int i = 1; i <= 5; ++i)
        toss.updateMotion(true, true, true, {0, -.01 * i, 0}, .01);
    const auto beforeDashboardThrow = thrown.current;
    toss.updateMotion(false, true, true, {0, -.05, 0}, .01);
    toss.updateMotion(true, false, true, {0, -.05, 0}, .01);
    check(toss.gravity.velocity().x == 0 && toss.gravity.velocity().z == 0 &&
              toss.gravity.velocity().y < 0 &&
              thrown.current.pose.t.y > beforeDashboardThrow.pose.t.y,
          "dashboard interruption discards the held throw estimate and resumes a plain fall");
    toss.reset();
    check(toss.enabled && !thrown.previewActive && thrown.lastWriteMode == WriteMode::Restore &&
              toss.gravity.speed() == 0 && length(toss.engine.offset) == 0,
          "explicit reset after a throw requests cleanup and clears momentum");
    const Space capturedBaseline = fixture::frameFloorBaseline();
    const Space capturedActual = fixture::frameFloorReadback();
    const Transform capturedRuntime = fixture::frameFloorRawToStanding().inverse();
    const RuntimeBoundsOrigins capturedOrigins{capturedRuntime, capturedRuntime};
    Space captured = capturedActual;
    check(normalizePreviewBounds(captured, capturedBaseline) == BoundsMatch::Different,
          "real Frame capture reproduces the old boundary rejection using only the working pose");
    double maxPhysicalError = 0;
    for (size_t i = 0; i < capturedActual.bounds.size(); ++i)
        for (size_t j = 0; j < 4; ++j)
            maxPhysicalError =
                std::max(maxPhysicalError,
                         length(capturedRuntime.point(capturedActual.bounds[i][j]) -
                                capturedBaseline.pose.point(capturedBaseline.bounds[i][j])));
    check(maxPhysicalError < .00003 && capturedActual.bounds.size() == 20,
          "all 80 captured corners physically match within 0.03 mm under the reported runtime "
          "origin");
    check(normalizePreviewBounds(captured, capturedBaseline, &capturedOrigins) ==
                  BoundsMatch::RuntimeReexpressed &&
              sameBounds(captured, capturedBaseline) && near(captured.pose, capturedActual.pose),
          "captured runtime-floor representation normalizes without changing the movement pose");
    for (int axis = 0; axis < 3; ++axis) {
        Space changed = capturedActual;
        auto &corner = changed.bounds[7][2];
        if (axis == 0)
            corner.x += .01;
        else if (axis == 1)
            corner.y += .01;
        else
            corner.z += .01;
        const auto originalChanged = changed;
        check(normalizePreviewBounds(changed, capturedBaseline, &capturedOrigins) ==
                      BoundsMatch::Different &&
                  same(changed, originalChanged),
              "runtime-floor normalization still rejects a changed corner on any axis");
    }
    RuntimeBoundsOrigins inconsistent = capturedOrigins;
    inconsistent.after.t.y += .01;
    captured = capturedActual;
    check(normalizePreviewBounds(captured, capturedBaseline, &inconsistent) ==
              BoundsMatch::Different,
          "a changing runtime origin cannot validate a mixed boundary snapshot");
    RuntimeBoundsOrigins invalid = capturedOrigins;
    invalid.before.t.x = std::numeric_limits<double>::quiet_NaN();
    check(normalizePreviewBounds(captured, capturedBaseline, &invalid) == BoundsMatch::Different,
          "invalid runtime origin cannot validate the floor representation");
    ++captured.universe;
    check(normalizePreviewBounds(captured, capturedBaseline, &capturedOrigins) ==
              BoundsMatch::Different,
          "runtime origin cannot hide a changed tracking universe");
    captured = capturedActual;
    captured.bounds.pop_back();
    check(normalizePreviewBounds(captured, capturedBaseline, &capturedOrigins) ==
              BoundsMatch::Different,
          "runtime-floor representation still rejects a removed wall");
    captured = capturedActual;
    captured.bounds[0][0].y = std::numeric_limits<double>::quiet_NaN();
    check(normalizePreviewBounds(captured, capturedBaseline, &capturedOrigins) ==
              BoundsMatch::Different,
          "runtime-floor representation still rejects a non-finite corner");
    Transform unknownOrigin = capturedRuntime;
    unknownOrigin.t.y += .03;
    captured = capturedActual;
    for (size_t i = 0; i < captured.bounds.size(); ++i)
        for (size_t j = 0; j < 4; ++j)
            captured.bounds[i][j] = unknownOrigin.inversePoint(
                capturedBaseline.pose.point(capturedBaseline.bounds[i][j]));
    const RuntimeBoundsOrigins unknownOrigins{unknownOrigin, unknownOrigin};
    check(normalizePreviewBounds(captured, capturedBaseline, &unknownOrigins) ==
              BoundsMatch::Different,
          "physically matching corners cannot excuse an unknown floor height");

    CapturedRuntime recorded;
    recorded.current = capturedBaseline;
    Session recordedSession(recorded);
    recordedSession.enable();
    recordedSession.engine.offset =
        capturedBaseline.pose.unrotate(capturedBaseline.pose.t - capturedActual.pose.t);
    recordedSession.apply();
    recorded.useCapture = true;
    const int writesBeforeCapture = recorded.writes;
    check(recordedSession.watch() && recordedSession.enabled && recordedSession.needsRestore() &&
              recorded.writes == writesBeforeCapture && recorded.cancelled == 0 &&
              recorded.mismatchStages.empty(),
          "exact user capture no longer disables movement during an observed-state check");
    check(recordedSession.recenter(fixture::frameFloorRawToStanding()) && recordedSession.enabled &&
              recorded.writes == writesBeforeCapture,
          "runtime-floor recenter notification for that capture preserves movement too");
    auto badRecenter = fixture::frameFloorRawToStanding();
    badRecenter.t.y += .01;
    check(!recordedSession.recenter(badRecenter) && recordedSession.enabled &&
              recordedSession.resynchronizing && recorded.writes == writesBeforeCapture,
          "a genuine runtime floor change suspends movement without overwriting calibration");

    Runtime alternating;
    alternating.current = capturedBaseline;
    Session alternatingSession(alternating);
    alternatingSession.enable();
    alternating.reexpressBounds = true;
    alternatingSession.engine.offset = {.2, .5, -.1};
    alternatingSession.apply();
    alternatingSession.gravity.setEnabled(true);
    alternatingSession.gravity.launch({.3, 1, -.2});
    bool stayedEnabled = true;
    for (int i = 0; i < 150; ++i) {
        alternating.runtimeFloorBounds = i % 2 == 0;
        alternatingSession.updateMotion(true, false, true, {}, .01);
        alternatingSession.watch();
        const auto runtimePose =
            alternating.runtimeFloorBounds
                ? previewAtBaselineFloor(alternatingSession.baseline.pose, alternating.current.pose)
                : alternating.current.pose;
        alternatingSession.recenter(runtimePose.inverse());
        stayedEnabled = stayedEnabled && alternatingSession.enabled;
    }
    check(stayedEnabled && std::abs(alternatingSession.engine.offset.y) < 1e-8 &&
              alternatingSession.gravity.speed() == 0 && alternating.cancelled == 0,
          "alternating working/runtime-floor representations preserve flight and landing");
    alternating.current.bounds[0][0].y += .03;
    check(!alternatingSession.watch() && alternatingSession.enabled &&
              alternatingSession.resynchronizing,
          "a real boundary height edit after flight is still detected");
    const Space idleBefore = fixture::frameIdleSpace(false);
    const Space idleAfter = fixture::frameIdleSpace(true);
    check(!near(idleBefore.pose, idleAfter.pose) && sameBounds(idleBefore, idleAfter) &&
              std::abs(idleAfter.pose.t.y - idleBefore.pose.t.y - .000540614) < 1e-10,
          "captured 0.54 mm idle floor refinement reproduces the old origin rejection");
    Runtime idle;
    idle.current = idleBefore;
    Session idleSession(idle);
    idleSession.enable();
    idle.current = idleAfter;
    check(idleSession.watch() && idleSession.enabled && !idleSession.resynchronizing &&
              !idleSession.needsRestore() && same(idleSession.baseline, idleAfter) &&
              idle.writes == 0 && idle.cancelled == 0 && idle.mismatchStages.empty(),
          "exact idle drift capture updates the reference without toggling movement or writing");
    idleSession.engine.offset.y = .1;
    idleSession.apply();
    check(
        same(idle.saved, idleAfter) &&
            near(idle.current.pose, withOffset(idleAfter.pose, {0, .1, 0})),
        "first movement backs up and uses the refreshed floor rather than the stale enable floor");
    check(idleSession.reset() && same(idle.current, idleAfter) && idleSession.enabled,
          "reset returns to the refreshed floor");
    for (int i = 0; i < 20; ++i) {
        idle.current.pose.t.y += .0006;
        idleSession.watch();
    }
    check(idleSession.enabled && !idleSession.resynchronizing &&
              idleSession.baseline.pose.t.y == idle.current.pose.t.y,
          "repeated idle floor refinements do not accumulate against an obsolete reference");

    Runtime movingReference;
    movingReference.current = idleBefore;
    Session recovering(movingReference);
    recovering.enable();
    recovering.gravity.setEnabled(true);
    recovering.engine.xyz = true;
    recovering.engine.gain = 1.5;
    recovering.engine.offset = {.1, .3, -.1};
    recovering.apply();
    recovering.gravity.launch({.2, 1, 0});
    Space newReference = idleAfter;
    ++newReference.universe;
    movingReference.liveAfterCancel = newReference;
    ++movingReference.current.universe;
    const int beforeRecoveryWrites = movingReference.writes;
    check(!recovering.watch() && recovering.enabled && recovering.resynchronizing &&
              recovering.needsRestore() && movingReference.previewActive &&
              recovering.gravity.speed() == 0 && length(recovering.engine.offset) > .3 &&
              movingReference.writes == beforeRecoveryWrites,
          "universe change suspends momentum but retains the preview and enabled setting");
    for (int i = 0; i < 2; ++i) {
        recovering.updateMotion(true, true, true, {0, 1, 0}, .01);
        check(!recovering.watch() && recovering.resynchronizing &&
                  movingReference.writes == beforeRecoveryWrites,
              "no movement or stale restoration writes while the new reference settles");
    }
    check(!recovering.watch() && recovering.needsRestore(),
          "stable changed universe alone cannot discard a held offset");
    check(!recovering.watch(true) && !recovering.needsRestore() && !movingReference.previewActive &&
              length(recovering.engine.offset) == 0,
          "confirmed release discards once before acquiring the new live reference");
    recovering.watch();
    recovering.watch();
    check(recovering.watch() && recovering.enabled && !recovering.resynchronizing &&
              same(recovering.baseline, newReference) &&
              same(movingReference.saved, newReference) && recovering.gravity.enabled() &&
              recovering.engine.xyz && recovering.engine.gain == 1.5,
          "three consistent reads acquire the new reference and preserve movement preferences");
    recovering.updateMotion(true, true, true, {0, .8, 0}, .01);
    check(movingReference.writes == beforeRecoveryWrites && !recovering.engine.held(),
          "holding drag through recalibration cannot restart motion automatically");
    recovering.updateMotion(true, false, true, {0, 1, 0}, .01);
    recovering.updateMotion(true, true, true, {0, 1, 0}, .01);
    recovering.updateMotion(true, true, true, {0, .95, 0}, .01);
    check(recovering.enabled && movingReference.writes == beforeRecoveryWrites + 1 &&
              std::abs(recovering.engine.offset.y - .075) < 1e-8,
          "release and regrab starts fresh movement relative to the new calibration");
    check(recovering.reset() && same(movingReference.current, newReference),
          "reset after resynchronization restores the new calibration only");
    recovering.engine.offset.y = .2;
    recovering.apply();
    movingReference.current.pose.t.y += .02;
    recovering.watch();
    const int unstableWrites = movingReference.writes;
    for (int i = 0; i < 8; ++i) {
        movingReference.current.pose.t.y += .01;
        recovering.watch();
    }
    check(recovering.enabled && recovering.resynchronizing &&
              movingReference.writes == unstableWrites,
          "unstable calibration keeps movement suspended without turning the setting off");
    movingReference.readable = false;
    recovering.watch();
    check(recovering.enabled && recovering.resynchronizing &&
              movingReference.writes == unstableWrites,
          "temporary read failure while synchronizing waits without writing");
    movingReference.readable = true;
    recovering.watch();
    recovering.watch();
    check(recovering.resynchronizing, "a read failure restarts the stability check");
    recovering.watch(true);
    check(recovering.resynchronizing && !recovering.needsRestore(),
          "stable changed state after read loss still needs release before preview cleanup");
    recovering.watch();
    recovering.watch();
    recovering.watch();
    check(!recovering.resynchronizing && recovering.enabled,
          "valid stable tracking resumes after the interrupted acquisition");
    recovering.engine.offset.y = .1;
    recovering.apply();
    ++movingReference.current.universe;
    recovering.watch();
    recovering.disable();
    recovering.watch();
    check(!recovering.enabled && !recovering.resynchronizing,
          "user disabling movement cancels pending automatic resynchronization");
    recovering.enable();
    recovering.engine.offset.y = .1;
    recovering.apply();
    ++movingReference.current.universe;
    recovering.watch();
    movingReference.backupOk = false;
    recovering.watch(true);
    recovering.watch(true);
    recovering.watch(true);
    recovering.watch();
    recovering.watch();
    recovering.watch();
    check(!recovering.enabled && !recovering.resynchronizing,
          "failed backup of the new reference still blocks movement");

    // Replay the actual room-to-square transition while holding a 2x XYZ drag.
    Runtime switching;
    switching.current = fixture::frameSwitchBaseline();
    Session heldRecovery(switching);
    heldRecovery.requestMovement(true);
    heldRecovery.engine.xyz = true;
    heldRecovery.engine.gain = 2;
    heldRecovery.engine.distanceLimit.enabled = false;
    const auto switchExpected = fixture::frameSwitchExpected();
    heldRecovery.engine.offset =
        switching.current.pose.unrotate(switching.current.pose.t - switchExpected.pose.t);
    heldRecovery.apply();
    const Vec heldOffset = heldRecovery.engine.offset;
    const Space confirmedPreview = switching.current;
    const int heldWrites = switching.writes;
    const Space oldBackup = switching.saved;
    auto switchActual = fixture::frameSwitchActual();
    const RuntimeBoundsOrigins switchOrigins{fixture::frameSwitchRuntimeOrigin(),
                                             fixture::frameSwitchRuntimeOrigin()};
    check(normalizePreviewBounds(switchActual, oldBackup, &switchOrigins) == BoundsMatch::Different,
          "captured four-wall universe is never normalized into the twenty-wall room");
    switching.current = switchActual;
    heldRecovery.recenter(fixture::frameSwitchRuntimeOrigin().inverse());
    for (int i = 0; i < 20; ++i) {
        heldRecovery.watch(); // Held, inactive, or unreadable input cannot prove release.
        heldRecovery.updateMotion(true, true, true, {0, 1 - .01 * i, 0}, .01);
    }
    check(heldRecovery.enabled && heldRecovery.movementRequested() &&
              heldRecovery.resynchronizing && heldRecovery.needsRestore() &&
              length(heldRecovery.engine.offset - heldOffset) < 1e-8 && switching.cancelled == 0 &&
              switching.writes == heldWrites && same(switching.saved, oldBackup),
          "captured universe switch while held causes no cleanup, writes, or offset loss");
    switching.readable = false; // Runtime next reported calibration 201 in the journal.
    heldRecovery.watch(true);
    check(heldRecovery.resynchronizing && heldRecovery.needsRestore() && switching.cancelled == 0,
          "unavailable calibration cannot be accepted as a new reset reference");
    switching.readable = true;
    switching.current = confirmedPreview; // Synthetic recovery: the old room returns.
    heldRecovery.recenter(confirmedPreview.pose.inverse());
    heldRecovery.watch();
    heldRecovery.watch();
    check(heldRecovery.watch() && !heldRecovery.resynchronizing &&
              length(heldRecovery.engine.offset - heldOffset) < 1e-8 && switching.cancelled == 0 &&
              switching.writes == heldWrites && same(switching.saved, oldBackup),
          "returning confirmed preview retains the original reset target and displacement");
    heldRecovery.updateMotion(true, true, true, {0, .5, 0}, .01);
    check(!heldRecovery.engine.held() && switching.writes == heldWrites,
          "returning tracking cannot reuse the old held controller anchor");
    heldRecovery.updateMotion(true, false, true, {0, 1, 0}, .01);
    heldRecovery.updateMotion(true, true, true, {0, 1, 0}, .01);
    heldRecovery.updateMotion(true, true, true, {0, .98, 0}, .01);
    check(heldRecovery.engine.held() &&
              std::abs(heldRecovery.engine.offset.y - heldOffset.y - .04) < 1e-8,
          "fresh grab after recovery continues at 2x from the preserved position");
    switching.current = switchActual;
    Space syntheticLive = switchActual;
    syntheticLive.pose.t = {.1, -1.6, .2}; // Post-hide values are deliberately simulated.
    switching.liveAfterCancel = syntheticLive;
    heldRecovery.watch();
    heldRecovery.watch(true);
    heldRecovery.watch(true);
    heldRecovery.watch(true);
    const int recoveryWrites = switching.writes;
    check(switching.cancelled == 1 && !heldRecovery.needsRestore() &&
              heldRecovery.resynchronizing && same(switching.saved, oldBackup),
          "persistent captured change discards preview exactly once after a valid release");
    heldRecovery.watch();
    heldRecovery.watch();
    heldRecovery.watch();
    check(!heldRecovery.resynchronizing && heldRecovery.enabled &&
              same(heldRecovery.baseline, syntheticLive) && same(switching.saved, syntheticLive) &&
              switching.cancelled == 1 && switching.writes == recoveryWrites,
          "new reset reference comes from stable post-hide reads without restoring stale geometry");

    // Explicit Reset and Off must remain available during a suspended drag.
    for (const bool turnOff : {false, true}) {
        heldRecovery.enable();
        heldRecovery.engine.offset.y = .1;
        heldRecovery.apply();
        ++switching.current.universe;
        heldRecovery.watch();
        const int cancels = switching.cancelled;
        if (turnOff)
            heldRecovery.requestMovement(false);
        else
            heldRecovery.reset();
        check(!heldRecovery.needsRestore() && switching.cancelled == cancels + 1 &&
                  heldRecovery.enabled == !turnOff && heldRecovery.movementRequested() == !turnOff,
              "explicit reset/off cleans suspended preview without changing the wrong preference");
    }
    Runtime adjustable;
    Session limited(adjustable);
    limited.enable();
    limited.gravity.setEnabled(true);
    limited.setDistanceLimit(true, 5);
    limited.engine.offset.y = 4.99;
    limited.apply();
    limited.gravity.launch({0, 3, 0});
    limited.updateMotion(true, false, true, {}, .1);
    check(limited.enabled && std::abs(limited.engine.offset.y - 5) < 1e-8 &&
              limited.gravity.speed() == 0,
          "session gravity uses the same selected distance limit as dragging");
    limited.gravity.launch({0, 2, 0});
    const int beforeLimitChange = adjustable.writes;
    limited.setDistanceLimit(true, 1);
    check(limited.enabled && limited.engine.offset.y == 5 && limited.gravity.speed() == 0 &&
              !limited.engine.held() && adjustable.writes == beforeLimitChange,
          "changing the limit clears momentum and held drag without writes or teleportation");
    limited.updateMotion(true, false, true, {}, .1);
    check(limited.engine.offset.y < 5 && limited.engine.offset.y > 4.9 && limited.enabled,
          "falling returns gradually when outside a newly lowered cap");
    limited.setDistanceLimit(false, 1);
    limited.gravity.launch({0, 3, 0});
    const double unlimitedY = limited.engine.offset.y;
    limited.updateMotion(true, false, true, {}, .1);
    check(limited.engine.offset.y > unlimitedY && limited.enabled,
          "session Off permits outward flight beyond the configured distance");
    limited.reset();
    check(limited.enabled && !limited.engine.distanceLimit.enabled &&
              limited.engine.distanceLimit.meters() == 1,
          "reset preserves the chosen distance limit preference");
    std::printf("%d playspace checks passed\n", checks);
}
