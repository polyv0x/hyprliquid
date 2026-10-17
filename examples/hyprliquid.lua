-- Load hyprliquid.so before this block runs. The plugin reloads the config on load.
if hl.plugin.hyprliquid then
    local glass = hl.plugin.hyprliquid

    glass.config({
        default_preset = "soft-glass", -- "frosted" matches the WebGL demo on first load
        manage_window_blur = true,
        layers = { enabled = false },
    })

    glass.preset("terminal", {
        inherits = "soft-glass",
        blur_sigma = 12,
        inner_refraction_amount = -10,
        rim_opacity = 0.6,
    })

    -- Optional Noctalia surfaces; enable layers above to apply these.
    glass.layer("noctalia-bar-default", { preset = "frosted", mask_threshold = 0.055 })
    glass.layer("noctalia-panel", { preset = "soft-glass", mask_threshold = 0.035 })
    glass.layer("noctalia-attached-panel", { preset = "soft-glass", mask_threshold = 0.035 })
    glass.layer("noctalia-notification", { preset = "frosted", mask_threshold = 0.045 })
end

hl.window_rule({ name = "hyprliquid-terminal", match = { class = "^kitty$" },
                 tag = "+hyprliquid_preset_terminal" })
hl.window_rule({ name = "hyprliquid-video", match = { class = "^(mpv|vlc)$" },
                 tag = "+hyprliquid_disabled" })
