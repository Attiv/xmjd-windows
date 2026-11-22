-- Build a bounded, code-constrained pool; the Mac app scores it off the key thread.
-- Every key touching this one on a physical ANSI keyboard (home row offset a
-- quarter key, bottom row three quarters). shortcut: non-QWERTY system layouts
-- still use this table; derive it from the active keyboard layout if that matters.
local NEIGHBORS = {
    q = "wa", w = "qeas", e = "wrsd", r = "etdf", t = "ryfg",
    y = "tugh", u = "yihj", i = "uojk", o = "ipkl", p = "ol",
    a = "qwsz", s = "weadzx", d = "ersfxc", f = "rtdgcv", g = "tyfhvb",
    h = "yugjbn", j = "uihknm", k = "iojlm", l = "opk", z = "asx",
    x = "sdzc", c = "dfxv", v = "fgcb", b = "ghvn", n = "hjbm",
    m = "jkn",
}
-- At most 6 keys x 6 neighbors + 5 swaps = 41 lookups per code.
local MAX_POOL = 96
local MAX_LEGAL = 5
local MAX_INSPECTED = 12

local function supported(candidate)
    if candidate.type ~= "table" and candidate.type ~= "completion"
        and candidate.type ~= "user_table" then return false end
    local text = candidate.text
    local length = utf8.len(text)
    return length and length > 0 and length <= 16 and not text:find("[%c]")
end

local function init(env)
    env.translator = Component.TableTranslator(env.engine, "translator", "table_translator")
    -- This is a separate translator instance. The user's completion switch still
    -- controls the ordinary menu; the hidden correction pool needs prefix hits.
    env.translator.enable_completion = true
end

local function translate(input, segment, env)
    local context = env.engine.context
    -- The host app installs this translator only into 键道6-family schemas; no ID check here.
    if not input or #input < 4 or #input > 6
        or not input:match("^[a-z]+$") or not segment:has_tag("abc")
        or segment.start ~= 0 or segment._end ~= #input
        or context:get_property("xmjd6_context_features_enabled") ~= "1" then
        return
    end
    local epoch = context:get_property("xmjd6_msime_epoch")
    if epoch == "" then return end
    local header = "msime2\t" .. epoch .. "\t" .. input
    -- Score refreshes re-run translators; the pool depends only on this header.
    if context:get_property("xmjd6_msime_pool"):sub(1, #header + 1) == header .. "\n" then return end
    local lines = { header }
    local seen, count = {}, 0
    local function append(kind, text, code)
        if count >= MAX_POOL or seen[text] then return end
        seen[text] = true
        count = count + 1
        lines[#lines + 1] = kind .. "\t" .. text .. "\t" .. code
    end
    local existing = env.translator:query(input, segment)
    if existing then
        for candidate in existing:iter() do
            if supported(candidate) then append("L", candidate.text, input) end
            if count >= MAX_LEGAL then break end
        end
    end
    -- A hidden pool must never turn a spelling with no normal candidate into an
    -- automatic first choice. At least one legal baseline is required for scoring.
    if count == 0 then
        context:set_property("xmjd6_msime_pool", "")
        return
    end
    -- One mistyped key (any surrounding key, any position) or two swapped keys.
    local codes = {}
    for position = 1, #input do
        local neighbors = NEIGHBORS[input:sub(position, position)] or ""
        for index = 1, #neighbors do
            codes[#codes + 1] = input:sub(1, position - 1) .. neighbors:sub(index, index)
                .. input:sub(position + 1)
        end
    end
    for position = 1, #input - 1 do
        local left, right = input:sub(position, position), input:sub(position + 1, position + 1)
        if left ~= right then
            codes[#codes + 1] = input:sub(1, position - 1) .. right .. left .. input:sub(position + 2)
        end
    end
    local variants = {}
    for _, code in ipairs(codes) do
        local hits = {}
        local translation = env.translator:query(code, segment)
        if translation then
            local inspected = 0
            for candidate in translation:iter() do
                inspected = inspected + 1
                -- The full code, or one key short of it (still typing, like bwwl -> bwwlo).
                -- Longer completions are guesses about keys not yet typed, not typo fixes.
                if supported(candidate) and (candidate.type ~= "completion"
                    or #((candidate.comment or ""):gsub("[^a-z]", "")) <= 1) then
                    hits[#hits + 1] = candidate.text
                end
                if inspected >= MAX_INSPECTED then break end
            end
        end
        variants[#variants + 1] = { code = code, hits = hits }
    end
    -- Round-robin prevents one code's many completions from consuming the pool
    -- before the other variants are checked; the model ranks what gets in.
    for depth = 1, MAX_INSPECTED do
        for _, variant in ipairs(variants) do
            local text = variant.hits[depth]
            if text then append("C", text, variant.code) end
        end
        if count >= MAX_POOL then break end
    end
    local pool = table.concat(lines, "\n")
    if context:get_property("xmjd6_msime_pool") ~= pool then
        context:set_property("xmjd6_msime_pool", pool)
    end
    -- Nothing is yielded here. The final filter inserts only model-approved
    -- corrections, after completion/simplification filters and deduplication.
end

return { init = init, func = translate }
