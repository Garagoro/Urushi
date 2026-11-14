-- scripts/steer_assist_mod.lua (pure Lua 155° Steering Assist patcher + telemetry)
local T = i18n.t

local DEG_TO_RAD = math.pi / 180.0
local RAD_TO_DEG = 180.0 / math.pi

-- Struct offsets inside NativeCarX::Car for live telemetry
local OFF_STEER_MODE          = 0x4E8
local OFF_STEER_FF_MODE       = 0x5BC
local OFF_ASSIST_ACTIVE       = 0x5F0
local OFF_SPEED_MPS           = 0x610
local OFF_MAX_STEER_RAD       = 0x838
local OFF_FFB_TORQUE          = 0xB0C
local OFF_WHEEL_FL_IDX        = 0xB3C
local OFF_WHEEL_FR_IDX        = 0xB40
local OFF_WHEELS_BASE         = 0xC18
local WHEEL_STRIDE            = 0x26C80
local OFF_WHEEL_SLIP_NEG      = 0xF6C
local OFF_WHEEL_HAS_SLIP_GATE = 0x26930

local DEFAULT_CFG = {
    enabled = true,
    flip_angle_deg = 155.0,
    disable_early_steer_flip = true,
    early_steer_flip_deg = 135.0,
    fix_slip_gate = true,
    center_scale = 0.30,
}

local sa_settings = {}
for k, v in pairs(DEFAULT_CFG) do sa_settings[k] = v end
if config and config.bind then
    sa_settings = config.bind("steer_assist", sa_settings)
end

local runtime = {
    initialized = false,
    native_found = false,
    bytes_verified = false,
    near_alloc_ok = false,
    flip_patched = false,
    slip_patched = false,
    native_base = 0,
    near_block = 0, -- float[3]: +0 uncondFlipRad, +4 steerFlipRad, +8 centerScale
    rvas = {
        center_scale  = 0x90DE,
        mode0_jump    = 0x91F9,
        pos_gate_jump = 0x92C8,
        neg_gate_jump = 0x92E9,
        steer_flip    = 0xA5C5,
        uncond_flip   = 0xA5F3,
    },
}

local function clamp(x, lo, hi)
    if x < lo then return lo end
    if x > hi then return hi end
    return x
end

local function sanitize_cfg(cfg)
    cfg.enabled = (cfg.enabled == true)
    cfg.flip_angle_deg = clamp(tonumber(cfg.flip_angle_deg) or 155.0, 90.0, 180.0)
    cfg.disable_early_steer_flip = (cfg.disable_early_steer_flip == true)
    cfg.early_steer_flip_deg = clamp(tonumber(cfg.early_steer_flip_deg) or 135.0, 90.0, cfg.flip_angle_deg)
    cfg.fix_slip_gate = (cfg.fix_slip_gate == true)
    cfg.center_scale = clamp(tonumber(cfg.center_scale) or 0.30, 0.15, 0.85)
end

local function patch_disp32(instr_addr, instr_len, disp_offset, target_addr)
    local diff = target_addr - (instr_addr + instr_len)
    if diff < -0x80000000 or diff > 0x7FFFFFFF then
        return false
    end
    local packed = string.pack("<i4", diff)
    local b1, b2, b3, b4 = packed:byte(1, 4)
    local ok = memory.patch_bytes(instr_addr + disp_offset, { b1, b2, b3, b4 })
    return ok == true
end

local function sync_near_floats()
    if runtime.near_block == 0 then return end
    local eff_steer_deg = sa_settings.disable_early_steer_flip
        and sa_settings.flip_angle_deg
        or math.min(sa_settings.early_steer_flip_deg, sa_settings.flip_angle_deg)
    memory.write_float(runtime.near_block + 0, sa_settings.flip_angle_deg * DEG_TO_RAD)
    memory.write_float(runtime.near_block + 4, eff_steer_deg * DEG_TO_RAD)
    memory.write_float(runtime.near_block + 8, sa_settings.center_scale)
end

local function restore_flip_patches()
    if not runtime.flip_patched or runtime.native_base == 0 then return end
    memory.restore_patch(runtime.native_base + runtime.rvas.steer_flip + 3)
    memory.restore_patch(runtime.native_base + runtime.rvas.uncond_flip + 3)
    memory.restore_patch(runtime.native_base + runtime.rvas.center_scale + 4)
    runtime.flip_patched = false
    log.info("SteerAssist (Lua): vector flip & scale patches restored to vanilla.")
end

local function restore_slip_patches()
    if not runtime.slip_patched or runtime.native_base == 0 then return end
    memory.restore_patch(runtime.native_base + runtime.rvas.mode0_jump)
    memory.restore_patch(runtime.native_base + runtime.rvas.pos_gate_jump)
    memory.restore_patch(runtime.native_base + runtime.rvas.neg_gate_jump)
    runtime.slip_patched = false
    log.info("SteerAssist (Lua): front-slip gate restored to vanilla.")
end

local function ensure_initialized()
    if runtime.initialized then return runtime.bytes_verified and runtime.near_alloc_ok end

    local mod = memory.get_module("NativeCarX.dll")
    if not mod or not mod.base or mod.base == 0 then
        return false
    end

    runtime.native_found = true
    runtime.native_base = mod.base

    local function scan_rva(sig, add_off, fallback_rva)
        local va = memory.scan("NativeCarX.dll", sig)
        if va and va ~= 0 then
            return (va + add_off) - runtime.native_base
        end
        return fallback_rva
    end

    runtime.rvas.center_scale  = scan_rva("0F 28 C4 F3 0F 59 05 ? ? ? ? F3 41 0F 5D D0", 3, 0x90DE)
    runtime.rvas.mode0_jump    = scan_rva("F3 0F 11 8B F8 00 00 00 76 40 44 0F 2F 15", 8, 0x91F9)
    runtime.rvas.pos_gate_jump = scan_rva("F3 0F 59 15 ? ? ? ? 76 1B F3 0F 10 05", 8, 0x92C8)
    runtime.rvas.neg_gate_jump = scan_rva("45 0F 2F D4 73 1B F3 0F 10 05", 4, 0x92E9)
    runtime.rvas.steer_flip    = scan_rva("0F 28 C8 0F 54 CA 0F 2F 0D ? ? ? ? 76 25", 6, 0xA5C5)
    runtime.rvas.uncond_flip   = scan_rva("B3 01 41 88 1F 0F 2F 0D ? ? ? ? 0F B6 DB", 5, 0xA5F3)

    local b_cs0 = memory.read_u8(runtime.native_base + runtime.rvas.center_scale)
    local b_cs1 = memory.read_u8(runtime.native_base + runtime.rvas.center_scale + 1)
    local b_cs2 = memory.read_u8(runtime.native_base + runtime.rvas.center_scale + 2)
    local b_m0  = memory.read_u8(runtime.native_base + runtime.rvas.mode0_jump)
    local b_pg  = memory.read_u8(runtime.native_base + runtime.rvas.pos_gate_jump)
    local b_ng  = memory.read_u8(runtime.native_base + runtime.rvas.neg_gate_jump)
    local b_sf0 = memory.read_u8(runtime.native_base + runtime.rvas.steer_flip)
    local b_sf1 = memory.read_u8(runtime.native_base + runtime.rvas.steer_flip + 1)
    local b_uf0 = memory.read_u8(runtime.native_base + runtime.rvas.uncond_flip)
    local b_uf1 = memory.read_u8(runtime.native_base + runtime.rvas.uncond_flip + 1)

    local valid = (b_cs0 == 0xF3 and b_cs1 == 0x0F and b_cs2 == 0x59)
        and (b_m0 == 0x76 or b_m0 == 0xEB)
        and (b_pg == 0x76 or b_pg == 0x90)
        and (b_ng == 0x73 or b_ng == 0x90)
        and (b_sf0 == 0x0F and b_sf1 == 0x2F)
        and (b_uf0 == 0x0F and b_uf1 == 0x2F)

    runtime.initialized = true
    if not valid then
        runtime.bytes_verified = false
        log.warn("SteerAssist (Lua): NativeCarX.dll opcodes could not be verified. Patches disabled safely.")
        return false
    end
    runtime.bytes_verified = true

    local page = memory.alloc_near("NativeCarX.dll", 4096)
    if not page or page == 0 then
        runtime.near_alloc_ok = false
        log.error("SteerAssist (Lua): failed to allocate memory within +/-2GB of NativeCarX.dll.")
        return false
    end
    runtime.near_block = page
    runtime.near_alloc_ok = true
    return true
end

local function apply_patches()
    sanitize_cfg(sa_settings)
    if not ensure_initialized() then return end

    sync_near_floats()

    if sa_settings.enabled and not runtime.flip_patched then
        local ok1 = patch_disp32(runtime.native_base + runtime.rvas.steer_flip, 7, 3, runtime.near_block + 4)
        local ok2 = patch_disp32(runtime.native_base + runtime.rvas.uncond_flip, 7, 3, runtime.near_block + 0)
        local ok3 = patch_disp32(runtime.native_base + runtime.rvas.center_scale, 8, 4, runtime.near_block + 8)
        if ok1 and ok2 and ok3 then
            runtime.flip_patched = true
            log.info("SteerAssist (Lua): 155-deg vector flip & target scale patches active.")
        else
            memory.restore_patch(runtime.native_base + runtime.rvas.steer_flip + 3)
            memory.restore_patch(runtime.native_base + runtime.rvas.uncond_flip + 3)
            memory.restore_patch(runtime.native_base + runtime.rvas.center_scale + 4)
            log.error("SteerAssist (Lua): failed to apply RIP-relative displacement patches (range > 2GB).")
        end
    elseif not sa_settings.enabled and runtime.flip_patched then
        restore_flip_patches()
    end

    local want_slip = sa_settings.enabled and sa_settings.fix_slip_gate
    if want_slip and not runtime.slip_patched then
        local ok1 = memory.patch_bytes(runtime.native_base + runtime.rvas.mode0_jump, { 0xEB, 0x40 })
        local ok2 = memory.patch_bytes(runtime.native_base + runtime.rvas.pos_gate_jump, { 0x90, 0x90 })
        local ok3 = memory.patch_bytes(runtime.native_base + runtime.rvas.neg_gate_jump, { 0x90, 0x90 })
        if ok1 and ok2 and ok3 then
            runtime.slip_patched = true
            log.info("SteerAssist (Lua): symmetric front-slip gate patch active (removed -acos lockup).")
        else
            memory.restore_patch(runtime.native_base + runtime.rvas.mode0_jump)
            memory.restore_patch(runtime.native_base + runtime.rvas.pos_gate_jump)
            memory.restore_patch(runtime.native_base + runtime.rvas.neg_gate_jump)
        end
    elseif not want_slip and runtime.slip_patched then
        restore_slip_patches()
    end
end

local function get_stats()
    ensure_initialized()
    local st = {
        initialized = runtime.initialized,
        native_found = runtime.native_found,
        bytes_verified = runtime.bytes_verified,
        near_alloc_ok = runtime.near_alloc_ok,
        patches_applied = runtime.flip_patched or runtime.slip_patched,
        native_base = runtime.native_base,
        near_data_addr = runtime.near_block,
        has_car = false,
        steer_mode = 0,
        steer_ff_mode = 0,
        assist_active = 0,
        speed_kmh = 0.0,
        steer_deg = 0.0,
        max_steer_deg = 0.0,
        ffb_torque = 0.0,
        front_slip_deg = 0.0,
    }

    local car = carx and carx.get_player_car and carx.get_player_car()
    local car_addr = car and car.addr or 0
    if car_addr and car_addr >= 0x10000 then
        local mode = memory.read_i32(car_addr + OFF_STEER_MODE)
        local ff_mode = memory.read_i32(car_addr + OFF_STEER_FF_MODE)
        local active = memory.read_i32(car_addr + OFF_ASSIST_ACTIVE)
        local speed_mps = memory.read_float(car_addr + OFF_SPEED_MPS)
        local max_steer = memory.read_float(car_addr + OFF_MAX_STEER_RAD)
        local ffb = memory.read_float(car_addr + OFF_FFB_TORQUE)
        if mode and ff_mode and active and speed_mps and max_steer and ffb then
            st.has_car = true
            st.steer_mode = mode
            st.steer_ff_mode = ff_mode
            st.assist_active = active
            st.speed_kmh = speed_mps * 3.6
            st.max_steer_deg = max_steer * RAD_TO_DEG
            st.ffb_torque = ffb
            st.steer_deg = (car.steer_angle or 0.0) * RAD_TO_DEG

            local fl_idx = memory.read_i32(car_addr + OFF_WHEEL_FL_IDX)
            local fr_idx = memory.read_i32(car_addr + OFF_WHEEL_FR_IDX)
            if fl_idx and fr_idx and fl_idx >= 0 and fl_idx <= 7 and fr_idx >= 0 and fr_idx <= 7 then
                local fl_base = car_addr + OFF_WHEELS_BASE + fl_idx * WHEEL_STRIDE
                local fr_base = car_addr + OFF_WHEELS_BASE + fr_idx * WHEEL_STRIDE
                local has_gate = memory.read_u8(fl_base + OFF_WHEEL_HAS_SLIP_GATE)
                local fl_sa = memory.read_float(fl_base + OFF_WHEEL_SLIP_NEG)
                local fr_sa = memory.read_float(fr_base + OFF_WHEEL_SLIP_NEG)
                if has_gate and has_gate ~= 0 and fl_sa and fr_sa then
                    st.front_slip_deg = 0.5 * (math.abs(fl_sa) + math.abs(fr_sa)) * RAD_TO_DEG
                end
            end
        end
    end
    return st
end

-- Expose compatible `steer_assist` table in Lua for any dependent script/runner
steer_assist = {
    get_config = function()
        local c = {}
        for k, v in pairs(sa_settings) do c[k] = v end
        return c
    end,
    set_config = function(cfg)
        if type(cfg) == "table" then
            for k, v in pairs(cfg) do
                if DEFAULT_CFG[k] ~= nil then sa_settings[k] = v end
            end
        end
        apply_patches()
    end,
    set_recommended = function()
        for k, v in pairs(DEFAULT_CFG) do sa_settings[k] = v end
        apply_patches()
    end,
    set_vanilla = function()
        sa_settings.enabled = false
        sa_settings.flip_angle_deg = 140.0
        sa_settings.disable_early_steer_flip = false
        sa_settings.early_steer_flip_deg = 90.0
        sa_settings.fix_slip_gate = false
        sa_settings.center_scale = 0.30
        apply_patches()
    end,
    get_stats = get_stats,
}

-- Apply initial patches once module loads
apply_patches()

local function is_recommended_preset(cfg)
    return cfg.enabled == true
        and math.abs((cfg.flip_angle_deg or 0.0) - 155.0) < 0.5
        and cfg.disable_early_steer_flip == true
        and cfg.fix_slip_gate == true
        and math.abs((cfg.center_scale or 0.0) - 0.30) < 0.01
end

local function render_steer_assist_window()
    local cfg = sa_settings
    local st = get_stats()

    local patch_txt = T("Сток", "Stock")
    local patch_tone = "muted"
    if st.patches_applied then
        patch_txt = string.format("%.0f°", cfg.flip_angle_deg or 155.0)
        patch_tone = "green"
    elseif not st.native_found then
        patch_txt = T("Ожидание", "Waiting")
        patch_tone = "yellow"
    elseif not st.bytes_verified then
        patch_txt = T("Не совпал", "Mismatch")
        patch_tone = "red"
    end

    local slip_txt = (cfg.enabled and cfg.fix_slip_gate) and T("Снят", "Fixed") or T("Сток", "Stock")
    local slip_tone = (cfg.enabled and cfg.fix_slip_gate) and "green" or "yellow"
    local snap_txt = (cfg.enabled and cfg.disable_early_steer_flip)
        and T("Выкл", "Off")
        or (cfg.enabled and string.format("%.0f°", cfg.early_steer_flip_deg or 135.0) or "90°")

    ui.card(T("Режим помощника DR2", "DR2 Steering Assist"), nil, function()
        local ch_en, val_en = ui.toggle(T("Включить фикс помощника##sa_en", "Enable Assist Fix##sa_en"), cfg.enabled == true)
        if ch_en then
            cfg.enabled = val_en
            apply_patches()
            if config then config.mark_dirty() end
        end

        ui.spacing(4)
        local is_rec = is_recommended_preset(cfg)
        local is_van = (cfg.enabled == false)
        local bw = (ui.avail_width() - 6) * 0.5

        if ui.button(T("155° Баквард##sa_p_rec", "155° Backwards##sa_p_rec"), bw, 28, is_rec and "primary" or "default") then
            steer_assist.set_recommended()
            if config then config.mark_dirty() end
        end
        ui.same_line(0, 6)
        if ui.button(T("Сток DR2##sa_p_van", "Stock DR2##sa_p_van"), bw, 28, is_van and "primary" or "default") then
            steer_assist.set_vanilla()
            if config then config.mark_dirty() end
        end

        ui.spacing(4)
        ui.stat_row({
            { label = T("Порог", "Flip"), value = patch_txt, tone = patch_tone },
            { label = T("Блок ±15.6°", "±15.6° Bug"), value = slip_txt, tone = slip_tone },
            { label = T("Срыв 90°", "90° Snap"), value = snap_txt, tone = (cfg.enabled and cfg.disable_early_steer_flip) and "accent" or "muted" },
        })
    end)

    ui.card(T("Пороги и поведение", "Thresholds"), nil, function()
        local ch_flip, val_flip = ui.slider_float(
            T("Переброс 360° / задний ход##sa_flip", "360° / Reverse Flip Angle##sa_flip"),
            cfg.flip_angle_deg or 155.0, 90.0, 180.0, "%.0f°"
        )
        if ch_flip then
            cfg.flip_angle_deg = val_flip
            if (cfg.early_steer_flip_deg or 135.0) > val_flip then
                cfg.early_steer_flip_deg = val_flip
            end
            apply_patches()
            if config then config.mark_dirty() end
        end

        local ch_early, val_early = ui.toggle(
            T("Убрать срыв на 90°##sa_no90", "Disable 90° Spin Snap##sa_no90"),
            cfg.disable_early_steer_flip == true
        )
        if ch_early then
            cfg.disable_early_steer_flip = val_early
            apply_patches()
            if config then config.mark_dirty() end
        end

        if not cfg.disable_early_steer_flip then
            local ch_edeg, val_edeg = ui.slider_float(
                T("Срыв при полном руле##sa_edeg", "Full-Steer Spin Flip##sa_edeg"),
                cfg.early_steer_flip_deg or 135.0, 90.0, cfg.flip_angle_deg or 155.0, "%.0f°"
            )
            if ch_edeg then
                cfg.early_steer_flip_deg = val_edeg
                apply_patches()
                if config then config.mark_dirty() end
            end
        end

        local ch_slip, val_slip = ui.toggle(
            T("Снять блок сноса ±15.6°##sa_slip", "Fix ±15.6° Asymmetric Slip Lock##sa_slip"),
            cfg.fix_slip_gate == true
        )
        if ch_slip then
            cfg.fix_slip_gate = val_slip
            apply_patches()
            if config then config.mark_dirty() end
        end

        if ui.collapsing(T("Тонкая настройка##sa_adv", "Advanced##sa_adv"), false) then
            local ch_cs, val_cs = ui.slider_float(
                T("Возврат ассиста (сток 0.30x)##sa_cs", "Centering Ratio (Stock 0.30x)##sa_cs"),
                cfg.center_scale or 0.30, 0.15, 0.85, "%.2fx"
            )
            if ch_cs then
                cfg.center_scale = val_cs
                apply_patches()
                if config then config.mark_dirty() end
            end
        end
    end)

    ui.card(T("Телеметрия", "Live Telemetry"), nil, function()
        if st.has_car then
            local front_sa = st.front_slip_deg or 0.0
            ui.stat_row({
                { label = T("Скорость", "Speed"), value = string.format(T("%.0f км/ч", "%.0f km/h"), st.speed_kmh or 0.0), tone = "accent" },
                { label = T("Руль", "Steer"), value = string.format("%.1f° / %.0f°", st.steer_deg or 0.0, st.max_steer_deg or 65.0) },
                { label = T("Снос", "Front Slip"), value = string.format("%.1f°", front_sa), tone = front_sa > 15.6 and "yellow" or "green" },
            })
        else
            ui.text_dim(T("Выйдите на трассу для телеметрии.", "Enter a track for live telemetry."))
        end
    end)
end

if urushi and urushi.register_window then
    urushi.register_window({
        id = "steer_assist",
        title = T("Помощник руля", "Steering Assist"),
        icon = "wheel",
        order = 35,
        default_open = false,
        default_pinned = false,
        x = 716,
        y = 24,
        width = 408,
        settings = sa_settings,
        render = render_steer_assist_window,
    })
end

log.info("URUSHI Steering Assistant module loaded (pure Lua).")
