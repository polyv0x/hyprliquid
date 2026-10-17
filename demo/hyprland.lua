hl.monitor({ output = "", mode = "800x600@60", position = "1920x0", scale = 1 })
hl.monitor({ output = "HLQ-DEMO", mode = "1920x1200@60", position = "0x0", scale = 1 })
hl.config({
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true,
             force_default_wallpaper = 0, background_color = "rgb(111324)" },
    general = { border_size = 0, gaps_in = 0, gaps_out = 0 },
    decoration = { rounding = 44, rounding_power = 4, shadow = { enabled = true, range = 36, render_power = 3 },
                   blur = { enabled = false } },
    animations = { enabled = false },
    input = { follow_mouse = 0 },
    debug = { suppress_errors = true },
    cursor = { inactive_timeout = 0.1 },
})
hl.window_rule({ name = "demo-float", match = { class = "^hyprliquid-demo-.*$" }, float = true, no_focus = true })
hl.window_rule({ name = "demo-backdrop", match = { class = "^hyprliquid-demo-backdrop$" },
                 tag = "+hyprliquid_disabled", rounding = 0, no_shadow = true })
for _, preset in ipairs({ "frosted", "soft-glass", "clear-glass", "frosted-glass", "clear-edge" }) do
    hl.window_rule({ name = "demo-" .. preset, match = { class = "^hyprliquid-demo-" .. preset .. "$" },
                     tag = "+hyprliquid_preset_" .. preset })
end
if hl.plugin.hyprliquid then
    hl.plugin.hyprliquid.config({ default_preset = "soft-glass", default_theme = "dark", layers = { enabled = false } })
end
