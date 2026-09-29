-- EQ follows capture activity. No audio is recorded or routed by this script.

local state = {
  stream_count = 0,
}

local filters_om = ObjectManager {
  Interest {
    type = "node",
    Constraint { "steamos.mic_filter", "=", "true", type = "pw" },
  }
}

local metadata_om = ObjectManager {
  Interest {
    type = "metadata",
    Constraint { "metadata.name", "=", "filters" },
  }
}

local function setting_for (node)
  local name = node.properties ["node.name"] or ""
  if name:find ("^echo_cancel") then
    return "frame-advanced-settings.mic-echo-cancel"
  end
  if name:find ("^ns_") or name:find ("^dsp_") then
    return "frame-advanced-settings.mic-noise-suppression"
  end
  return nil
end

local function apply_node (metadata, node)
  local disabled = state.stream_count == 0
  local key = setting_for (node)
  if not disabled and key ~= nil and not Settings.get_boolean (key) then
    disabled = true
  end
  local node_id = node.properties ["object.id"]
  metadata:set (node_id, "filter.smart.disabled", "Spa:String:JSON",
      disabled and "true" or "false")
end

local function apply_all ()
  local metadata = metadata_om:lookup ()
  if metadata == nil then
    return
  end
  for node in filters_om:iterate () do
    apply_node (metadata, node)
  end
end

filters_om:connect ("object-added", function (_, node)
  local metadata = metadata_om:lookup ()
  if metadata ~= nil then
    apply_node (metadata, node)
  end
end)
metadata_om:connect ("object-added", function ()
  apply_all ()
end)

filters_om:activate ()
metadata_om:activate ()

Settings.subscribe ("frame-advanced-settings.mic-*", function ()
  apply_all ()
end)

SimpleEventHook {
  name = "frame-advanced-settings-mic-stream-added",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "node-added" },
      Constraint { "media.class", "=", "Stream/Input/Audio" },
      Constraint { "node.virtual", "!", "true" },
    }
  },
  execute = function (event)
    state.stream_count = state.stream_count + 1
    if state.stream_count == 1 then
      apply_all ()
    end
  end
}:register ()

SimpleEventHook {
  name = "frame-advanced-settings-mic-stream-removed",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "node-removed" },
      Constraint { "media.class", "=", "Stream/Input/Audio" },
      Constraint { "node.virtual", "!", "true" },
    }
  },
  execute = function (event)
    state.stream_count = math.max (0, state.stream_count - 1)
    if state.stream_count == 0 then
      apply_all ()
    end
  end
}:register ()
