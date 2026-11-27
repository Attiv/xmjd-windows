-- Rank legal 星空键道 candidates from local commit-context choices.
-- Only code/context/candidate hashes are persisted; entered text is not stored.
local MAX_HISTORY_ENTRIES = 4096
local CORRECTION_TYPE = "context_correction"
-- A correction ignored this many times in a row (no pick in between) stops showing.
local IGNORES_TO_SUPPRESS = 3
-- Each past pick lowers the required score margin, up to MAX_PICK_BONUS picks.
local MARGIN_PER_PICK = 0.6
local MAX_PICK_BONUS = 3

-- Scores are natural-log probabilities, so a lead of log(1.4) means "1.4x as likely".
local function thresholds(context)
    local strength = tonumber(context:get_property("xmjd6_msime_strength")) or 35
    strength = math.max(0, math.min(100, strength)) / 100
    -- The lead over the runner-up is its own setting, in tenths (14 = 1.4x).
    local lead = tonumber(context:get_property("xmjd6_msime_lead")) or 14
    lead = math.max(10, math.min(30, lead)) / 10
    return 4.6 - 3.5 * strength, math.log(lead), 1.5 - strength
end

local function model_scores(context, code)
    local expected = "msime2\t" .. context:get_property("xmjd6_msime_epoch") .. "\t" .. code
    local data = context:get_property("xmjd6_msime_scores")
    local header, body = data:match("^([^\n]+)\n(.*)$")
    if header ~= expected then return {}, {}, {} end
    local scores, corrections, legal = {}, {}, {}
    for row in body:gmatch("[^\n]+") do
        local kind, text, variant, number, gain_value = row:match("^([LC])\t([^\t]+)\t([a-z]+)\t([^\t]+)\t([^\t]+)$")
        local score, gain = tonumber(number), tonumber(gain_value)
        if score and gain and score == score and gain == gain
            and math.abs(score) < 100000 and math.abs(gain) < 100000 then
            scores[text] = score
            if kind == "C" then
                corrections[#corrections + 1] = { text = text, code = variant, score = score, gain = gain }
            else
                legal[text] = true
            end
        end
    end
    return scores, corrections, legal
end

local handlers = setmetatable({}, { __mode = "k" })

local function is_word_codepoint(code)
    return (code >= 0x3400 and code <= 0x9fff)
        or (code >= 0xf900 and code <= 0xfaff)
        or (code >= 0x20000 and code <= 0x3134f)
        or (code >= 48 and code <= 57)
        or (code >= 65 and code <= 90)
        or (code >= 97 and code <= 122)
end

local function recent_context(context)
    if context:get_property("xmjd6_context_features_enabled") == "0" then
        return {}
    end
    local text
    local ok, supplied = pcall(function()
        return context:get_property("xmjd6_before_cursor")
    end)
    if ok and type(supplied) == "string" and string.byte(supplied, 1) == 1 then
        text = string.sub(supplied, 2)
    else
        local history = context and context.commit_history
        if not history then return {} end
        ok, text = pcall(function() return history:latest_text() end)
        if not ok then return {} end
    end
    if type(text) ~= "string" or text == "" then return {} end

    local count = utf8.len(text)
    if not count then return {} end
    local suffixes = {}
    for length = 1, math.min(4, count) do
        local start = utf8.offset(text, -length)
        if not start then break end
        local suffix = string.sub(text, start)
        local valid = true
        for _, code in utf8.codes(suffix) do
            if not is_word_codepoint(code) then valid = false; break end
        end
        if not valid then break end
        suffixes[#suffixes + 1] = { suffix, length * length }
    end
    return suffixes
end

local function hash(value)
    local left, right = 5381, 52711
    for index = 1, #value do
        local byte = string.byte(value, index)
        left = (left * 33 + byte) % 2147483647
        right = (right * 65599 + byte) % 2147483629
    end
    return string.format("%08x%08x", left, right)
end

local function preference_key(code, suffix, candidate)
    return "v1_" .. hash(code .. "\0" .. suffix .. "\0" .. candidate)
end

-- Feedback on shown corrections, keyed by typed code + correction text (hashed).
-- shortcut: fb1_ keys are never trimmed; they only grow with distinct corrections
-- that were actually shown, and the settings reset erases them. Add ticks if needed.
local function feedback_key(code, text)
    return "fb1_" .. hash(code .. "\0" .. text)
end

local function read_feedback(env, code, text)
    local key, value = feedback_key(code, text), nil
    if env.db then
        local ok, stored = pcall(function() return env.db:fetch(key) end)
        if ok then value = stored end
    else
        value = env.memory[key]
    end
    local picks, ignores = tostring(value or ""):match("^(%d+),(%d+)$")
    return tonumber(picks) or 0, tonumber(ignores) or 0
end

local function write_feedback(env, code, text, picks, ignores)
    local key = feedback_key(code, text)
    local value = math.min(picks, 10000) .. "," .. math.min(ignores, 10000)
    if env.db then
        pcall(function() env.db:update(key, value) end)
    else
        env.memory[key] = value
    end
end

-- Any commit while a correction is shown is feedback: choosing it counts a pick
-- and clears the ignore streak; committing anything else counts an ignore.
local function learn_feedback(env, context)
    local shown = env.shown
    env.shown = nil
    if not shown or shown.code ~= context.input then return end
    local picks, ignores = read_feedback(env, shown.code, shown.text)
    local chosen = context:get_selected_candidate()
    if chosen and chosen.type == CORRECTION_TYPE and chosen.text == shown.text then
        write_feedback(env, shown.code, shown.text, picks + 1, 0)
    else
        write_feedback(env, shown.code, shown.text, picks, ignores + 1)
    end
end

-- The settings button bumps this token; erase every learned key once per new value.
local function apply_reset(env, context)
    local token = context:get_property("xmjd6_learning_reset")
    if token == "" or token == env.reset_token then return end
    env.reset_token = token
    env.memory = {}
    if not env.db then return end
    local ok, stored = pcall(function() return env.db:fetch("_reset_token") end)
    if ok and stored == token then return end
    local keys = {}
    pcall(function()
        local accessor = env.db:query("")
        if not accessor then return end
        for key in accessor:iter() do keys[#keys + 1] = key end
    end)
    for _, key in ipairs(keys) do pcall(function() env.db:erase(key) end) end
    pcall(function() env.db:update("_reset_token", token) end)
    env.tick = 0
end

local function candidate_score(env, code, contexts, candidate)
    if #contexts == 0 then return 0 end
    local score = 0
    if not env.db then
        for _, item in ipairs(contexts) do
            score = score + (env.memory[preference_key(code, item[1], candidate.text)] or 0) * item[2]
        end
        return score
    end
    for _, item in ipairs(contexts) do
        local ok, value = pcall(function()
            return env.db:fetch(preference_key(code, item[1], candidate.text))
        end)
        if ok then score = score + (tonumber(value) or 0) * item[2] end
    end
    return score
end

local function trim_history(env)
    if not env.db then return end
    local items = {}
    local ok = pcall(function()
        local accessor = env.db:query("")
        if not accessor then return end
        for key in accessor:iter() do
            if string.sub(key, 1, 3) == "v1_" then
                local tick = tonumber(env.db:fetch("_tick_" .. key)) or 0
                items[#items + 1] = { key = key, tick = tick }
            end
        end
    end)
    if not ok or #items <= MAX_HISTORY_ENTRIES then return end
    table.sort(items, function(left, right) return left.tick < right.tick end)
    for index = 1, #items - MAX_HISTORY_ENTRIES do
        local key = items[index].key
        pcall(function() env.db:erase(key) end)
        pcall(function() env.db:erase("_tick_" .. key) end)
    end
end

local function learn_selection(env, context)
    local candidate = context:get_selected_candidate()
    if not candidate then return end
    local code = context.input or ""
    if #code < 1 or #code > 6 or not string.match(code, "^[a-z]+$") then return end

    local segment = context.composition and context.composition:back()
    if not segment or not segment:has_tag("abc") then return end
    local contexts = recent_context(context)
    if #contexts == 0 then return end

    local previous_tick = env.tick
    for _, item in ipairs(contexts) do
        local key = preference_key(code, item[1], candidate.text)
        local ok, value = pcall(function() return env.db and env.db:fetch(key) end)
        local count = ok and (tonumber(value) or 0) or 0
        if env.db then
            env.tick = env.tick + 1
            pcall(function() env.db:update(key, tostring(math.min(count + 1, 10000))) end)
            pcall(function() env.db:update("_tick_" .. key, tostring(env.tick)) end)
        else
            env.memory[key] = math.min((env.memory[key] or 0) + 1, 10000)
        end
    end

    if env.db then
        pcall(function() env.db:update("_next_tick", tostring(env.tick)) end)
        if math.floor(previous_tick / 256) ~= math.floor(env.tick / 256) then
            trim_history(env)
        end
    end
end

local function filter(input, env)
    local context = env.engine.context
    -- The host app installs this filter only into 键道6-family schemas; no ID check here.
    if not context then
        for candidate in input:iter() do yield(candidate) end
        return
    end
    env.shown = nil
    apply_reset(env, context)

    local segment = context.composition and context.composition:back()
    local code = context.input or ""
    local short = #code == 2
    if not segment or not segment:has_tag("abc")
        or #code < 1 or #code > 6
        or not string.match(code, "^[a-z]+$") then
        for candidate in input:iter() do yield(candidate) end
        return
    end

    if context:get_property("xmjd6_context_features_enabled") ~= "1"
        or (short and context:get_property("xmjd6_msime_short_request") ~=
            context:get_property("xmjd6_msime_epoch") .. "\t" .. code) then
        for candidate in input:iter() do yield(candidate) end
        return
    end

    local contexts = recent_context(context)
    local neural, alternatives, legal = model_scores(context, code)
    env.neural_scores = neural
    if #alternatives == 0 then
        for candidate in input:iter() do yield(candidate) end
        return
    end
    local batch = {}
    for candidate in input:iter() do batch[#batch + 1] = candidate end

    -- Keep every ordinary candidate in the scheme's order. The neural model
    -- proposes a single correction; it never reshuffles legal words or aux codes.
    local ordered = batch

    -- Corrections are generated here, after the user's normal completion and
    -- simplification filters, so those filters cannot silently truncate the pool.
    local visible, first_legal, baseline = {}, nil, nil
    for position, candidate in ipairs(ordered) do
        visible[candidate.text] = true
        if (not short or legal[candidate.text]) and (candidate.type == "table"
            or (candidate.type == "completion" and not short) or candidate.type == "user_table") then
            first_legal = first_legal or position
            local score = neural[candidate.text]
            if score then baseline = math.max(baseline or score, score) end
        end
    end
    -- No length veto: four keys spell 2-char full codes and 4-char idioms alike
    -- (dhsd is 丢魂丧胆, a slip for 带伞). The sentence decides via score and gain.
    local eligible = {}
    for _, candidate in ipairs(alternatives) do
        if not visible[candidate.text] then
            eligible[#eligible + 1] = candidate
        end
    end
    table.sort(eligible, function(left, right)
        if left.score ~= right.score then return left.score > right.score end
        local a = candidate_score(env, code, contexts, left)
        local b = candidate_score(env, code, contexts, right)
        if a ~= b then return a > b end
        return left.text < right.text
    end)
    local suggestion
    local best, runner_up = eligible[1], eligible[2]
    if best and first_legal and (not short or first_legal == 1)
        and neural[ordered[first_legal].text] and baseline then
        local margin, separation, min_gain = thresholds(context)
        -- Only the top correction is judged: if it was ignored repeatedly, stay quiet
        -- for this code rather than offering the next guess.
        local picks, ignores = read_feedback(env, code, best.text)
        margin = math.max(0, margin - MARGIN_PER_PICK * math.min(picks, MAX_PICK_BONUS))
        if short then
            -- The experimental mode keeps a hard confidence floor, even if
            -- frequent picks lowered the normal threshold through learning.
            margin = math.max(margin, 4.6)
            separation = math.max(separation, math.log(2))
            min_gain = math.max(min_gain, 1.5)
        end
        if ignores < IGNORES_TO_SUPPRESS and best.score >= baseline + margin and best.gain >= min_gain
            and (not runner_up or best.score - runner_up.score >= separation) then
            suggestion = Candidate(CORRECTION_TYPE, segment.start, segment._end,
                best.text, "〔纠错〕")
            env.shown = { code = code, text = best.text }
        end
    end
    for position, candidate in ipairs(ordered) do
        yield(candidate)
        if suggestion and position == first_legal then yield(suggestion) end
    end
end

local function init(env)
    env.memory = {}
    env.db = nil
    env.tick = 0
    pcall(function()
        env.db = UserDb("xmjd6_contextual_ranker", "userdb")
        if env.db then
            env.db:open()
            env.tick = tonumber(env.db:fetch("_next_tick")) or 0
        end
    end)

    local context = env.engine.context
    if context and context.property_update_notifier then
        env.property_connection = context.property_update_notifier:connect(function(current, name)
            if (name == "xmjd6_before_cursor" or name == "xmjd6_msime_scores"
                or name == "xmjd6_context_features_enabled") and current:is_composing() then
                current:refresh_non_confirmed_composition()
            end
        end)
    end
    if context and context.select_notifier and not handlers[context] then
        local callback = function(current)
            learn_selection(env, current)
        end
        env.select_handler = callback
        env.select_connection = context.select_notifier:connect(callback)
        handlers[context] = callback
        -- Top-up commits never fire select_notifier, so feedback listens to commits.
        if context.commit_notifier then
            env.commit_connection = context.commit_notifier:connect(function(current)
                learn_feedback(env, current)
            end)
        end
    end
end

local function fini(env)
    local context = env.engine and env.engine.context
    if context and env.property_connection then
        pcall(function() env.property_connection:disconnect() end)
    end
    if context and env.select_connection then
        pcall(function() env.select_connection:disconnect() end)
        handlers[context] = nil
    end
    if env.commit_connection then
        pcall(function() env.commit_connection:disconnect() end)
    end
    if env.db then pcall(function() env.db:close() end) end
    env.memory = nil
end

return { init = init, func = filter, fini = fini }
