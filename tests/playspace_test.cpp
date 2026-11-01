#include "playspace.h"
#include "fixtures/frame_universe_switch.h"
#include "fixtures/frame_idle_drift.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace drag;
static int checks = 0;
static void check(bool ok, const char *name) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", name);
        std::exit(1);
    }
}
struct Runtime : SpaceAccess {
    Space saved, working, live;
    double time = 100, restoreAt = -1, writeAt = -1, restoreDelay = .15, writeDelay = 0;
    uint64_t id = 123;
    bool readable = true, tracked = true, backupOk = true, writeOk = true;
    bool liveReadable = true, repairOk = true;
    int writes = 0, hides = 0, backups = 0, repairs = 0, reports = 0;
    std::optional<Space> queued;
    std::vector<std::string> messages;
    Runtime() {
        saved.universe = id;
        saved.pose.t = {1, -1.6, 2};
        saved.seated = Transform{};
        saved.seated->t = {1, -1, 2};
        saved.bounds.push_back({Vec{-1, 0, -1}, Vec{-1, 2, -1}, Vec{1, 2, -1}, Vec{1, 0, -1}});
        working = live = saved;
    }
    void flush() {
        if (restoreAt >= 0 && time >= restoreAt) {
            working = live = saved;
            restoreAt = -1;
        }
        if (queued && time >= writeAt) {
            working = live = *queued;
            queued.reset();
        }
    }
    double seconds() const override { return time; }
    uint64_t currentUniverse() override { return id; }
    bool headTracked() override { return tracked; }
    bool read(Space &s) override {
        flush();
        s = working;
        return readable;
    }
    bool liveStanding(Transform &pose) override {
        flush();
        pose = live.pose;
        return tracked && liveReadable;
    }
    bool write(const Space &s) override {
        ++writes;
        if (!writeOk)
            return false;
        if (writeDelay > 0) {
            queued = s;
            writeAt = time + writeDelay;
        } else
            working = live = s;
        return true;
    }
    void cancelPreview() override {
        ++hides;
        queued.reset();
        restoreAt = time + restoreDelay;
    }
    bool saveBaseline(const Space &) override {
        ++backups;
        return backupOk;
    }
    bool reanchor(const Space &s) override {
        ++repairs;
        check(same(s, saved), "floor repair receives saved calibration, never a dragged preview");
        return repairOk;
    }
    void reportMismatch(const char *, const Space &, const Space &) override { ++reports; }
    void recoveryLog(const std::string &s) override { messages.push_back(s); }
    void tick(Session &s, double dt = .1) {
        time += dt;
        s.watch();
    }
    void settle(Session &s, int ticks = 30) {
        for (int i = 0; i < ticks; ++i)
            tick(s);
    }
};
static void move(Session &s, Vec offset) {
    s.engine.offset = offset;
    check(s.apply(), "preview submitted");
}
int main() {
    {
        Runtime r;
        r.saved.pose.r[0][0] = r.saved.pose.r[2][2] = 0;
        r.saved.pose.r[0][2] = 1;
        r.saved.pose.r[2][0] = -1;
        r.working = r.live = r.saved;
        Session s(r);
        s.enable();
        s.engine.gain = 2;
        const Vec rawHead = r.saved.pose.point({.2, 1.7, -.1});
        const Vec viewpointBefore = r.saved.pose.inversePoint(rawHead);
        s.updateMotion(true, false, true, {0, 1.2, 0}, .01);
        s.updateMotion(true, true, true, {0, 1.2, 0}, .01);
        s.updateMotion(true, true, true, {0, 1.1, 0}, .01);
        const Vec viewpointAfter = r.working.pose.inversePoint(rawHead);
        check(length(viewpointAfter - viewpointBefore - Vec{0, .2, 0}) < 1e-8,
              "downward hand drag raises the viewpoint with a rotated standing reference");
        check(sameOrigin(*r.working.seated, *r.saved.seated),
              "lifting the viewpoint leaves the room overlay reference unchanged");
        const Vec seatedBoundaryBefore = r.saved.seated->inversePoint({1, 0, 2});
        const Vec seatedBoundaryAfter = r.working.seated->inversePoint({1, 0, 2});
        check(length(seatedBoundaryAfter - seatedBoundaryBefore) < 1e-8,
              "physical room points do not move in the seated reference during a lift");
        s.event(SpaceEvent::DataChanged);
        r.settle(s);
        check(sameOrigin(*r.working.seated, *r.saved.seated) &&
                  length(r.working.pose.inversePoint(rawHead) - viewpointAfter) < 1e-8,
              "recovery reapplies standing movement without translating the room reference");
        s.gravity.setEnabled(true);
        for (int i = 0; i < 100; ++i) {
            r.time += .01;
            s.updateMotion(true, false, true, {0, 1.1, 0}, .01);
            check(sameOrigin(*r.working.seated, *r.saved.seated),
                  "gravity also leaves the seated room reference unchanged");
        }
        check(s.engine.offset.y == 0 &&
                  length(r.working.pose.inversePoint(rawHead) - viewpointBefore) < 1e-8,
              "gravity returns only the standing viewpoint to its original height");
    }
    {
        Runtime r;
        Session s(r);
        r.backupOk = false;
        check(!s.enable() && !s.enabled && r.writes == 0, "backup required before enabling");
        r.backupOk = true;
        check(s.enable() && r.writes == 0, "enable does not move the playspace");
        move(s, {.2, .4, -.1});
        check(sameOrigin(*r.working.seated, *r.saved.seated),
              "drag moves the standing origin without moving the seated room reference");
        r.writeDelay = .35;
        move(s, {.3, .5, -.2});
        r.tick(s);
        check(s.enabled && !s.resynchronizing, "delayed working-copy readback is not rejection");
        r.settle(s, 5);
        check(same(r.working, s.expected) && s.enabled, "delayed submission settles normally");
        s.reset();
        check(s.enabled && s.resynchronizing, "manual reset leaves module enabled");
        r.settle(s);
        check(!s.resynchronizing && !s.owned && same(r.working, r.saved) &&
                  length(s.engine.offset) == 0,
              "reset restores both origins and clears movement");
    }
    {
        Runtime r;
        Session s(r);
        s.enable();
        move(s, {0, .4, 0});
        r.restoreDelay = 1.5;
        s.reset();
        r.settle(s, 10);
        check(s.resynchronizing && !s.owned && near(s.baseline.pose, r.saved.pose),
              "long-readable old preview cannot become a new original after reset");
        r.settle(s);
        check(!s.resynchronizing && same(s.baseline, r.saved),
              "async hide eventually captures the actual saved calibration");
    }
    {
        Runtime r;
        Session s(r);
        s.enable();
        s.engine.xyz = true;
        s.updateMotion(true, false, true, {}, .01);
        s.updateMotion(true, true, true, {}, .01);
        s.updateMotion(true, true, true, {.05, -.05, 0}, .01);
        r.working.bounds.clear(); // Different coordinate representation while previewing.
        r.tick(s, .7);
        check(s.enabled && !s.resynchronizing && s.engine.held(),
              "boundary readback changes do not interrupt a held drag");
        const auto offset = s.engine.offset;
        r.live.pose.t.x += .5;
        r.settle(s, 10);
        check(r.writes == 1 && s.engine.held() && length(s.engine.offset - offset) < 1e-8,
              "idle preview repair does not fight an active gesture");
        s.event(SpaceEvent::DataChanged);
        const int hides = r.hides;
        r.tick(s, .1);
        s.event(SpaceEvent::DataChanged);
        r.tick(s, .1);
        check(r.hides == hides, "bursts of chaperone events are debounced");
        r.settle(s);
        check(s.enabled && !s.resynchronizing && length(s.engine.offset - offset) < 1e-8,
              "data reload preserves the requested offset without requiring button release");
        s.updateMotion(true, true, true, {4, 2, -3}, .01);
        check(s.engine.held() && length(s.engine.offset - offset) < 1e-8,
              "held controller reanchors on recovery without jumping");
        s.updateMotion(true, true, true, {4, 1.99, -3}, .01);
        check(std::abs(s.engine.offset.y - offset.y - .01) < 1e-8,
              "next held sample resumes ordinary drag");
    }
    {
        Runtime r;
        Session s(r);
        s.enable();
        move(s, {.2, .4, -.3});
        const Vec raw = s.baseline.pose.rotate(s.engine.offset);
        r.id = ++r.saved.universe;
        r.saved.pose.r[0][0] = r.saved.pose.r[2][2] = 0;
        r.saved.pose.r[0][2] = 1;
        r.saved.pose.r[2][0] = -1;
        r.saved.pose.t.x += 3;
        r.working = r.live = r.saved;
        r.settle(s);
        check(!s.resynchronizing && s.enabled &&
                  length(s.baseline.pose.rotate(s.engine.offset) - raw) < 1e-8,
              "real map changes preserve the raw-space movement across a rotated reference");
        r.id = temporaryUniverse;
        r.saved = r.working = r.live = fixture::frameSwitchActual();
        r.tracked = false;
        const auto backups = r.backups;
        r.settle(s);
        check(s.resynchronizing && s.enabled && r.backups == backups,
              "temporary 424242 universe is never adopted as a saved baseline");
        r.saved = r.working = r.live = fixture::frameSwitchBaseline();
        r.id = r.saved.universe;
        r.tracked = true;
        r.settle(s);
        check(!s.resynchronizing && s.enabled && length(s.engine.offset) == 0 && !s.owned,
              "tracking-world loss discards movement instead of transferring it to a new world");
    }
    {
        Runtime r;
        r.saved = r.working = r.live = fixture::frameIdleSpace(false);
        r.id = r.saved.universe;
        Session s(r);
        s.enable();
        r.saved = r.working = r.live = fixture::frameIdleSpace(true);
        r.tick(s);
        check(!s.resynchronizing && same(s.baseline, r.saved),
              "idle floor refinements are followed without disabling movement");
    }
    {
        Runtime r;
        Session s(r);
        s.enable();
        move(s, {0, .4, 0});
        s.event(SpaceEvent::Reset);
        check(!s.resynchronizing, "recent preview reset echo with matching copy is ignored");
        r.time += 1;
        r.saved.pose.t.z += .2;
        r.working = r.saved;
        s.event(SpaceEvent::Reset);
        r.settle(s);
        check(s.enabled && !s.owned && length(s.engine.offset) == 0 && same(s.baseline, r.saved),
              "real recenter drops the offset but leaves module on");
        move(s, {0, .1, 0});
        r.time += 1;
        r.working.pose.t.x += 1;
        s.watch();
        check(s.resynchronizing && s.enabled, "external edit schedules recovery");
        r.tick(s, 2);
        check(s.resynchronizing && r.hides == 1, "external editor gets time to finish");
        r.saved = r.working; // The external editor saves its new calibration.
        r.settle(s);
        check(length(s.engine.offset) == 0 && same(s.baseline, r.saved),
              "external edit is not overwritten with our old reset target");
    }
    {
        Runtime r;
        Session s(r);
        s.enable();
        move(s, {0, .5, 0});
        r.readable = false;
        r.tick(s, .8);
        check(s.enabled && s.resynchronizing,
              "temporary read failure suspends without toggling Off");
        r.settle(s, 5);
        r.readable = true;
        r.settle(s);
        check(s.enabled && !s.resynchronizing && std::abs(s.engine.offset.y - .5) < 1e-8,
              "read failure recovers the offset automatically");
        r.writeOk = false;
        s.engine.offset.y = .6;
        check(!s.apply() && s.enabled && s.resynchronizing,
              "failed preview submission does not permanently disable the module");
        r.writeOk = true;
        r.settle(s);
        check(!s.resynchronizing && std::abs(s.engine.offset.y - .6) < 1e-8,
              "submission failure retries after restoration");
    }
    {
        Runtime r;
        Session s(r);
        s.enable();
        move(s, {0, .5, 0});
        r.live.pose.t.y = r.saved.pose.t.y;
        r.tick(s, .7);
        check(r.repairs == 0 && !s.resynchronizing,
              "one live discrepancy cannot repair calibration");
        r.tick(s);
        r.tick(s);
        check(s.resynchronizing, "repeated height-only rollback requests floor repair");
        r.settle(s);
        check(r.repairs == 1 && !s.resynchronizing && s.enabled &&
                  std::abs(s.engine.offset.y - .5) < 1e-8,
              "floor repair reloads saved calibration then restores the offset");
        s.event(SpaceEvent::SetupFinished);
        r.settle(s, 10);
        check(std::abs(s.engine.offset.y - .5) < 1e-8,
              "our calibration commit echo does not erase movement");
        for (int i = 0; i < 4; ++i) {
            r.live.pose.t.y = r.saved.pose.t.y;
            r.tick(s, .7);
            r.tick(s);
            r.tick(s);
        }
        check(r.repairs == 1 && s.status.find("30 seconds") != std::string::npos,
              "floor commit is once per tracking world; repeated recovery backs off");
        const int writes = r.writes;
        r.settle(s, 200);
        check(r.writes == writes, "backoff prevents repeated preview writes");
        r.tick(s, 11);
        r.tick(s);
        r.tick(s);
        check(r.writes == writes + 1, "bounded preview repair resumes after backoff");
    }
    {
        Runtime r;
        Session s(r);
        s.enable();
        move(s, {0, .3, 0});
        r.repairOk = false;
        r.live.pose.t.y = r.saved.pose.t.y;
        r.tick(s, .7);
        r.tick(s);
        r.tick(s);
        r.settle(s);
        check(s.enabled && !s.resynchronizing && r.repairs == 1,
              "unavailable backup/repair falls back to the preview without disabling movement");
        move(s, {.2, .3, 0});
        s.roomSetupActive(true);
        r.tick(s, 120);
        check(s.resynchronizing && s.enabled, "active room setup stays suspended beyond timeout");
        s.roomSetupActive(false);
        r.settle(s);
        check(!s.resynchronizing && s.enabled && length(s.engine.offset) == 0,
              "room setup completion automatically captures the new original");
        move(s, {0, .1, 0});
        s.event(SpaceEvent::SetupStarted);
        r.tick(s, 91);
        r.settle(s);
        check(!s.resynchronizing && s.enabled, "cancelled event-only setup recovers after timeout");
    }
    {
        Runtime r;
        Session s(r);
        s.roomSetupActive(true);
        s.restoreMovementPreference(true);
        r.settle(s);
        check(!s.enabled && s.startupEnablePending() && r.backups == 0,
              "saved On waits for an already-running room setup before activation");
        s.roomSetupActive(false);
        r.tick(s);
        check(s.enabled && !s.startupEnablePending(), "activation resumes after room setup");
    }
    {
        Runtime r;
        Session s(r);
        s.enable();
        s.gravity.setEnabled(true);
        move(s, {0, .4, 0});
        for (int i = 0; i < 100; ++i) {
            r.time += .01;
            s.updateMotion(true, false, true, {}, .01);
            s.event(SpaceEvent::Reset);
        }
        check(s.enabled && !s.resynchronizing && s.engine.offset.y == 0 && s.gravity.speed() == 0 &&
                  r.hides == 0,
              "gravity lands at zero without hiding the preview or disabling movement");
        s.event(SpaceEvent::DataChanged);
        s.requestMovement(false);
        const int writes = r.writes;
        r.settle(s);
        check(!s.enabled && !s.resynchronizing && !s.movementRequested() && r.writes == writes,
              "Off cancels recovery and never reapplies later");
    }
    std::printf("%d checks passed\n", checks);
}
