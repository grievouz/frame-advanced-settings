-- Run with Lua 5.4: lua tests/microphone_hook_test.lua
-- Exercise the real packaged script against a small WirePlumber API fixture.
local managers, hooks, values, filters = {}, {}, {}, {}
local settings = {
  ["frame-advanced-settings.mic-echo-cancel"] = true,
  ["frame-advanced-settings.mic-noise-suppression"] = true,
}
local metadata = {}
function metadata:set(id, key, kind, value)
  assert(key == "filter.smart.disabled" and kind == "Spa:String:JSON")
  assert(value == "true" or value == "false")
  values[id] = value == "true"
end
function Constraint(value) return value end
function Interest(value) return value end
function EventInterest(value) return value end
function ObjectManager(definition)
  local manager = { definition = definition, callbacks = {}, objects = {} }
  function manager:connect(event, callback) self.callbacks[event] = callback end
  function manager:activate() self.active = true end
  function manager:lookup() return self.objects[1] end
  function manager:iterate()
    local i = 0
    return function() i = i + 1; return self.objects[i] end
  end
  managers[#managers + 1] = manager
  return manager
end
local on_setting
Settings = {
  get_boolean = function(key) assert(settings[key] ~= nil); return settings[key] end,
  subscribe = function(pattern, callback)
    assert(pattern == "frame-advanced-settings.mic-*"); on_setting = callback
  end,
}
function SimpleEventHook(definition)
  function definition:register()
    local interest = self.interests[1]
    assert(interest[2][1] == "media.class" and interest[2][3] == "Stream/Input/Audio")
    assert(interest[3][1] == "node.virtual" and interest[3][2] == "!" and interest[3][3] == "true")
    hooks[interest[1][3]] = self.execute
  end
  return definition
end
dofile("contrib/wireplumber/frame-advanced-settings-mic.lua")
assert(#managers == 2 and managers[1].active and managers[2].active)
assert(managers[1].definition[1][1][1] == "steamos.mic_filter")
assert(managers[1].definition[1][1].type == "pw")
local function node(id, name)
  local item = { properties = { ["object.id"] = id, ["node.name"] = name } }
  table.insert(managers[1].objects, item)
  managers[1].callbacks["object-added"](managers[1], item)
end
node(1, "echo_cancel_capture") -- Missing metadata is normal during startup.
node(2, "ns_capture")
node(3, "dsp_capture")
node(4, "eq_capture")
managers[2].objects = {metadata}
managers[2].callbacks["object-added"]()
for i=1,4 do assert(values[i] == true, "idle filters disabled") end
hooks["node-added"]({})
for i=1,4 do assert(values[i] == false, "stock defaults while capturing") end
settings["frame-advanced-settings.mic-noise-suppression"] = false
on_setting()
assert(not values[1] and values[2] and values[3] and not values[4])
settings["frame-advanced-settings.mic-echo-cancel"] = false
on_setting()
assert(values[1] and not values[4], "echo switch does not disable EQ")
hooks["node-added"]({})
hooks["node-removed"]({})
assert(not values[4], "remaining capture stream keeps filters active")
node(5, "echo_cancel_playback")
assert(values[5], "late filter uses saved switch")
settings["frame-advanced-settings.mic-echo-cancel"] = true
settings["frame-advanced-settings.mic-noise-suppression"] = true
on_setting()
for i=1,5 do assert(not values[i], "reset enables all filters while capturing") end
hooks["node-removed"]({})
for i=1,5 do assert(values[i], "last stream removal disables idle filters") end
hooks["node-removed"]({}) -- Count is clamped instead of becoming negative.
hooks["node-added"]({})
assert(not values[4])
managers[2].objects = {}
on_setting() -- Metadata disappearing must not crash the session manager.
managers[2].objects = {metadata}
managers[2].callbacks["object-added"]()
assert(not values[4])
print("Microphone hook: stream lifetime, filter switches, late nodes and metadata recovery passed.")
