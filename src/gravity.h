#pragma once
#include "drag.h"

namespace drag {
// Release momentum and a vertical fall relative to the captured standing floor.
// No game-world collision queries or persistent floor edits.
class Gravity {
    Vec velocity_;
    bool enabled_ = false;
    double strength_ = 9.8;

  public:
    static constexpr double maxSpeed = 3.0;
    bool enabled() const { return enabled_; }
    double strength() const { return strength_; }
    double speed() const { return length(velocity_); }
    Vec velocity() const { return velocity_; }
    void stop() { velocity_ = {}; }
    void launch(Vec velocity) {
        stop();
        if (!enabled_ || !finite(velocity))
            return;
        const double speed = length(velocity);
        if (!std::isfinite(speed))
            return;
        velocity_ = speed > maxSpeed ? velocity * (maxSpeed / speed) : velocity;
    }
    void setEnabled(bool enabled) {
        enabled_ = enabled;
        stop();
    }
    void setStrength(double strength) {
        if (std::isfinite(strength))
            strength_ = std::clamp(strength, 1.0, 20.0);
        stop();
    }
    bool step(Vec &offset, double dt, bool suspended, const DistanceLimit &limit = {}) {
        if (!enabled_ || suspended || !finite(offset) || !std::isfinite(dt) || dt <= 0 || dt > .1 ||
            offset.y < 0 || (offset.y == 0 && velocity_.y <= 0)) {
            stop();
            return false;
        }
        // Integrate to terminal downward speed and the floor exactly, including
        // an initial upward throw. Horizontal travel ends at the landing time.
        const double toTerminal = (velocity_.y + maxSpeed) / strength_;
        const double yAtTerminal =
            offset.y + velocity_.y * toTerminal - .5 * strength_ * toTerminal * toTerminal;
        double toFloor;
        if (yAtTerminal > 0) {
            toFloor = toTerminal + yAtTerminal / maxSpeed;
        } else {
            const double root = std::sqrt(velocity_.y * velocity_.y + 2 * strength_ * offset.y);
            toFloor = velocity_.y >= 0 ? (velocity_.y + root) / strength_
                                       : 2 * offset.y / (root - velocity_.y);
        }
        const double travelTime = std::min(dt, toFloor);
        const double accelerating = std::min(travelTime, toTerminal);
        Vec next =
            offset + Vec{velocity_.x * travelTime,
                         velocity_.y * accelerating - .5 * strength_ * accelerating * accelerating -
                             maxSpeed * (travelTime - accelerating),
                         velocity_.z * travelTime};
        velocity_.y = std::max(-maxSpeed, velocity_.y - strength_ * travelTime);
        if (toFloor <= dt) {
            next.y = 0;
            stop();
        }
        const Vec limited = limit.constrain(offset, next);
        if (length(limited - next) > 0) {
            next = limited;
            stop();
        }
        const bool changed = length(next - offset) > 0;
        offset = next;
        return changed;
    }
};
} // namespace drag
