use hyprliquid_core::{
    DESCRIPTORS, LIQUID_DARK, LIQUID_DEFAULT, Material, geometry::*, material::*,
};

#[test]
fn untouched_values_keep_double_precision() {
    for dark in [false, true] {
        let base = if dark { LIQUID_DARK } else { LIQUID_DEFAULT };
        let mut material = liquid_control_defaults(dark);
        assert_eq!(effective_liquid_material(material), base);
        material.blur_sigma = 19.0;
        let mut expected = base;
        expected.blur.radius = 19.0;
        assert_eq!(effective_liquid_material(material), expected);
        assert_ne!(base.shadow.opacity, f64::from(base.shadow.opacity as f32));
        assert_eq!(padding(material, 1.0), Some(268));
        assert_eq!(padding(liquid_control_defaults(dark), 1.0), Some(209));
    }
}

#[test]
fn signed_controls_and_disabled_rays() {
    let mut material = liquid_control_defaults(true);
    material.outer_refraction_inv_height = 0.001;
    assert_eq!(
        effective_liquid_material(material).refraction.outer_height,
        0.0
    );
    material.outer_refraction_amount = -120.0;
    material.refraction_opacity = 0.5;
    material.shadow_offset = -30.0;
    let effective = effective_liquid_material(material);
    assert_eq!(effective.refraction.outer_amount, -60.0);
    assert_eq!(effective.refraction.inner_amount, -30.0);
    assert_eq!(
        effective.refraction.outer_height,
        1.0 / f64::from(0.001_f32)
    );
    assert_eq!(output_extent(material), 127.0);
    material.refraction_opacity = 0.0;
    assert_eq!(
        effective_liquid_material(material).refraction.outer_height,
        0.0
    );
    assert_eq!(
        effective_liquid_material(material).refraction.inner_amount,
        0.0
    );
    material.blur_sigma = 0.0;
    material.blur_dist3 = 0.1;
    let effective = effective_liquid_material(material);
    assert_eq!(effective.blur.radius, 0.0);
    assert_eq!(effective.blur.distances[2..], [0.0; 3]);
}

#[test]
fn sanitization_and_descriptor_contract() {
    assert_eq!(DESCRIPTORS.len(), 34);
    let angle = DESCRIPTORS
        .iter()
        .find(|d| d.name == "display_angle_degrees")
        .unwrap();
    assert_eq!((angle.default, angle.min, angle.max), (0.0, -180.0, 180.0));
    assert_eq!(DESCRIPTORS[0].name, "blur_sigma");
    assert_eq!(DESCRIPTORS.last().unwrap().name, "neighbor_falloff");
    let material = Material {
        blur_sigma: f32::NAN,
        effect_scale: f32::INFINITY,
        inner_refraction_amount: -1000.0,
        refraction_threshold0: 160.0,
        refraction_threshold1: 0.0,
        ..Material::default()
    }
    .sanitized();
    assert_eq!(material.blur_sigma, 19.8_f32);
    assert_eq!(material.effect_scale, 1.0);
    assert_eq!(material.inner_refraction_amount, -120.0);
    assert_eq!(material.refraction_threshold1, 160.0_f32 + 0.01_f32);
    for scale in [f32::NAN, f32::INFINITY, -1.0, f32::MAX] {
        assert_eq!(padding(material, scale), None);
    }
}

#[test]
fn checked_geometry_before_narrowing() {
    for value in [
        f64::NAN,
        f64::INFINITY,
        f64::NEG_INFINITY,
        0.0,
        -1.0,
        1.5,
        f64::from(i32::MAX),
    ] {
        assert!(!dimension(value));
    }
    assert!(dimension(f64::from(i32::MAX - 1)));
    assert!(integer_coordinate(f64::from(i32::MIN)));
    assert!(integer_coordinate(f64::from(i32::MAX)));
    assert!(!integer_coordinate(f64::from(i32::MAX) + 1.0));
    let geometry = sample_geometry(-10.25, 15.5, 640.25, 480.75, 60).unwrap();
    assert_eq!(
        geometry,
        SampleGeometry {
            width: 761,
            height: 601,
            x0: -71,
            y0: -45,
            x1: 690,
            y1: 556,
            owner: [-11, 15, 630, 497],
        }
    );
    assert_eq!(sample_geometry(f64::from(i32::MIN), 0.0, 1.0, 1.0, 1), None);
    assert_eq!(sample_geometry(0.0, 0.0, 1.0, 1.0, i32::MAX), None);
    assert_eq!(sample_geometry(0.0, 0.0, 1.0, 1.0, -1), None);
}

#[test]
fn blit_mapping_handles_full_integer_range() {
    assert_eq!(map_blit_coordinate(0, 1, 1, 0, 100), None);
    assert_eq!(
        map_blit_coordinate(i32::MAX, i32::MIN, i32::MAX, i32::MIN, i32::MAX),
        Some(i32::MAX)
    );
    assert_eq!(
        map_blit_coordinate(i32::MIN, i32::MIN, i32::MAX, i32::MAX, i32::MIN),
        Some(i32::MAX)
    );
    assert_eq!(map_blit_coordinate(i32::MAX, 0, 1, 0, i32::MAX), None);
    assert_eq!(map_blit_coordinate(1, 0, 3, 10, 0), Some(7)); // truncates toward zero
}

#[test]
fn allocation_size_checks_every_product() {
    assert_eq!(buffer_bytes(1920, 1080, 1, 1, 4), Some(8_294_400));
    assert_eq!(buffer_bytes(7, 5, 4, 4, 16), Some(64));
    assert_eq!(buffer_bytes(i32::MAX, i32::MAX, 1, 1, 8), None);
    assert_eq!(
        buffer_bytes(i32::MAX, i32::MAX, 1, 1, 4),
        Some(18_446_744_056_529_682_436)
    );
    assert_eq!(buffer_bytes(-1, 100, 1, 1, 4), None);
    assert_eq!(buffer_bytes(100, 100, 0, 1, 4), None);
    assert_eq!(buffer_bytes(100, 100, 1, 0, 4), None);
    assert_eq!(buffer_bytes(100, 100, 1, 1, 0), None);
}
