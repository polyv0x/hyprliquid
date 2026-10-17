hl.monitor({ output = "", mode = "1280x800@60", position = "auto", scale = 1 })
hl.config({
    misc = { disable_hyprland_logo = true, force_default_wallpaper = 0, disable_splash_rendering = true },
    animations = { enabled = false },
    decoration = { rounding = 32, rounding_power = 3.6, shadow = { enabled = false }, blur = { enabled = false } },
    input = { follow_mouse = 0 },
})
if hl.plugin.hyprliquid then
    local glass = hl.plugin.hyprliquid
    glass.config({ default_preset = "test", layers = { enabled = false } })
    glass.preset("test", { inherits = "platform", inner_refraction_amount = -11, coverage_offset = -0.25 })
    glass.preset("cycle", { inherits = "cycle" })
end

hl.window_rule({ name = "test-background", match = { class = "^hyprliquid-background$" }, tag = "+hyprliquid_disabled" })
