#pragma once
#include "panel.h"
#include "preferences.h"

// Reset only this row, preserving position, module state and other preferences.
inline bool resetSpaceDragSetting(Command command, drag::Session &session) {
    const drag::Preferences defaults;
    switch (command) {
    case Command::ResetDirection:
        session.engine.xyz = defaults.xyz;
        session.engine.release();
        break;
    case Command::ResetGain:
        session.engine.gain = defaults.gain;
        session.engine.release();
        break;
    case Command::ResetLimit:
        session.setDistanceLimit(defaults.distanceLimit, defaults.distanceLimitMeters);
        break;
    case Command::ResetGravity:
        session.gravity.setEnabled(defaults.gravity);
        session.gravity.setStrength(defaults.gravityStrength);
        break;
    default:
        return false;
    }
    return true;
}
