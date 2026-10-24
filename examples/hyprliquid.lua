-- Load hyprliquid.so before this block runs. The plugin reloads the config on load.
if hl.plugin.hyprliquid then
    local glass = hl.plugin.hyprliquid

    glass.config({
        default_preset = "frosted-glass",
        manage_window_blur = true,
        layers = { enabled = false },
    })

    -- The Liquid family has its own refraction, highlights, and shadow.
    -- glass.preset("my_liquid", {
    --     inherits = "frosted-liquid", glass_opacity = 0.9,
    --     effect_scale = 1, neighbor_contribution = 1, neighbor_falloff = 0,
    --     display_angle_degrees = 30,
    -- })
    -- glass.config({ default_preset = "my_liquid", default_theme = "dark" })
    -- Optional 0 -> configured angle -> 0 sweep during native window resizing.
    -- glass.config({ animate_display_angle = true })

    -- Optional custom terminal material; enable its window rule below too.
    -- glass.preset("terminal", {
    --     inherits = "frosted-glass-clear-edge",
    --     blur_sigma = 12,
    --     inner_refraction_amount = -10,
    --     rim_opacity = 0.6,
    -- })

    -- Optional Noctalia surfaces; enable layers above to apply these.
    glass.layer("noctalia-bar-default", { preset = "frosted", mask_threshold = 0.055 })
    glass.layer("noctalia-panel", { preset = "soft-glass", mask_threshold = 0.035 })
    glass.layer("noctalia-attached-panel", { preset = "soft-glass", mask_threshold = 0.035 })
    glass.layer("noctalia-notification", { preset = "frosted", mask_threshold = 0.045 })
end

-- hl.window_rule({ name = "hyprliquid-terminal", match = { class = "^kitty$" },
--                  tag = "+hyprliquid_preset_terminal" })
hl.window_rule({ name = "hyprliquid-video", match = { class = "^(mpv|vlc)$" },
                 tag = "+hyprliquid_disabled" })
