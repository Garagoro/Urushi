-- scripts/runner.lua: Автономный IPC-мост для удаленного выполнения тестов и сканирования на Main Thread Unity

local runner = {}

local function escape_str(s)
    s = tostring(s)
    s = s:gsub("\\", "\\\\"):gsub("\"", "\\\""):gsub("\n", "\\n"):gsub("\r", "\\r"):gsub("\t", "\\t")
    return "\"" .. s .. "\""
end

local function safe_json_encode(v, depth)
    depth = depth or 0
    if depth > 5 then return "\"<max_depth>\"" end
    local t = type(v)
    if t == "nil" then return "null"
    elseif t == "boolean" then return v and "true" or "false"
    elseif t == "number" then
        if v ~= v then return "\"NaN\"" end
        if v == math.huge then return "\"Infinity\"" end
        if v == -math.huge then return "\"-Infinity\"" end
        return tostring(v)
    elseif t == "string" then return escape_str(v)
    elseif t == "userdata" or t == "lightuserdata" then return escape_str(tostring(v))
    elseif t == "table" then
        local is_array = (#v > 0)
        local parts = {}
        if is_array then
            for i = 1, #v do
                table.insert(parts, safe_json_encode(v[i], depth + 1))
            end
            return "[" .. table.concat(parts, ",") .. "]"
        else
            for k, val in pairs(v) do
                table.insert(parts, string.format("%s:%s", escape_str(tostring(k)), safe_json_encode(val, depth + 1)))
            end
            return "{" .. table.concat(parts, ",") .. "}"
        end
    else
        return escape_str(tostring(v))
    end
end

local function process_ipc_task(dt)
    local f = io.open("scripts/run_task.lua", "r")
    if not f then return end
    local code = f:read("*a")
    f:close()
    os.remove("scripts/run_task.lua")

    if not code or code == "" then return end

    local logs = {}
    local old_print = print
    _G.print = function(...)
        local args = { ... }
        local str = {}
        for i = 1, #args do
            table.insert(str, tostring(args[i]))
        end
        local msg = table.concat(str, "\t")
        table.insert(logs, msg)
        log.info("[IPC Task] " .. msg)
    end

    local fn, err = load(code, "run_task")
    local success, result = false, nil
    if fn then
        success, result = pcall(fn)
    else
        result = tostring(err)
    end

    _G.print = old_print

    local response = {
        success = success,
        result = result,
        logs = logs,
        main_thread = (unity and unity.is_main_thread) and unity.is_main_thread() or false,
        timestamp = os.time()
    }

    local ok, json_text = pcall(safe_json_encode, response)
    if not ok then
        json_text = string.format("{\"success\":false,\"error\":\"JSON encode error: %s\"}", escape_str(tostring(json_text)))
    end

    local out_file = io.open("scripts/task_output.json", "w")
    if out_file then
        out_file:write(json_text)
        out_file:close()
    end
end

if events.on_main_update then
    events.on_main_update(process_ipc_task)
    log.info("Автономный IPC-мост runner.lua (v3 Main Thread) запущен.")
else
    events.on_update(process_ipc_task)
    log.info("Автономный IPC-мост runner.lua (v3 Fallback Present) запущен.")
end

return runner
