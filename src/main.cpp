#include "playspace.h"
#include "preferences.h"
#include "setting_resets.h"
#include "build_version.h"
#include "panel.h"
#include "vk_texture.h"
#include "vulkan_dispatch.h"
#include "openvr_dispatch.h"
#include "openvr.h"
#include "openvr_bounds.h"
#include "session_log.h"
#include "log_export.h"
#include "launch_menu_controller.h"
#include "microphone_controller.h"
#include "settings_controller.h"
#include "app_registration.h"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
static volatile std::sig_atomic_t stopping = 0;
static void stop(int) {
    stopping = 1;
}
// Retain the registered identity so upgrades preserve existing SteamVR bindings.
static constexpr const char *appKey = appregistration::key;
static void log(const std::string &s) {
    sessionlog::info("app", s);
}
static std::string loggingNotice() {
    const auto status = sessionlog::statusText();
    if (status.find("unavailable") != std::string::npos ||
        status.find("not started") != std::string::npos)
        return "Could not write log files.";
    if (status.find("dropped") != std::string::npos)
        return "Some log records were dropped.";
    if (status.find("truncated") != std::string::npos)
        return "Some log records were shortened.";
    return {};
}
static drag::Transform fromVr(const vr::HmdMatrix34_t &m) {
    drag::Transform t;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            t.r[i][j] = m.m[i][j];
    t.t = {m.m[0][3], m.m[1][3], m.m[2][3]};
    return t;
}
static vr::HmdMatrix34_t toVr(const drag::Transform &t) {
    vr::HmdMatrix34_t m{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            m.m[i][j] = static_cast<float>(t.r[i][j]);
    m.m[0][3] = t.t.x;
    m.m[1][3] = t.t.y;
    m.m[2][3] = t.t.z;
    return m;
}
using drag::Session;
using drag::Space;
static bool readSpace(Space &s, bool refreshWorking = true, std::string *problem = nullptr,
                      drag::RuntimeBoundsOrigins *boundsOrigins = nullptr) {
    if (problem)
        problem->clear();
    auto fail = [&](const std::string &reason) {
        if (problem)
            *problem = reason;
        return false;
    };
    auto c = vr::VRChaperoneSetup();
    auto calibration = vr::VRChaperone()->GetCalibrationState();
    if (calibration < vr::ChaperoneCalibrationState_OK ||
        calibration >= vr::ChaperoneCalibrationState_Error)
        return fail("Calibration state is " + std::to_string(static_cast<int>(calibration)));
    // Reverting while previewing would discard our current offset on every read.
    if (refreshWorking)
        c->RevertWorkingCopy();
    vr::HmdMatrix34_t m{};
    if (!c->GetWorkingStandingZeroPoseToRawTrackingPose(&m))
        return fail("Working standing origin query returned false");
    s.pose = fromVr(m);
    if (!s.pose.valid())
        return fail("Working standing origin is not a finite rigid transform");
    std::string boundaryProblem;
    if (boundsOrigins)
        boundsOrigins->before =
            fromVr(vr::VRSystem()->GetRawZeroPoseToStandingAbsoluteTrackingPose()).inverse();
    if (!drag::readLiveBounds(
            [c](vr::HmdQuad_t *quads, uint32_t *count) {
                return c->GetLiveCollisionBoundsInfo(quads, count);
            },
            s.bounds, boundaryProblem))
        return fail(boundaryProblem);
    if (boundsOrigins)
        boundsOrigins->after =
            fromVr(vr::VRSystem()->GetRawZeroPoseToStandingAbsoluteTrackingPose()).inverse();
    vr::ETrackedPropertyError e = vr::TrackedProp_Success;
    s.universe =
        vr::VRSystem()->GetUint64TrackedDeviceProperty(0, vr::Prop_CurrentUniverseId_Uint64, &e);
    if (e != vr::TrackedProp_Success)
        return fail("Universe ID query: " +
                    std::string(vr::VRSystem()->GetPropErrorNameFromEnum(e)));
    if (s.universe == 0)
        return fail("Universe ID is zero; current prototype requires a nonzero ID");
    return true;
}
static fs::path stateDirectory() {
    const char *state = std::getenv("XDG_STATE_HOME");
    const char *home = std::getenv("HOME");
    if (state && *state && fs::path(state).is_absolute())
        return fs::path(state) / "frame-advanced-settings";
    if (home && *home)
        return fs::path(home) / ".local/state/frame-advanced-settings";
    throw std::runtime_error("HOME is not set");
}
static std::string escaped(const std::string &s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\')
            out += '\\';
        if (c >= 32)
            out += c;
        else
            out += ' ';
    }
    return out;
}
static void matrix(std::ostream &f, const drag::Transform &t) {
    if (!t.valid()) {
        f << "null";
        return;
    }
    auto m = toVr(t);
    f << "[";
    for (int i = 0; i < 3; i++) {
        if (i)
            f << ",";
        f << "[";
        for (int j = 0; j < 4; j++) {
            if (j)
                f << ",";
            f << m.m[i][j];
        }
        f << "]";
    }
    f << "]";
}
static void spaceJson(std::ostream &f, const Space &s) {
    f << "{\"universe\":" << s.universe << ",\"standing_to_raw\":";
    matrix(f, s.pose);
    f << ",\"bounds\":[";
    for (size_t i = 0; i < s.bounds.size(); i++) {
        if (i)
            f << ",";
        f << "[";
        for (int j = 0; j < 4; j++) {
            if (j)
                f << ",";
            auto &v = s.bounds[i][j];
            f << "[" << v.x << "," << v.y << "," << v.z << "]";
        }
        f << "]";
    }
    f << "]}";
}
static bool backup(const Space &s) {
    auto dir = stateDirectory();
    fs::create_directories(dir);
    // Keep one durable current reset reference; historical references live in
    // the bounded session logs instead of accumulating unbounded backup files.
    const auto path = dir / "baseline.json";
    const auto tmp = dir / "baseline.tmp";
    std::ostringstream data;
    data.precision(10);
    spaceJson(data, s);
    const auto payload = data.str();
    if (payload.size() > 128 * 1024)
        return false;
    std::ofstream f(tmp);
    f << payload << '\n';
    f.close();
    if (!f)
        return false;
    fs::rename(tmp, path);
    sessionlog::eventJson("baseline", payload);
    log("Original playspace saved to " + path.string());
    return true;
}

static void saveDiagnostic(const fs::path &path, const std::string &data, const char *category) {
    // Fixed compatibility snapshots remain useful for SSH collection. Historical
    // records go through the rotating logger, including completed gestures.
    sessionlog::eventJson(category, data);
    if (data.size() > 2 * 1024 * 1024) {
        sessionlog::warn(category, "Snapshot exceeds 2 MiB; omitted fixed-file copy");
        return;
    }
    auto tmp = path;
    tmp += ".tmp";
    std::ofstream output(tmp);
    output << data;
    output.close();
    if (!output)
        throw std::runtime_error("Cannot write diagnostic snapshot");
    fs::rename(tmp, path);
}

static std::string preferenceSummary(const drag::Preferences &p) {
    std::ostringstream out;
    out << std::boolalpha << "axes=" << (p.xyz ? "xyz" : "height") << " gain=" << p.gain
        << " gravity=" << p.gravity << " gravity_strength=" << p.gravityStrength
        << " limit=" << p.distanceLimit << " limit_m=" << p.distanceLimitMeters
        << " movement_requested=" << p.movementEnabled << " detailed_logging=" << p.detailedLogging;
    return out.str();
}
struct MotionSample {
    int64_t time;
    uint32_t controller;
    bool inputOk, active, pressed, enabled, resynchronizing;
    vr::TrackedDevicePose_t hand, head;
    drag::Vec offset;
    drag::Transform expectedOrigin;
    uint64_t expectedUniverse;
    int64_t readTime;
    std::optional<drag::Transform> observedOrigin, runtimeOrigin;
};
class OpenVRSpaceAccess : public drag::SpaceAccess {
    Space original_;
    bool preview_ = false;
    bool boundsReexpressionLogged_ = false, runtimeBoundsLogged_ = false, mismatchSaved_ = false;
    std::optional<Space> rawRead_, submitted_;
    std::optional<drag::RuntimeBoundsOrigins> boundsOrigins_;
    std::string readProblem_;
    int64_t lastReadAt_ = 0;
    std::deque<MotionSample> motionHistory_;
    bool firstMismatchSaved_ = false;
    bool dragTraceActive_ = false;
    int64_t dragStartedAt_ = 0;
    int64_t debugSampleAt_ = 0;

    void writeMotionHistory(std::ostream &f) const {
        f << "[";
        bool first = true;
        for (const auto &sample : motionHistory_) {
            if (!first)
                f << ",";
            first = false;
            f << "{\"time_unix_ms\":" << sample.time << ",\"controller_index\":";
            if (sample.controller < vr::k_unMaxTrackedDeviceCount)
                f << sample.controller;
            else
                f << "null";
            f << ",\"input_ok\":" << (sample.inputOk ? "true" : "false")
              << ",\"drag_active\":" << (sample.active ? "true" : "false")
              << ",\"drag_pressed\":" << (sample.pressed ? "true" : "false")
              << ",\"enabled\":" << (sample.enabled ? "true" : "false")
              << ",\"resynchronizing\":" << (sample.resynchronizing ? "true" : "false")
              << ",\"hand_tracking\":" << static_cast<int>(sample.hand.eTrackingResult)
              << ",\"head_tracking\":" << static_cast<int>(sample.head.eTrackingResult)
              << ",\"raw_hand\":";
            if (sample.hand.bPoseIsValid && sample.hand.bDeviceIsConnected)
                matrix(f, fromVr(sample.hand.mDeviceToAbsoluteTracking));
            else
                f << "null";
            f << ",\"raw_head\":";
            if (sample.head.bPoseIsValid && sample.head.bDeviceIsConnected)
                matrix(f, fromVr(sample.head.mDeviceToAbsoluteTracking));
            else
                f << "null";
            f << ",\"offset\":[" << sample.offset.x << "," << sample.offset.y << ","
              << sample.offset.z << "],\"expected_universe\":" << sample.expectedUniverse
              << ",\"expected_standing_to_raw\":";
            matrix(f, sample.expectedOrigin);
            f << ",\"observed_read_unix_ms\":" << sample.readTime
              << ",\"working_standing_to_raw\":";
            if (sample.observedOrigin)
                matrix(f, *sample.observedOrigin);
            else
                f << "null";
            f << ",\"runtime_standing_to_raw\":";
            if (sample.runtimeOrigin)
                matrix(f, *sample.runtimeOrigin);
            else
                f << "null";
            f << "}";
        }
        f << "]";
    }
    void saveDragTrace(const Session &session, const char *reason) {
        try {
            const auto path = stateDirectory() / "last-drag.json";
            std::ostringstream f;
            f.precision(10);
            f << "{\"build\":\"" << build::full << "\",\"pid\":" << getpid()
              << ",\"updated_unix_ms\":" << motionHistory_.back().time
              << ",\"drag_started_unix_ms\":" << dragStartedAt_ << ",\"reason\":\"" << reason
              << "\",\"drag_gain\":" << session.engine.gain << ",\"movement_axes\":\""
              << (session.engine.xyz ? "xyz" : "height") << "\",\"baseline\":";
            spaceJson(f, session.baseline);
            f << ",\"expected\":";
            spaceJson(f, session.expected);
            f << ",\"raw_readback\":";
            if (rawRead_)
                spaceJson(f, *rawRead_);
            else
                f << "null";
            f << ",\"motion_history\":";
            writeMotionHistory(f);
            f << "}\n";
            saveDiagnostic(path, f.str(), "drag-completed");
            log("Saved completed drag tracking history to " + path.string());
        } catch (const std::exception &ex) {
            sessionlog::error("diagnostics", ex.what());
        }
    }

  public:
    void recordMotion(uint32_t controller, bool inputOk, const vr::InputDigitalActionData_t &action,
                      const vr::TrackedDevicePose_t &hand, const vr::TrackedDevicePose_t &head,
                      const Session &session) {
        const auto time = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
        motionHistory_.push_back(
            {time, controller, inputOk, action.bActive, action.bState, session.enabled,
             session.resynchronizing, hand, head, session.engine.offset, session.expected.pose,
             session.expected.universe, lastReadAt_,
             rawRead_ ? std::optional{rawRead_->pose} : std::nullopt,
             boundsOrigins_ ? std::optional{boundsOrigins_->after} : std::nullopt});
        if (motionHistory_.size() > 180)
            motionHistory_.pop_front();
        if (sessionlog::detailed() && time >= debugSampleAt_) {
            debugSampleAt_ = time + 100;
            std::ostringstream out;
            out.precision(10);
            out << std::boolalpha << "{\"sample_ms\":" << time << ",\"controller\":" << controller
                << ",\"input_ok\":" << inputOk << ",\"pressed\":" << action.bState
                << ",\"active\":" << action.bActive << ",\"enabled\":" << session.enabled
                << ",\"resynchronizing\":" << session.resynchronizing
                << ",\"hand_tracking\":" << hand.eTrackingResult
                << ",\"head_tracking\":" << head.eTrackingResult
                << ",\"hand_valid\":" << hand.bPoseIsValid
                << ",\"head_valid\":" << head.bPoseIsValid << ",\"offset\":["
                << session.engine.offset.x << ',' << session.engine.offset.y << ','
                << session.engine.offset.z << "],\"universe\":" << session.expected.universe
                << ",\"raw_hand\":";
            if (hand.bPoseIsValid && hand.bDeviceIsConnected)
                matrix(out, fromVr(hand.mDeviceToAbsoluteTracking));
            else
                out << "null";
            out << ",\"raw_head\":";
            if (head.bPoseIsValid && head.bDeviceIsConnected)
                matrix(out, fromVr(head.mDeviceToAbsoluteTracking));
            else
                out << "null";
            out << ",\"expected_origin\":";
            matrix(out, session.expected.pose);
            out << ",\"observed_read_ms\":" << lastReadAt_ << ",\"runtime_origin\":";
            if (boundsOrigins_)
                matrix(out, boundsOrigins_->after);
            else
                out << "null";
            out << '}';
            sessionlog::debug("tracking", out.str());
        }
        // Save while the gesture is still in the ring, even when no mismatch
        // occurred. A later standby event must not replace the held-drag data.
        if (dragTraceActive_ &&
            (!session.engine.held() || !inputOk || !action.bActive || !action.bState)) {
            dragTraceActive_ = false;
            saveDragTrace(session,
                          inputOk && action.bActive && !action.bState ? "released" : "interrupted");
        } else if (!dragTraceActive_ && session.engine.held() && inputOk && action.bActive &&
                   action.bState) {
            dragTraceActive_ = true;
            dragStartedAt_ = time;
        }
    }
    bool read(Space &result) override {
        std::string problem;
        drag::RuntimeBoundsOrigins sampledOrigins;
        const bool resultOk =
            readSpace(result, !preview_, &problem, preview_ ? &sampledOrigins : nullptr);
        boundsOrigins_.reset();
        if (resultOk) {
            rawRead_ = result;
            lastReadAt_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
            if (preview_) {
                boundsOrigins_ = sampledOrigins;
                const auto match = drag::normalizePreviewBounds(result, original_, &sampledOrigins);
                if (match == drag::BoundsMatch::Reexpressed && !boundsReexpressionLogged_) {
                    log("Boundary coordinates follow the preview origin; all corners match the "
                        "baseline in raw space. Normalizing readback only.");
                    boundsReexpressionLogged_ = true;
                }
                if (match == drag::BoundsMatch::RuntimeReexpressed && !runtimeBoundsLogged_) {
                    log("Live boundary coordinates use the runtime origin at the baseline floor; "
                        "all corners match in raw space. Kept the working movement offset.");
                    runtimeBoundsLogged_ = true;
                }
            }
        }
        if (!resultOk && problem != readProblem_)
            log("Playspace read: " + problem);
        readProblem_ = problem;
        return resultOk;
    }
    std::string readProblem() const override { return readProblem_; }
    bool saveBaseline(const Space &original) override {
        if (!backup(original))
            return false;
        original_ = original;
        boundsReexpressionLogged_ = runtimeBoundsLogged_ = mismatchSaved_ = false;
        submitted_.reset();
        return true;
    }
    void reportMismatch(const char *stage, const Space &expected, const Space &actual) override {
        // Preserve the first failure, before cancelling or rolling back the
        // preview can replace it with a secondary recenter notification.
        if (mismatchSaved_)
            return;
        try {
            const auto path = stateDirectory() / "last-space-mismatch.json";
            std::ostringstream f;
            f.precision(10);
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
            f << "{\"build\":\"" << build::full << "\",\"pid\":" << getpid()
              << ",\"updated_unix_ms\":" << now << ",\"stage\":\"" << escaped(stage)
              << "\",\"preview_active\":" << (preview_ ? "true" : "false") << ",\"baseline\":";
            spaceJson(f, original_);
            f << ",\"expected\":";
            spaceJson(f, expected);
            f << ",\"actual\":";
            spaceJson(f, actual);
            f << ",\"raw_readback\":";
            if (rawRead_)
                spaceJson(f, *rawRead_);
            else
                f << "null";
            f << ",\"last_submitted\":";
            if (submitted_)
                spaceJson(f, *submitted_);
            else
                f << "null";
            f << ",\"boundary_origins_standing_to_raw\":";
            if (boundsOrigins_) {
                f << "{\"before\":";
                matrix(f, boundsOrigins_->before);
                f << ",\"after\":";
                matrix(f, boundsOrigins_->after);
                f << "}";
            } else {
                f << "null";
            }
            f << ",\"runtime_raw_to_standing\":";
            matrix(f, fromVr(vr::VRSystem()->GetRawZeroPoseToStandingAbsoluteTrackingPose()));
            // Bounded in-memory samples from before each loop's movement write.
            // These diagnose raw-pose feedback and controller-source changes;
            // they do not claim that the sampled working pose was displayed.
            f << ",\"motion_history\":";
            writeMotionHistory(f);
            f << "}\n";
            saveDiagnostic(path, f.str(), "playspace-mismatch");
            mismatchSaved_ = true;
            sessionlog::warn("playspace", std::string("First playspace mismatch at ") + stage +
                                              ": " + drag::changeReason(expected, actual) +
                                              " Details saved to " + path.string());
            // Later resynchronizations used to overwrite the earlier failure
            // that initiated a sequence of jumps. Keep the first of this run too.
            if (!firstMismatchSaved_) {
                std::error_code error;
                fs::copy_file(path, stateDirectory() / "first-space-mismatch.json",
                              fs::copy_options::overwrite_existing, error);
                if (error)
                    log("Could not preserve first mismatch: " + error.message());
                else
                    firstMismatchSaved_ = true;
            }
        } catch (const std::exception &ex) {
            sessionlog::error("diagnostics", ex.what());
        }
    }
    void cancelPreview() override {
        if (!preview_)
            return;
        vr::VRChaperoneSetup()->HideWorkingSetPreview();
        vr::VRChaperoneSetup()->RevertWorkingCopy();
        preview_ = false;
    }
    bool write(const Space &target, drag::WriteMode mode) override {
        submitted_ = target;
        // Passing through zero during motion is still a preview. Hiding it here
        // exposes the live Frame origin and can look like an external recenter.
        if (mode == drag::WriteMode::Restore && drag::same(target, original_)) {
            cancelPreview();
            return true;
        }
        auto c = vr::VRChaperoneSetup();
        auto pose = toVr(target.pose);
        c->SetWorkingStandingZeroPoseToRawTrackingPose(&pose);
        // OVRAS uses this API for ordinary motion. It previews the working origin
        // in the compositor without a disk commit or a rewrite of boundary quads.
        // This void API only confirms submission; visible behavior needs a Frame test.
        c->ShowWorkingSetPreview();
        preview_ = true;
        return true;
    }
};
struct Input {
    vr::VRActionSetHandle_t set = 0;
    vr::VRActionHandle_t drag = 0, reset = 0, gravity = 0;
    vr::InputDigitalActionData_t d{}, r{}, g{};
    uint32_t controller = vr::k_unTrackedDeviceIndexInvalid;
    vr::TrackedDevicePose_t controllerPose{}, headPose{};
    bool init(const fs::path &root) {
        auto apps = vr::VRApplications();
        if (!apps) {
            log("Application registration interface unavailable.");
            return false;
        }
        const auto e = apps->IdentifyApplication(static_cast<uint32_t>(getpid()), appKey);
        if (e != vr::VRApplicationError_None) {
            log("Identify app: " + std::string(apps->GetApplicationsErrorNameFromEnum(e)));
            return false;
        }
        char actualKey[vr::k_unMaxApplicationKeyLength]{};
        const auto identityError = apps->GetApplicationKeyByProcessId(
            static_cast<uint32_t>(getpid()), actualKey, sizeof(actualKey));
        if (identityError != vr::VRApplicationError_None || std::strcmp(actualKey, appKey) != 0) {
            log("Application identity does not match the controller-binding editor: " +
                std::string(actualKey));
            return false;
        }
        log("Verified application identity: " + std::string(actualKey));
        // Identify the process before acquiring its input interface.
        auto i = vr::VRInput();
        if (!i) {
            log("Input interface unavailable.");
            return false;
        }
        auto actionPath = (root / "input/actions.json").string();
        auto checkInput = [](const char *step, vr::EVRInputError error) {
            if (error == vr::VRInputError_None)
                return true;
            log(std::string(step) + " failed (VRInputError " +
                std::to_string(static_cast<int>(error)) + ")");
            return false;
        };
        const bool ok =
            checkInput("SetActionManifestPath", i->SetActionManifestPath(actionPath.c_str())) &&
            checkInput("GetActionSetHandle /actions/drag",
                       i->GetActionSetHandle("/actions/drag", &set)) &&
            checkInput("GetActionHandle move",
                       i->GetActionHandle("/actions/drag/in/move", &drag)) &&
            checkInput("GetActionHandle reset",
                       i->GetActionHandle("/actions/drag/in/reset", &reset)) &&
            checkInput("GetActionHandle gravity",
                       i->GetActionHandle("/actions/drag/in/gravity", &gravity));
        if (ok)
            log("Input actions ready: " + actionPath);
        return ok;
    }
    bool poll(drag::Vec &hand, bool &tracked) {
        d = {};
        r = {};
        g = {};
        tracked = false;
        controller = vr::k_unTrackedDeviceIndexInvalid;
        controllerPose = headPose = {};
        vr::VRActiveActionSet_t active{};
        active.ulActionSet = set;
        auto i = vr::VRInput();
        if (i->UpdateActionState(&active, sizeof(active), 1) != vr::VRInputError_None)
            return false;
        if (i->GetDigitalActionData(drag, &d, sizeof(d), vr::k_ulInvalidInputValueHandle) !=
                vr::VRInputError_None ||
            i->GetDigitalActionData(reset, &r, sizeof(r), vr::k_ulInvalidInputValueHandle) !=
                vr::VRInputError_None ||
            i->GetDigitalActionData(gravity, &g, sizeof(g), vr::k_ulInvalidInputValueHandle) !=
                vr::VRInputError_None)
            return false;
        if (!d.bActive)
            return true;
        vr::InputOriginInfo_t origin{};
        if (i->GetOriginTrackedDeviceInfo(d.activeOrigin, &origin, sizeof(origin)) !=
                vr::VRInputError_None ||
            origin.trackedDeviceIndex >= vr::k_unMaxTrackedDeviceCount)
            return true;
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseRawAndUncalibrated, 0,
                                                        poses, vr::k_unMaxTrackedDeviceCount);
        auto &p = poses[origin.trackedDeviceIndex];
        auto &h = poses[0];
        controller = origin.trackedDeviceIndex;
        controllerPose = p;
        headPose = h;
        tracked = p.bPoseIsValid && p.bDeviceIsConnected &&
                  p.eTrackingResult == vr::TrackingResult_Running_OK && h.bPoseIsValid &&
                  h.bDeviceIsConnected && h.eTrackingResult == vr::TrackingResult_Running_OK;
        hand = {p.mDeviceToAbsoluteTracking.m[0][3], p.mDeviceToAbsoluteTracking.m[1][3],
                p.mDeviceToAbsoluteTracking.m[2][3]};
        return true;
    }
};
static void updatePanelState(const Session &session, PanelState &state) {
    state.enabled = session.enabled;
    state.dragging = session.engine.held();
    state.xyz = session.engine.xyz;
    state.gain = session.engine.gain;
    state.gravity = session.gravity.enabled();
    state.gravityStrength = session.gravity.strength();
    state.distanceLimit = session.engine.distanceLimit.enabled;
    state.distanceLimitMeters = session.engine.distanceLimit.meters();
    state.offset = session.engine.offset;
    state.status = session.status;
    state.statusAttention = session.statusAttention;
}
struct StatusDetails {
    bool dashboard = false, inputOk = false, handTracked = false, dragPressed = false;
    bool dragBound = false, resetBound = false;
    bool resynchronizing = false;
    bool previewRetained = false;
};
static void statusFile(const PanelState &p, const StatusDetails &details = {}) {
    auto root = stateDirectory();
    fs::create_directories(root);
    auto tmp = root / "status.tmp";
    auto updated = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
    std::ofstream f(tmp);
    f << "{\"build\":\"" << build::full << "\",\"pid\":" << getpid()
      << ",\"updated_unix_ms\":" << updated << ",\"enabled\":" << (p.enabled ? "true" : "false")
      << ",\"gravity\":" << (p.gravity ? "true" : "false")
      << ",\"gravity_strength\":" << p.gravityStrength
      << ",\"distance_limit_enabled\":" << (p.distanceLimit ? "true" : "false")
      << ",\"distance_limit_m\":" << p.distanceLimitMeters
      << ",\"resynchronizing\":" << (details.resynchronizing ? "true" : "false")
      << ",\"preview_retained_during_resync\":" << (details.previewRetained ? "true" : "false")
      << ",\"drag_bound\":" << (details.dragBound ? "true" : "false")
      << ",\"reset_bound\":" << (details.resetBound ? "true" : "false")
      << ",\"dashboard_open\":" << (details.dashboard ? "true" : "false")
      << ",\"input_ok\":" << (details.inputOk ? "true" : "false")
      << ",\"hand_tracked\":" << (details.handTracked ? "true" : "false")
      << ",\"drag_pressed\":" << (details.dragPressed ? "true" : "false") << ",\"offset\":["
      << p.offset.x << "," << p.offset.y << "," << p.offset.z << "],\"status\":\""
      << escaped(p.status)
      << "\",\"detailed_logging\":" << (sessionlog::detailed() ? "true" : "false")
      << ",\"log_session\":\"" << escaped(sessionlog::sessionDirectory().filename().string())
      << "\",\"dropped_log_records\":" << sessionlog::droppedRecords() << "}\n";
    f.close();
    if (f)
        fs::rename(tmp, root / "status.json");
}
static bool probeSpace() {
    Space space;
    std::string problem;
    const bool ready = readSpace(space, true, &problem);
    std::printf("Movement readiness: %s\n", ready ? "passed" : "blocked");
    if (!ready)
        std::printf("First failed check: %s\n", problem.c_str());
    auto setup = vr::VRChaperoneSetup();
    vr::HmdMatrix34_t working{};
    const bool originOk = setup->GetWorkingStandingZeroPoseToRawTrackingPose(&working);
    std::printf("Working standing origin: query=%s rigid=%s\n", originOk ? "true" : "false",
                fromVr(working).valid() ? "true" : "false");
    for (const auto &row : working.m)
        std::printf("  %.6f %.6f %.6f %.6f\n", row[0], row[1], row[2], row[3]);
    for (bool live : {true, false}) {
        uint32_t count = 0;
        const bool countOk = live ? setup->GetLiveCollisionBoundsInfo(nullptr, &count)
                                  : setup->GetWorkingCollisionBoundsInfo(nullptr, &count);
        std::printf("%s boundary count: query=%s count=%u\n", live ? "Live" : "Working",
                    countOk ? "true" : "false", count);
        if (count > 0 && count <= 512) {
            std::vector<vr::HmdQuad_t> quads(count);
            const uint32_t capacity = count;
            const bool dataOk = live ? setup->GetLiveCollisionBoundsInfo(quads.data(), &count)
                                     : setup->GetWorkingCollisionBoundsInfo(quads.data(), &count);
            bool finite = dataOk && count <= capacity;
            for (size_t q = 0; q < std::min(count, capacity); ++q)
                for (const auto &corner : quads[q].vCorners)
                    for (float value : corner.v)
                        finite = finite && std::isfinite(value);
            std::printf("%s boundary data: query=%s count=%u finite=%s\n",
                        live ? "Live" : "Working", dataOk ? "true" : "false", count,
                        finite ? "true" : "false");
        }
    }
    vr::ETrackedPropertyError error = vr::TrackedProp_Success;
    const auto universe = vr::VRSystem()->GetUint64TrackedDeviceProperty(
        vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_CurrentUniverseId_Uint64, &error);
    std::printf("Universe ID: %llu; property status=%s (%d)\n",
                static_cast<unsigned long long>(universe),
                vr::VRSystem()->GetPropErrorNameFromEnum(error), static_cast<int>(error));
    float width = 0, depth = 0;
    const bool areaOk = setup->GetWorkingPlayAreaSize(&width, &depth);
    std::printf("Working play area: query=%s width=%.3f depth=%.3f\n", areaOk ? "true" : "false",
                width, depth);
    return ready;
}
static int probe() {
    vr::EVRInitError e = vr::VRInitError_None;
    vr::VR_Init(&e, vr::VRApplication_Background);
    if (e != vr::VRInitError_None) {
        log(vr::VR_GetVRInitErrorAsEnglishDescription(e));
        return 2;
    }
    if (!vr::VRSystem() || !vr::VRChaperone() || !vr::VRChaperoneSetup()) {
        log("This runtime does not expose the required playspace interfaces.");
        vr::VR_Shutdown();
        return 2;
    }
    char model[256] = {};
    vr::VRSystem()->GetStringTrackedDeviceProperty(0, vr::Prop_ModelNumber_String, model,
                                                   sizeof(model));
    std::printf(
        "Frame Advanced Settings %s read-only probe (diagnostics 3)\nHMD: %s\nCalibration: %d\n",
        build::full, model, vr::VRChaperone()->GetCalibrationState());
    bool ok = probeSpace();
    if (!ok) {
        std::this_thread::sleep_for(std::chrono::milliseconds(750));
        std::printf("After 750 ms: calibration=%d\n", vr::VRChaperone()->GetCalibrationState());
        ok = probeSpace();
    }
    const auto rawToStanding = vr::VRSystem()->GetRawZeroPoseToStandingAbsoluteTrackingPose();
    std::puts("Runtime raw-to-standing transform:");
    for (const auto &row : rawToStanding.m)
        std::printf("  %.6f %.6f %.6f %.6f\n", row[0], row[1], row[2], row[3]);
    if (vr::VRCompositor())
        std::printf("Compositor tracking space: %d (standing=1)\n",
                    vr::VRCompositor()->GetTrackingSpace());
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseRawAndUncalibrated, 0,
                                                    poses, vr::k_unMaxTrackedDeviceCount);
    for (uint32_t j = 0; j < vr::k_unMaxTrackedDeviceCount; j++)
        if (poses[j].bDeviceIsConnected)
            std::printf("Device %u: class=%d role=%d valid=%d tracking=%d\n", j,
                        vr::VRSystem()->GetTrackedDeviceClass(j),
                        vr::VRSystem()->GetControllerRoleForTrackedDeviceIndex(j),
                        poses[j].bPoseIsValid, poses[j].eTrackingResult);
    vr::VR_Shutdown();
    return ok ? 0 : 10;
}
static int run(const fs::path &root, bool background) {
    std::string error;
    vr::EVRInitError initError = vr::VRInitError_None;
    vr::VR_Init(&initError, vr::VRApplication_Utility);
    if (initError != vr::VRInitError_None) {
        log(vr::VR_GetVRInitErrorAsEnglishDescription(initError));
        return 2;
    }
    // Register before connecting as an overlay, so the executable, binding
    // editor and input client all refer to the same installed application.
    auto apps = vr::VRApplications();
    const auto registrationError = apps ? appregistration::registerCurrent(*apps, root)
                                        : "Application registration interface unavailable.";
    vr::VR_Shutdown();
    if (!registrationError.empty()) {
        log(registrationError);
        return 2;
    }
    log("Verified current application and action-manifest paths.");
    vr::VR_Init(&initError, vr::VRApplication_Overlay);
    if (initError != vr::VRInitError_None) {
        log(vr::VR_GetVRInitErrorAsEnglishDescription(initError));
        return 2;
    }
    Input input;
    if (!input.init(root)) {
        log("Could not load action bindings.");
        vr::VR_Shutdown();
        return 2;
    }
    if (!vr::VRSystem() || !vr::VRChaperone() || !vr::VRChaperoneSetup() || !vr::VROverlay() ||
        !vr::VRCompositor()) {
        log("This runtime does not expose the required dashboard/input/playspace interfaces.");
        vr::VR_Shutdown();
        return 2;
    }
    OpenVRSpaceAccess spaceAccess;
    Session session(spaceAccess);
    fs::path preferencesFile;
    drag::Preferences savedPreferences;
    bool automaticUpdateChecks = true;
    bool preferencesReady = false;
    std::string preferencesProblem;
    auto nextPreferencesSave = Clock::now();
    auto savePreferenceChanges = [&] {
        if (!preferencesReady)
            return;
        auto current = drag::Preferences::capture(session);
        current.detailedLogging = sessionlog::detailed();
        current.automaticUpdateChecks = automaticUpdateChecks;
        if (current == savedPreferences) {
            preferencesProblem.clear();
            return;
        }
        std::string problem;
        if (drag::savePreferences(preferencesFile, current, problem)) {
            savedPreferences = current;
            sessionlog::info("preferences", preferenceSummary(current));
            if (!preferencesProblem.empty())
                log("Preferences saved successfully after retry.");
            preferencesProblem.clear();
        } else {
            if (problem != preferencesProblem)
                log(problem);
            preferencesProblem = problem;
            nextPreferencesSave = Clock::now() + std::chrono::seconds(5);
        }
    };
    VulkanContext vulkan;
    OverlayTexture texture;
    vr::VROverlayHandle_t overlay = 0, icon = 0;
    bool vrQuit = false;
    auto cleanup = [&] {
        savePreferenceChanges();
        session.disable();
        // Also hide our preview if a final readback could not complete during shutdown.
        spaceAccess.cancelPreview();
        try {
            PanelState finalState;
            updatePanelState(session, finalState);
            finalState.status = session.needsRestore() ? session.status : "Stopped.";
            statusFile(finalState);
        } catch (const std::exception &ex) {
            log(std::string("Could not save final status: ") + ex.what());
        }
        if (overlay) {
            vr::VROverlay()->ClearOverlayTexture(overlay);
            vr::VROverlay()->ClearOverlayTexture(icon);
            vr::VROverlay()->DestroyOverlay(overlay);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        if (vrQuit)
            vr::VRSystem()->AcknowledgeQuit_Exiting();
        vr::VR_Shutdown();
        texture.destroy();
        vulkan.destroy();
    };
    try {
        preferencesFile =
            drag::preferencesPath(std::getenv("XDG_CONFIG_HOME"), std::getenv("HOME"));
        const auto loadedPreferences = drag::loadPreferences(preferencesFile);
        loadedPreferences.values.apply(session);
        sessionlog::setDetailed(loadedPreferences.values.detailedLogging);
        automaticUpdateChecks = loadedPreferences.values.automaticUpdateChecks;
        savedPreferences = drag::Preferences::capture(session);
        savedPreferences.detailedLogging = sessionlog::detailed();
        savedPreferences.automaticUpdateChecks = automaticUpdateChecks;
        sessionlog::info("preferences", preferenceSummary(savedPreferences));
        preferencesReady = true;
        if (!loadedPreferences.problem.empty())
            log(loadedPreferences.problem);
        else if (loadedPreferences.found)
            log("Loaded preferences from " + preferencesFile.string());
        if (!loadVulkan(error) || !vulkan.init(error))
            throw std::runtime_error(error);
        auto o = vr::VROverlay();
        auto e = o->CreateDashboardOverlay(appKey, "Frame Advanced Settings", &overlay, &icon);
        if (e != vr::VROverlayError_None)
            throw std::runtime_error(o->GetOverlayErrorNameFromEnum(e));
        auto check = [&](vr::EVROverlayError err) {
            if (err != vr::VROverlayError_None)
                throw std::runtime_error(o->GetOverlayErrorNameFromEnum(err));
        };
        check(o->SetOverlayWidthInMeters(overlay, Panel::widthMeters));
        check(o->SetOverlayInputMethod(overlay, vr::VROverlayInputMethod_Mouse));
        vr::HmdVector2_t scale{{Panel::width, Panel::height}};
        check(o->SetOverlayMouseScale(overlay, &scale));
        check(o->SetOverlayFlag(overlay, vr::VROverlayFlags_EnableControlBarClose, true));
        check(o->SetOverlayFlag(overlay, vr::VROverlayFlags_EnableClickStabilization, true));
        check(o->SetOverlayFlag(overlay, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true));
        if (!texture.create(vulkan, Panel::width, Panel::height, error))
            throw std::runtime_error(error);
        const auto iconPath = (root / "assets/icons/frame-advanced-settings.png").string();
        check(o->SetOverlayFromFile(icon, iconPath.c_str()));
        const auto runtimePath = openVRRuntimePath();
        Panel panel(root / "assets/fonts", runtimePath.empty()
                                               ? fs::path{}
                                               : runtimePath / "resources/webinterface/fonts");
        log("Dashboard font: " + panel.fontFamily());
        PanelState ps;
        LogExport logExport;
        MicrophoneController microphone;
        LaunchMenuController launchMenu;
        SettingsController settings(root, stateDirectory());
        ps.selfUpdates = build::selfUpdates;
        ps.automaticUpdateChecks = automaticUpdateChecks;
        launchMenu.load(ps);
        ps.appVersion = build::version;
        ps.buildId = build::id;
        ps.buildTarget = "Linux ARM64 / glibc 2.35";
        ps.openVRVersion = std::to_string(vr::k_nSteamVRVersionMajor) + "." +
                           std::to_string(vr::k_nSteamVRVersionMinor) + "." +
                           std::to_string(vr::k_nSteamVRVersionBuild);
        const char *runtimeVersion = vr::VRSystem()->GetRuntimeVersion();
        ps.runtimeVersion = runtimeVersion ? runtimeVersion : "Unavailable";
        ps.loggingStatus = loggingNotice();
        ps.debugLogging = sessionlog::detailed();
        sessionlog::info("runtime", "SteamVR=" + ps.runtimeVersion + " OpenVR SDK=" +
                                        ps.openVRVersion + " target=" + ps.buildTarget);
        char headsetModel[256]{};
        vr::ETrackedPropertyError modelError;
        vr::VRSystem()->GetStringTrackedDeviceProperty(0, vr::Prop_ModelNumber_String, headsetModel,
                                                       sizeof(headsetModel), &modelError);
        if (modelError == vr::TrackedProp_Success)
            sessionlog::info("runtime", "Headset model=" + std::string(headsetModel));
        sessionlog::info("runtime", "Compositor tracking space=" +
                                        std::to_string(vr::VRCompositor()->GetTrackingSpace()));
        updatePanelState(session, ps);
        ps.notice = loadedPreferences.problem;
        panel.render(ps);
        PanelState drawn = ps;
        if (!texture.update(overlay, panel.pixels.data(), error))
            throw std::runtime_error(error);
        log(std::string("Dashboard ready. Saved movement choice: ") +
            (session.movementRequested() ? "On; acquiring current playspace." : "Off."));
        auto showDashboard = [&] {
            o->ShowDashboard(appKey);
            log("Requested Frame Advanced Settings dashboard; waiting for overlay visibility.");
        };
        if (!background) showDashboard();
        Clock::time_point last = Clock::now(), renderAt = last, statusAt = last, watchAt = last;
        Clock::time_point recenterLogAt = last;
        Clock::time_point noticeUntil = last + std::chrono::seconds(12);
        Command pressed = Command::None;
        bool inputErrorLogged = false;
        bool previousTracked = false, previousPressed = false;
        uint32_t previousController = vr::k_unTrackedDeviceIndexInvalid;
        while (!stopping) {
            const auto now = Clock::now();
            const double dt = std::chrono::duration<double>(now - last).count();
            last = now;
            vr::VREvent_t event{};
            bool seatedReset = false, standingReset = false;
            while (vr::VRSystem()->PollNextEvent(&event, sizeof(event))) {
                if (event.eventType == vr::VREvent_Quit) {
                    vrQuit = true;
                    stopping = 1;
                }
                seatedReset |= event.eventType == vr::VREvent_SeatedZeroPoseReset;
                standingReset |= event.eventType == vr::VREvent_StandingZeroPoseReset;
            }
            if (!stopping && (seatedReset || standingReset) &&
                (session.enabled || session.needsRestore())) {
                const auto runtimeOrigin =
                    fromVr(vr::VRSystem()->GetRawZeroPoseToStandingAbsoluteTrackingPose());
                const bool known = session.recenter(runtimeOrigin);
                if (!known || now >= recenterLogAt) {
                    log(std::string("Recenter notification (seated=") +
                        (seatedReset ? "yes" : "no") +
                        ", standing=" + (standingReset ? "yes" : "no") + "): " +
                        (known ? "standing data matches baseline/our preview; kept movement state."
                               : session.status));
                    recenterLogAt = now + std::chrono::seconds(2);
                }
            }
            while (o->PollNextOverlayEvent(overlay, &event, sizeof(event))) {
                if (event.eventType == vr::VREvent_ScrollDiscrete && ps.page == PanelPage::Settings) {
                    ps.settingsScroll = std::clamp(ps.settingsScroll - int(event.data.scroll.ydelta * 78),
                                                   0, Panel::settingsScrollMax);
                    pressed = Command::None;
                    ps.hover = Command::None;
                    continue;
                }
                if (event.eventType == vr::VREvent_OverlayShown)
                    log("Frame Advanced Settings dashboard is visible.");
                if (event.eventType == vr::VREvent_OverlayHidden)
                    log("Frame Advanced Settings dashboard is hidden.");
                if (event.eventType == vr::VREvent_OverlayClosed)
                    stopping = 1;
                if (event.eventType == vr::VREvent_FocusLeave ||
                    event.eventType == vr::VREvent_OverlayHidden) {
                    pressed = Command::None;
                    ps.hover = Command::None;
                }
                if (event.eventType == vr::VREvent_MouseMove) {
                    ps.hover =
                        panel.hit(event.data.mouse.x, Panel::height - event.data.mouse.y, ps);
                    continue;
                }
                if (event.eventType != vr::VREvent_MouseButtonDown &&
                    event.eventType != vr::VREvent_MouseButtonUp)
                    continue;
                if (event.data.mouse.button != vr::VRMouseButton_Left)
                    continue;
                auto hit =
                    panel.hit(event.data.mouse.x, Panel::height - event.data.mouse.y, ps);
                ps.hover = hit;
                if (event.eventType == vr::VREvent_MouseButtonDown)
                    pressed = hit;
                if (event.eventType == vr::VREvent_MouseButtonUp) {
                    if (pressed != hit) {
                        pressed = Command::None;
                        continue;
                    }
                    if (hit != Command::None)
                        ps.notice.clear();
                    switch (hit) {
                    case Command::PageSpaceDrag:
                    case Command::PageLaunchMenu:
                    case Command::PageMicrophone:
                    case Command::PageSettings:
                        ps.page = hit == Command::PageSettings ? PanelPage::Settings
                                  : hit == Command::PageMicrophone ? PanelPage::Microphone
                                  : hit == Command::PageLaunchMenu ? PanelPage::LaunchMenu
                                                                 : PanelPage::SpaceDrag;
                        ps.hover = Command::None;
                        break;
                    case Command::SettingsUp:
                    case Command::SettingsDown:
                        ps.settingsScroll = std::clamp(ps.settingsScroll + (hit == Command::SettingsDown ? 156 : -156),
                                                       0, Panel::settingsScrollMax);
                        ps.hover = Command::None;
                        break;
                    case Command::AutoUpdateOff:
                    case Command::AutoUpdateOn:
                    case Command::ResetAutoUpdate:
                        automaticUpdateChecks = hit != Command::AutoUpdateOff;
                        ps.automaticUpdateChecks = automaticUpdateChecks;
                        break;
                    case Command::DebugLoggingOff:
                    case Command::DebugLoggingOn:
                    case Command::ResetDebugLogging:
                        sessionlog::setDetailed(hit == Command::ResetDebugLogging
                                                    ? drag::Preferences{}.detailedLogging
                                                    : hit == Command::DebugLoggingOn);
                        ps.debugLogging = sessionlog::detailed();
                        break;
                    case Command::ExportLogs:
                        logExport.start(root);
                        ps.exportStatus = logExport.status;
                        break;
                    case Command::Disable:
                        session.requestMovement(false);
                        break;
                    case Command::Enable:
                        session.requestMovement(true);
                        break;
                    case Command::Bindings: {
                        session.engine.release();
                        const auto result = vr::VRInput()->OpenBindingUI(
                            appKey, input.set, vr::k_ulInvalidInputValueHandle, false);
                        if (result == vr::VRInputError_None) {
                            log("Requested in-headset binding editor for Frame Advanced Settings.");
                        } else {
                            ps.notice = "Could not open bindings. Use SteamVR Settings > "
                                        "Controllers > Manage bindings.";
                            log("OpenBindingUI failed (VRInputError " +
                                std::to_string(static_cast<int>(result)) + ").");
                        }
                        noticeUntil = now + std::chrono::seconds(12);
                        break;
                    }
                    case Command::Reset:
                        log("Reset requested from the dashboard.");
                        session.reset();
                        break;
                    case Command::ResetDirection:
                    case Command::ResetGain:
                    case Command::ResetLimit:
                    case Command::ResetGravity: {
                        resetSpaceDragSetting(hit, session);
                        const char *row = hit == Command::ResetDirection ? "movement direction"
                                          : hit == Command::ResetGain ? "drag speed"
                                          : hit == Command::ResetLimit ? "distance limit" : "gravity";
                        sessionlog::info("preferences", std::string("Reset ") + row + " to defaults");
                        break;
                    }
                    case Command::Height:
                    case Command::XYZ:
                        session.engine.xyz = hit == Command::XYZ;
                        session.engine.release();
                        break;
                    case Command::Slower:
                        session.engine.gain = std::max(.25, session.engine.gain - .25);
                        session.engine.release();
                        break;
                    case Command::Faster:
                        session.engine.gain = std::min(5., session.engine.gain + .25);
                        session.engine.release();
                        break;
                    case Command::LimitOff:
                    case Command::LimitOn:
                        session.setDistanceLimit(hit == Command::LimitOn,
                                                 session.engine.distanceLimit.meters());
                        break;
                    case Command::LimitSmaller:
                    case Command::LimitLarger:
                        session.setDistanceLimit(session.engine.distanceLimit.enabled,
                                                 session.engine.distanceLimit.meters() +
                                                     (hit == Command::LimitLarger ? .5 : -.5));
                        break;
                    case Command::GravityOff:
                    case Command::GravityOn:
                        session.gravity.setEnabled(hit == Command::GravityOn);
                        break;
                    case Command::GravityWeaker:
                        session.gravity.setStrength(session.gravity.strength() - 1.0);
                        break;
                    case Command::GravityStronger:
                        session.gravity.setStrength(session.gravity.strength() + 1.0);
                        break;
                    default:
                        settings.command(hit, ps);
                        microphone.command(hit, ps);
                        launchMenu.command(hit, ps);
                        break;
                    }
                    pressed = Command::None;
                    renderAt = now;
                }
            }
            if (stopping)
                break;
            microphone.poll(ps, o->IsOverlayVisible(overlay) && ps.page == PanelPage::Microphone);
            settings.poll(ps, o->IsOverlayVisible(overlay) &&
                              ps.page == PanelPage::Settings);
            if (launchMenu.poll(ps)) {
                // A refreshed list must not turn an in-flight pointer click into
                // a visibility change on a different shortcut in the same slot.
                pressed = Command::None;
                ps.hover = Command::None;
            }
            drag::Vec hand;
            bool tracked = false;
            bool inputOk = input.poll(hand, tracked);
            const bool dashboard = o->IsDashboardVisible();
            if (tracked != previousTracked || input.controller != previousController ||
                input.d.bState != previousPressed) {
                std::ostringstream transition;
                transition << std::boolalpha << "tracked=" << tracked
                           << " controller=" << input.controller << " pressed=" << input.d.bState
                           << " active=" << input.d.bActive << " dashboard=" << dashboard
                           << " hand_result=" << input.controllerPose.eTrackingResult
                           << " head_result=" << input.headPose.eTrackingResult;
                sessionlog::info("input", transition.str());
                previousTracked = tracked;
                previousController = input.controller;
                previousPressed = input.d.bState;
            }
            if (dt > .05)
                sessionlog::warn("timing", "Main loop interval_ms=" + std::to_string(dt * 1000.));
            spaceAccess.recordMotion(input.controller, inputOk, input.d, input.controllerPose,
                                     input.headPose, session);
            if (!inputOk) {
                session.engine.release();
                if (!inputErrorLogged) {
                    sessionlog::warn("input",
                                     "Input read failed; waiting for controller bindings.");
                    inputErrorLogged = true;
                }
            } else
                inputErrorLogged = false;
            if (now >= watchAt) {
                if ((session.enabled || session.needsRestore() || session.startupEnablePending()) &&
                    vr::VRApplications()->GetApplicationProcessId("openvr.tool.steamvr_room_setup"))
                    session.abandon("Room setup opened. Movement paused.");
                else
                    session.watch(!dashboard && inputOk && input.d.bActive && !input.d.bState);
                watchAt = now + std::chrono::milliseconds(100);
            }
            if (inputOk && input.r.bActive && input.r.bState && input.r.bChanged) {
                log("Reset requested by the controller binding.");
                session.reset();
            }
            if (inputOk && input.g.bActive && input.g.bState && input.g.bChanged)
                session.gravity.setEnabled(!session.gravity.enabled());
            if (now >= nextPreferencesSave) {
                savePreferenceChanges();
                if (!preferencesProblem.empty()) {
                    ps.notice = "Could not save preferences. Retrying; see the app log.";
                    noticeUntil = now + std::chrono::seconds(12);
                } else if (ps.notice == "Could not save preferences. Retrying; see the app log.")
                    ps.notice.clear();
            }
            session.updateMotion(!dashboard && inputOk && input.d.bActive, input.d.bState, tracked,
                                 hand, dt);
            if (ps.enabled != session.enabled || ps.status != session.status) {
                const auto message = "Movement " +
                                     std::string(session.enabled ? "enabled: " : "paused: ") +
                                     session.status;
                if (session.statusAttention)
                    sessionlog::warn("movement", message);
                else
                    sessionlog::info("movement", message);
            }
            updatePanelState(session, ps);
            if (!ps.notice.empty() && now >= noticeUntil)
                ps.notice.clear();
            ps.pressed = pressed;
            if (now >= renderAt && o->IsOverlayVisible(overlay) && !(ps == drawn)) {
                panel.render(ps);
                if (!texture.update(overlay, panel.pixels.data(), error))
                    throw std::runtime_error(error);
                drawn = ps;
                renderAt = now + std::chrono::milliseconds(33);
            }
            if (now >= statusAt) {
                logExport.poll();
                ps.exportStatus = logExport.status;
                ps.loggingStatus = loggingNotice();
                // ExecReload drops this request so Launch program (+) can reopen
                // the existing panel without restarting or resetting movement.
                std::error_code requestError;
                if (fs::remove(root / "show-dashboard", requestError))
                    showDashboard();
                if (requestError)
                    log("Could not read dashboard request: " + requestError.message());
                statusFile(ps, {dashboard, inputOk, tracked, input.d.bState, input.d.bActive,
                                input.r.bActive, session.resynchronizing,
                                session.resynchronizing && session.needsRestore()});
                statusAt = now + std::chrono::milliseconds(500);
            }
            std::this_thread::sleep_until(now + std::chrono::milliseconds(10));
        }
        cleanup();
        return 0;
    } catch (const std::exception &ex) {
        sessionlog::error("runtime", ex.what());
        cleanup();
        return 2;
    }
}
int main(int argc, char **argv) {
    try {
        if (argc == 3 && (std::strcmp(argv[1], "--voice-record-helper") == 0 ||
                         std::strcmp(argv[1], "--voice-play-helper") == 0))
            return voiceclip::helper(std::strcmp(argv[1], "--voice-record-helper") == 0, argv[2]);
        if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
            std::printf("Frame Advanced Settings %s\n", build::full);
            return 0;
        }
        if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
            std::printf("Frame Advanced Settings %s\n", build::full);
            std::puts("  --version  Print build identity without starting OpenVR.\n"
                      "  --background  Start without opening the dashboard.\n"
                      "  --probe  Read-only runtime/playspace diagnostic; "
                      "no overlay or movement.\n  no args  Native dashboard; restores saved "
                      "preferences, including the movement on/off choice.");
            return 0;
        }
        const bool probeOnly = argc == 2 && std::strcmp(argv[1], "--probe") == 0;
        const bool background = argc == 2 && std::strcmp(argv[1], "--background") == 0;
        if (argc != 1 && !probeOnly && !background) {
            std::fprintf(stderr, "Unknown argument. Use --help.\n");
            return 1;
        }
        auto root = fs::canonical("/proc/self/exe").parent_path();
        std::string loaderError;
        if (probeOnly) {
            if (!loadOpenVR(root, loaderError)) {
                log(loaderError);
                return 2;
            }
            return probe();
        }
        fs::create_directories(stateDirectory());
        int lock = ::open((stateDirectory() / "instance.lock").c_str(),
                          O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
            log("Cannot acquire lock; another instance may be running.");
            return 3;
        }
        sessionlog::start(stateDirectory() / "logs", build::full);
        log(std::string("Starting ") + build::full + "; pid=" + std::to_string(getpid()));
        struct utsname platform{};
        if (uname(&platform) == 0)
            sessionlog::info("platform", std::string(platform.sysname) + " " + platform.release +
                                             " " + platform.machine);
        if (!loadOpenVR(root, loaderError)) {
            sessionlog::error("startup", loaderError);
            sessionlog::shutdown();
            ::close(lock);
            return 2;
        }
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        auto result = run(root, background);
        sessionlog::info("lifecycle", "Dashboard exit code=" + std::to_string(result));
        sessionlog::shutdown();
        ::close(lock);
        return result;
    } catch (const std::exception &ex) {
        sessionlog::error("fatal", ex.what());
        sessionlog::shutdown();
        return 2;
    }
}
