use crate::{LIQUID_DARK, LIQUID_DEFAULT, LiquidMaterial, Material};

pub fn liquid_material(dark: bool) -> LiquidMaterial {
    if dark { LIQUID_DARK } else { LIQUID_DEFAULT }
}

pub fn liquid_control_defaults(dark: bool) -> Material {
    let data = liquid_material(dark);
    Material {
        liquid: true,
        dark_theme: dark,
        blur_sigma: data.blur.radius as f32,
        blur_alpha0: data.blur.opacities[4] as f32,
        blur_alpha3: data.blur.opacities[0] as f32,
        blur_dist3: -data.blur.distances[0] as f32,
        inner_refraction_amount: data.refraction.inner_amount as f32,
        inner_refraction_inv_height: (1.0 / data.refraction.inner_height) as f32,
        outer_refraction_amount: data.refraction.outer_amount as f32,
        // Zero amount disables the outer ray; its editable height starts here.
        outer_refraction_inv_height: (1.0 / data.refraction.inner_height) as f32,
        refraction_threshold0: -data.refraction.outer_distances[1] as f32,
        refraction_threshold1: -data.refraction.outer_distances[0] as f32,
        refraction_opacity: 1.0,
        face_opacity: data.face.opacity as f32,
        rim_width: data.highlight.key_height as f32,
        rim_opacity: 1.0,
        shadow_amount: data.shadow.opacity as f32,
        shadow_offset: data.shadow.offset[1] as f32,
        neighbor_falloff: 0.0,
        ..Material::default()
    }
}

pub fn effective_liquid_material(material: Material) -> LiquidMaterial {
    let mut data = liquid_material(material.dark_theme);
    let defaults = liquid_control_defaults(material.dark_theme);
    // Preserve untouched doubles, float comparisons, and the original order of
    // operations. An override must not round unrelated base values through f32.
    if material.blur_sigma != defaults.blur_sigma {
        data.blur.radius = f64::from(material.blur_sigma);
    }
    if material.blur_alpha3 != defaults.blur_alpha3 {
        data.blur.opacities[0] = f64::from(material.blur_alpha3);
    }
    if material.blur_alpha0 != defaults.blur_alpha0 {
        for opacity in &mut data.blur.opacities[1..] {
            *opacity *= f64::from(material.blur_alpha0);
        }
    }
    if material.blur_dist3 != defaults.blur_dist3 {
        for distance in &mut data.blur.distances {
            *distance *= f64::from(material.blur_dist3) / f64::from(defaults.blur_dist3);
        }
    }
    if material.inner_refraction_inv_height != defaults.inner_refraction_inv_height {
        data.refraction.inner_height = 1.0 / f64::from(material.inner_refraction_inv_height);
    }
    if material.inner_refraction_amount != defaults.inner_refraction_amount
        || material.refraction_opacity != 1.0
    {
        data.refraction.inner_amount =
            f64::from(material.inner_refraction_amount) * f64::from(material.refraction_opacity);
    }
    if material.outer_refraction_amount != 0.0 && material.refraction_opacity != 0.0 {
        data.refraction.outer_amount =
            f64::from(material.outer_refraction_amount) * f64::from(material.refraction_opacity);
        data.refraction.outer_height = 1.0 / f64::from(material.outer_refraction_inv_height);
    }
    if material.refraction_threshold0 != defaults.refraction_threshold0
        || material.refraction_threshold1 != defaults.refraction_threshold1
    {
        data.refraction.outer_distances = [
            -f64::from(material.refraction_threshold1),
            -f64::from(material.refraction_threshold0),
        ];
    }
    if material.face_opacity != defaults.face_opacity {
        data.face.opacity = f64::from(material.face_opacity);
    }
    if material.rim_width != defaults.rim_width {
        data.highlight.key_height *= f64::from(material.rim_width);
        data.highlight.fill_height *= f64::from(material.rim_width);
    }
    if material.rim_opacity != defaults.rim_opacity {
        for component in &mut data.highlight.key_color {
            *component *= f64::from(material.rim_opacity);
        }
        for component in &mut data.highlight.fill_color {
            *component *= f64::from(material.rim_opacity);
        }
    }
    if material.shadow_amount != defaults.shadow_amount {
        data.shadow.opacity = f64::from(material.shadow_amount);
    }
    if material.shadow_offset != defaults.shadow_offset {
        data.shadow.offset[1] = f64::from(material.shadow_offset);
    }
    data
}

pub fn blur_sigma(material: Material) -> f32 {
    material.blur_sigma
}

pub fn output_extent(material: Material) -> f32 {
    if !material.liquid {
        return 0.0;
    }
    liquid_output_extent(&effective_liquid_material(material))
}

fn liquid_output_extent(liquid: &LiquidMaterial) -> f32 {
    let shadow = liquid.shadow;
    (4.0 * shadow.shadow_radius + shadow.offset[0].abs().max(shadow.offset[1].abs()) + 1.0) as f32
}

pub fn padding(material: Material, scale: f32) -> Option<i32> {
    let footprint = if material.liquid {
        let liquid = effective_liquid_material(material);
        let rays = liquid
            .refraction
            .inner_amount
            .abs()
            .max(liquid.refraction.outer_amount.abs())
            .max(liquid.shadow.amount.abs());
        // Preserve the f32 extent round-trip used by output_extent while
        // reusing the effective material already resolved for the ray bounds.
        f64::from(liquid_output_extent(&liquid)) + rays + 4.5 * liquid.blur.radius + 2.0
    } else {
        // The max and addition below intentionally use f32 before promotion.
        let refraction = material
            .outer_refraction_amount
            .abs()
            .max(material.inner_refraction_amount.abs())
            + material.aberration_amount;
        let bleed = if material.edge_bleed_opacity > 0.0 {
            material.edge_bleed_amount
        } else {
            0.0
        };
        f64::from(refraction.max(bleed)) + 4.5 * f64::from(material.blur_sigma) + 2.0
    };
    let padding = (footprint * f64::from(scale)).ceil();
    if !padding.is_finite() || padding < 0.0 || padding > f64::from(i32::MAX / 4) {
        return None;
    }
    Some(padding as i32)
}
