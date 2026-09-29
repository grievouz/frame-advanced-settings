#pragma once
#include <algorithm>
#include <cmath>
#include <deque>

namespace drag {
struct Vec {
    double x = 0, y = 0, z = 0;
};
inline Vec operator+(Vec a, Vec b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec operator-(Vec a, Vec b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec operator*(Vec a, double b) {
    return {a.x * b, a.y * b, a.z * b};
}
inline double length(Vec a) {
    return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}
inline bool finite(Vec a) {
    return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}
struct Transform {
    double r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    Vec t;
    Vec rotate(Vec p) const {
        return {r[0][0] * p.x + r[0][1] * p.y + r[0][2] * p.z,
                r[1][0] * p.x + r[1][1] * p.y + r[1][2] * p.z,
                r[2][0] * p.x + r[2][1] * p.y + r[2][2] * p.z};
    }
    Vec unrotate(Vec p) const {
        return {r[0][0] * p.x + r[1][0] * p.y + r[2][0] * p.z,
                r[0][1] * p.x + r[1][1] * p.y + r[2][1] * p.z,
                r[0][2] * p.x + r[1][2] * p.y + r[2][2] * p.z};
    }
    Vec point(Vec p) const { return rotate(p) + t; }
    Vec inversePoint(Vec p) const { return unrotate(p - t); }
    Transform inverse() const {
        Transform result;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                result.r[i][j] = r[j][i];
        result.t = unrotate(t) * -1;
        return result;
    }
    bool valid() const {
        if (!finite(t))
            return false;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                double v = 0;
                for (int k = 0; k < 3; k++)
                    v += r[k][i] * r[k][j];
                if (!std::isfinite(v) || std::abs(v - (i == j ? 1. : 0.)) > .002)
                    return false;
            }
        double det = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) -
                     r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
                     r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
        return std::abs(det - 1) < .002;
    }
};
inline bool near(const Transform &a, const Transform &b, double eps = .0005) {
    if (!a.valid() || !b.valid() || length(a.t - b.t) > eps)
        return false;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            if (std::abs(a.r[i][j] - b.r[i][j]) > eps)
                return false;
    return true;
}
// Public offset is the user's movement in standing space, opposite to hand drag.
inline Transform withOffset(Transform baseline, Vec offset) {
    baseline.t = baseline.t - baseline.rotate(offset);
    return baseline;
}
inline Vec keepBoundaryFixed(Vec point, const Transform &oldPose, const Transform &newPose) {
    return newPose.inversePoint(oldPose.point(point));
}

class DistanceLimit {
    double meters_ = 2.0;

  public:
    bool enabled = true;
    double meters() const { return meters_; }
    void setMeters(double value) {
        if (std::isfinite(value))
            meters_ = std::clamp(value, .5, 100.0);
    }
    Vec constrain(Vec from, Vec to) const {
        if (!enabled)
            return to;
        // Lowering the limit never teleports an existing offset. Until back
        // inside, allow movement toward the origin but no additional distance.
        const double radius = std::max(meters_, length(from));
        if (length(to) <= radius)
            return to;
        // Clip along the actual movement segment, preserving locked axes.
        const Vec delta = to - from;
        const double a = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
        const double b = 2 * (from.x * delta.x + from.y * delta.y + from.z * delta.z);
        const double c =
            std::min(0.0, from.x * from.x + from.y * from.y + from.z * from.z - radius * radius);
        const double fraction =
            a > 0 ? std::clamp((-b + std::sqrt(b * b - 4 * a * c)) / (2 * a), 0.0, 1.0) : 0;
        return from + delta * fraction;
    }
};

class Engine {
    bool held_ = false, requireRelease_ = true;
    Vec handAnchor_, previous_, offsetAnchor_;
    struct Motion {
        Vec delta;
        double dt;
    };
    std::deque<Motion> recentMotion_;
    double motionTime_ = 0;
    Vec releasedVelocity_;

    void recordMotion(Vec delta, double dt) {
        // Average the last 60 ms, including stationary samples. This smooths
        // pose noise without retaining a throw after the hand has stopped.
        constexpr double window = .06;
        recentMotion_.push_back({delta, dt});
        motionTime_ += dt;
        while (recentMotion_.size() > 1 &&
               (motionTime_ - recentMotion_.front().dt >= window || recentMotion_.size() > 128)) {
            motionTime_ -= recentMotion_.front().dt;
            recentMotion_.pop_front();
        }
        if (motionTime_ > window) {
            auto &first = recentMotion_.front();
            const double keep = first.dt - (motionTime_ - window);
            first.delta = first.delta * (keep / first.dt);
            first.dt = keep;
            motionTime_ = window;
        }
    }

  public:
    Vec offset;
    bool xyz = false;
    double gain = 1.0;
    DistanceLimit distanceLimit;
    void release() {
        held_ = false;
        requireRelease_ = true;
        recentMotion_.clear();
        motionTime_ = 0;
        releasedVelocity_ = {};
    }
    void reset() {
        offset = {};
        release();
    }
    bool held() const { return held_; }
    Vec releasedVelocity() const { return releasedVelocity_; }
    // Raw tracking poses do not include the offset we just applied. This avoids feedback.
    bool sample(bool pressed, bool tracked, Vec rawHand, const Transform &baseline, double dt) {
        releasedVelocity_ = {};
        if (!tracked || !finite(rawHand) || !baseline.valid() || !std::isfinite(dt) || dt > .10 ||
            dt <= 0) {
            release();
            return false;
        }
        if (!pressed) {
            if (held_ && motionTime_ >= .02) {
                for (const auto &motion : recentMotion_)
                    releasedVelocity_ = releasedVelocity_ + motion.delta;
                releasedVelocity_ = releasedVelocity_ * (1.0 / motionTime_);
                if (length(releasedVelocity_) < .05)
                    releasedVelocity_ = {};
            }
            held_ = false;
            requireRelease_ = false;
            recentMotion_.clear();
            motionTime_ = 0;
            return false;
        }
        if (requireRelease_)
            return false;
        if (!held_) {
            held_ = true;
            handAnchor_ = previous_ = rawHand;
            offsetAnchor_ = offset;
            return false;
        }
        if (length(rawHand - previous_) > .25) {
            release();
            return false;
        }
        previous_ = rawHand;
        Vec delta = baseline.unrotate(rawHand - handAnchor_);
        if (!xyz)
            delta = {0, delta.y, 0};
        Vec next = offsetAnchor_ - delta * gain;
        if (!finite(next)) {
            release();
            return false;
        }
        next = distanceLimit.constrain(offset, next);
        const bool changed = length(next - offset) >= .0001;
        recordMotion(changed ? next - offset : Vec{}, dt);
        if (!changed)
            return false;
        offset = next;
        return true;
    }
};
} // namespace drag
