-- scripts/il2cpp.lua
-- Universal IL2CPP & Unity Scene Reflection Library for URUSHI
-- Allows modders and AI assistants to inspect, read, and write any Unity/CarX component.

local il2cpp = {}

local function to_addr(ptr_or_num)
    if not ptr_or_num then return 0 end
    if type(ptr_or_num) == "number" then return ptr_or_num end
    if type(ptr_or_num) == "table" then
        return ptr_or_num.managed_addr or ptr_or_num.addr or to_addr(ptr_or_num.ptr)
    end
    local s = tostring(ptr_or_num)
    local hex = s:match("%s+(%x+)") or s:match("(%x+)$")
    return hex and tonumber(hex, 16) or 0
end

-- Check if Unity managed object is alive (not destroyed)
-- In Unity IL2CPP, UnityEngine.Object has m_CachedPtr at offset +0x10.
-- If m_CachedPtr == 0, the native C++ object is destroyed.
function il2cpp.is_alive(obj_ptr)
    local addr = to_addr(obj_ptr)
    if not addr or addr < 0x10000 then return false end
    local test = memory.read_ptr(addr)
    if not test then return false end
    local cachedPtr = memory.read_ptr(addr + 0x10)
    return cachedPtr ~= nil and cachedPtr ~= 0
end

-- Get all components on a GameObject
function il2cpp.get_components(go)
    if not unity or not unity.get_components then return {} end
    return unity.get_components(go)
end

-- Find a specific component on a GameObject by name (e.g. "CarBehaviour")
function il2cpp.get_component(go, comp_name)
    local comps = il2cpp.get_components(go)
    local lower_target = comp_name:lower()
    for _, c in ipairs(comps) do
        if c.name and (c.name:lower() == lower_target or c.name:lower():find(lower_target, 1, true)) then
            return c
        end
    end
    return nil
end

-- Inspect an IL2CPP component / managed object
function il2cpp.inspect(comp_or_ptr)
    local addr = to_addr(comp_or_ptr)
    if not addr or addr < 0x10000 then return nil end

    local klass = 0
    if type(comp_or_ptr) == "table" and comp_or_ptr.class_addr and comp_or_ptr.class_addr > 0x10000 then
        klass = comp_or_ptr.class_addr
    else
        klass = memory.read_u64(addr)
    end

    if not klass or klass < 0x10000 then return nil end

    local namePtr = memory.read_u64(klass + 0x10)
    local nsPtr = memory.read_u64(klass + 0x18)
    local kname = namePtr and memory.read_string(namePtr, 64) or "UnknownClass"
    local kns = nsPtr and memory.read_string(nsPtr, 64) or ""

    local method_count = memory.read_u16(klass + 0x120) or 0
    local field_count = memory.read_u16(klass + 0x124) or 0

    local fieldsPtr = memory.read_u64(klass + 0x80)
    local methodsPtr = memory.read_u64(klass + 0x98)

    local info = {
        name = kname,
        namespace = kns,
        full_name = (#kns > 0) and (kns .. "." .. kname) or kname,
        managed_addr = addr,
        class_addr = klass,
        is_alive = il2cpp.is_alive(addr),
        field_count = field_count,
        method_count = method_count,
        fields = {},
        methods = {},
    }

    -- Enumerate Fields
    if fieldsPtr and fieldsPtr > 0x10000 and field_count > 0 and field_count < 1000 then
        for i = 0, field_count - 1 do
            local fPtr = fieldsPtr + i * 32
            local fNamePtr = memory.read_u64(fPtr + 0)
            if fNamePtr and fNamePtr > 0x10000 then
                local fName = memory.read_string(fNamePtr, 64) or "field_" .. i
                local offRaw = memory.read_u64(fPtr + 24) or 0
                local off = offRaw % 0x100000000
                local is_static = (off == 0 and i < 20)

                local field_entry = {
                    index = i,
                    name = fName,
                    offset = off,
                    is_static = is_static,
                }

                -- Read live values if instance field
                if addr > 0x10000 and off > 0 and off < 0x2000 then
                    local fAddr = addr + off
                    field_entry.val_float = memory.read_float(fAddr)
                    field_entry.val_int = memory.read_i32 and memory.read_i32(fAddr) or memory.read_u32(fAddr)
                    field_entry.val_ptr = memory.read_ptr and memory.read_ptr(fAddr) or memory.read_u64(fAddr)
                end

                table.insert(info.fields, field_entry)
            end
        end
    end

    -- Enumerate Methods
    if methodsPtr and methodsPtr > 0x10000 and method_count > 0 and method_count < 1000 then
        for i = 0, method_count - 1 do
            local mPtr = memory.read_u64(methodsPtr + i * 8)
            if mPtr and mPtr > 0x10000 then
                local mNamePtr = memory.read_u64(mPtr + 0x18)
                local mName = mNamePtr and memory.read_string(mNamePtr, 64) or "method_" .. i
                local fnPtr = memory.read_u64(mPtr + 0x00)
                local paramsCount = memory.read_u8(mPtr + 0x4B) or 0

                table.insert(info.methods, {
                    index = i,
                    name = mName,
                    fn_ptr = fnPtr,
                    params_count = paramsCount,
                })
            end
        end
    end

    return info
end

-- Get field offset by name
function il2cpp.get_field_offset(klass_or_comp, field_name)
    local info = il2cpp.inspect(klass_or_comp)
    if not info then return nil end
    for _, f in ipairs(info.fields) do
        if f.name == field_name then
            return f.offset
        end
    end
    return nil
end

-- Get method address by name and optional param count
function il2cpp.get_method_addr(klass_or_comp, method_name, param_count)
    local info = il2cpp.inspect(klass_or_comp)
    if not info then return nil end
    for _, m in ipairs(info.methods) do
        if m.name == method_name then
            if param_count == nil or m.params_count == param_count then
                return m.fn_ptr
            end
        end
    end
    return nil
end

-- Read a field value by name
function il2cpp.get_field(comp_or_ptr, field_name)
    local info = il2cpp.inspect(comp_or_ptr)
    if not info then return nil end
    for _, f in ipairs(info.fields) do
        if f.name == field_name then
            return f.val_float, f.val_int, f.val_ptr
        end
    end
    return nil
end

-- Write a float field by name
function il2cpp.set_field_float(comp_or_ptr, field_name, value)
    local addr = to_addr(comp_or_ptr)
    local off = il2cpp.get_field_offset(comp_or_ptr, field_name)
    if not off or off <= 0 or not addr or addr < 0x10000 then return false end
    return memory.write_float(addr + off, value)
end

-- Write an integer/i32 field by name
function il2cpp.set_field_int(comp_or_ptr, field_name, value)
    local addr = to_addr(comp_or_ptr)
    local off = il2cpp.get_field_offset(comp_or_ptr, field_name)
    if not off or off <= 0 or not addr or addr < 0x10000 then return false end
    if memory.write_i32 then
        return memory.write_i32(addr + off, value)
    else
        return memory.write_u32(addr + off, value)
    end
end

-- Find all objects of a given component name in the scene
function il2cpp.find_objects_of_type(comp_name)
    if not unity or not unity.get_all_objects then return {} end
    local all = unity.get_all_objects(2000)
    local results = {}
    local lower_target = comp_name:lower()
    for _, obj in ipairs(all) do
        local comp = il2cpp.get_component(obj.ptr, comp_name)
        if comp then
            table.insert(results, {
                go = obj.ptr,
                go_name = obj.name,
                comp = comp,
            })
        end
    end
    return results
end

-- Generate a complete, self-contained Markdown prompt for AI (ChatGPT / Claude / Gemini)
function il2cpp.dump_for_ai(comp_or_obj, opts)
    opts = opts or {}
    local addr = to_addr(comp_or_obj)
    local info = il2cpp.inspect(comp_or_obj)
    if not info then return "# Error: Could not inspect object at " .. string.format("0x%X", addr) end

    local mod = pattern and pattern.get_module and pattern.get_module("GameAssembly.dll")
    local modBase = mod and mod.base or 0

    local out = {}
    table.insert(out, "# URUSHI LUA MODDING CONTEXT (CarX Drift Racing Online 2 - Unity 6 IL2CPP x64)")
    table.insert(out, "Use the built-in `memory`, `il2cpp`, and `re` Lua 5.4 libraries provided by Urushi.")
    table.insert(out, "Never hardcode raw memory addresses; always use `il2cpp.get_field_offset` or `memory.scan` with the signatures below.\n")

    table.insert(out, "## Available Lua API Reference:")
    table.insert(out, "- `memory.read_float(addr)`, `memory.write_float(addr, val)`")
    table.insert(out, "- `memory.read_i32(addr)`, `memory.write_i32(addr, val)`")
    table.insert(out, "- `memory.read_u8(addr)`, `memory.write_u8(addr, val)` (1/0 for bool)")
    table.insert(out, "- `memory.read_ptr(addr)`, `memory.write_ptr(addr, val)`")
    table.insert(out, "- `memory.scan(\"GameAssembly.dll\", \"IDA_SIGNATURE\")` -> returns address (cached)")
    table.insert(out, "- `memory.patch_bytes(addr, \"90 90\")`, `memory.restore_patch(addr)`")
    table.insert(out, "- `il2cpp.is_alive(obj_ptr)` -> checks if Unity object is not destroyed")
    table.insert(out, "- `il2cpp.get_field(comp, \"name\")` / `il2cpp.set_field_float(comp, \"name\", val)`")
    table.insert(out, "- `il2cpp.get_field_offset(comp, \"name\")` -> integer byte offset")
    table.insert(out, "- `il2cpp.get_component(game_object, \"ComponentName\")` -> component table")
    table.insert(out, "- `carx.get_player_car()` -> table with car physics and state\n")

    table.insert(out, string.format("## Target Class Dump: `%s`", info.full_name))
    table.insert(out, string.format("Instance Pointer at dump time: `0x%016X`", addr))
    table.insert(out, string.format("Is Alive: `%s`", tostring(info.is_alive)))
    table.insert(out, string.format("Class Metadata Pointer: `0x%016X`\n", info.class_addr))

    table.insert(out, string.format("### Fields (%d total, Offsets & Live Values):", #info.fields))
    for _, f in ipairs(info.fields) do
        local valStr = "null"
        if f.val_float and math.abs(f.val_float) > 0.00001 and math.abs(f.val_float) < 1000000 then
            valStr = string.format("float=%.4f (i32=%d)", f.val_float, f.val_int or 0)
        elseif f.val_ptr and f.val_ptr > 0 then
            valStr = string.format("ptr=0x%016X", f.val_ptr)
        elseif f.val_int then
            valStr = string.format("int=%d", f.val_int)
        end
        table.insert(out, string.format("- `+0x%04X` | `%-32s` = `%s`", f.offset, f.name, valStr))
    end

    table.insert(out, string.format("\n### Methods (%d total, with Auto-Generated Signatures in GameAssembly.dll):", #info.methods))
    local sigCount = 0
    for _, m in ipairs(info.methods) do
        local rvaStr = (modBase > 0 and m.fn_ptr > modBase) and string.format("0x%08X", m.fn_ptr - modBase) or string.format("0x%X", m.fn_ptr)
        table.insert(out, string.format("- `%s(params: %d)`", m.name, m.params_count))
        table.insert(out, string.format("  - RVA: `%s`", rvaStr))

        -- Generate signature via Zydis if re.make_signature is available
        if re and re.make_signature and m.fn_ptr > 0x10000 and sigCount < 15 then
            local sig, matches, len = re.make_signature(m.fn_ptr, "GameAssembly.dll", 14, 48)
            if sig and #sig > 0 then
                table.insert(out, string.format("  - Signature (%d bytes, matches=%d): `%s`", len or 0, matches or 0, sig))
                sigCount = sigCount + 1
            end
        end
    end

    local markdown = table.concat(out, "\n")

    -- Save to urushi_data/dumps/<ClassName>_AI_Context.txt
    local safeName = info.name:gsub("[^%w_%-]", "_")
    local dumpPath = "urushi_data/" .. safeName .. "_AI_Context.txt"
    local f = io.open(dumpPath, "w")
    if f then
        f:write(markdown)
        f:close()
        if log and log.info then log.info("Saved AI context dump to " .. dumpPath) end
    end

    -- Copy to Windows clipboard if available
    if re and re.copy_to_clipboard then
        re.copy_to_clipboard(markdown)
    end

    return markdown
end

-- Call an IL2CPP method dynamically on an object
function il2cpp.call(comp_or_ptr, method_name, ...)
    local addr = to_addr(comp_or_ptr)
    if not addr or addr < 0x10000 then return nil, "invalid target object" end
    local fn_addr = il2cpp.get_method_addr(comp_or_ptr, method_name)
    if not fn_addr or fn_addr < 0x10000 then
        return nil, "method not found: " .. tostring(method_name)
    end
    if not memory or not memory.call then
        return nil, "memory.call is not available"
    end
    return memory.call(fn_addr, addr, ...)
end

-- Helper for repairing local player car
function il2cpp.repair_car()
    if carx and carx.repair_car then
        return carx.repair_car()
    end
    return false
end

-- Expose to global re namespace
if re then
    re.dump_for_ai = il2cpp.dump_for_ai
end

_G.il2cpp = il2cpp
return il2cpp
