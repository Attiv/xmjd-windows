-- Run before scheme-specific top-up/fallback processors, without swallowing keys.
-- Only an explicit selection key/tap may commit a model correction.
local function process(key, env)
    if key:release() or key:ctrl() or key:alt() then return 2 end
    local code = key.keycode
    if not ((code >= 97 and code <= 122) or (code >= 65 and code <= 90)) then return 2 end
    local context = env.engine.context
    local selected = context:get_selected_candidate()
    if not selected or selected.type ~= "context_correction" then return 2 end
    local segment = context.composition and context.composition:back()
    if not segment or not segment.menu then return 2 end
    local first = segment.menu:get_candidate_at(0)
    if first and first.type ~= "context_correction" then segment.selected_index = 0 end
    return 2
end
return { func = process }
