//! Value-only, synchronous CXX boundary. Every exported operation catches panics.
//! No callbacks, shared mutable state, GL, Lua, or host pointers cross this layer.
#[cfg(panic = "abort")]
compile_error!("hyprliquid's CXX panic boundary requires panic=unwind");

use hyprliquid_core as core;
use std::panic::{UnwindSafe, catch_unwind};

include!(concat!(env!("OUT_DIR"), "/ffi.rs"));

fn boundary<T: Default>(operation: impl FnOnce() -> Option<T> + UnwindSafe) -> (ffi::Status, T) {
    match catch_unwind(operation) {
        Ok(Some(value)) => (ffi::Status::Ok, value),
        Ok(None) => (ffi::Status::InvalidGeometry, T::default()),
        Err(payload) => {
            // An arbitrary panic payload can itself panic in Drop. Do not let
            // that second unwind cross CXX. Only this exceptional path leaks it.
            std::mem::forget(payload);
            (ffi::Status::InternalFailure, T::default())
        }
    }
}

fn liquid_control_defaults(dark: bool) -> ffi::MaterialResult {
    let (status, value) = boundary(|| Some(core::material::liquid_control_defaults(dark).into()));
    ffi::MaterialResult { status, value }
}

fn sanitize_material(material: ffi::Material) -> ffi::MaterialResult {
    let (status, value) = boundary(|| Some(core::Material::from(material).sanitized().into()));
    ffi::MaterialResult { status, value }
}

fn effective_liquid_material(material: ffi::Material) -> ffi::LiquidResult {
    let (status, value) =
        boundary(|| Some(core::material::effective_liquid_material(material.into()).into()));
    ffi::LiquidResult { status, value }
}

fn material_blur_sigma(material: ffi::Material) -> ffi::FloatResult {
    let (status, value) = boundary(|| Some(core::material::blur_sigma(material.into())));
    ffi::FloatResult { status, value }
}

fn material_output_extent(material: ffi::Material) -> ffi::FloatResult {
    let (status, value) = boundary(|| Some(core::material::output_extent(material.into())));
    ffi::FloatResult { status, value }
}

fn material_padding(material: ffi::Material, scale: f32) -> ffi::IntResult {
    let (status, value) = boundary(|| core::material::padding(material.into(), scale));
    ffi::IntResult { status, value }
}

fn integer_coordinate(value: f64) -> ffi::BoolResult {
    let (status, value) = boundary(|| Some(core::geometry::integer_coordinate(value)));
    ffi::BoolResult { status, value }
}

fn dimension(value: f64) -> ffi::BoolResult {
    let (status, value) = boundary(|| Some(core::geometry::dimension(value)));
    ffi::BoolResult { status, value }
}

fn sample_geometry(x: f64, y: f64, w: f64, h: f64, padding: i32) -> ffi::GeometryResult {
    let (status, value) = boundary(|| {
        core::geometry::sample_geometry(x, y, w, h, padding).map(|g| ffi::SampleGeometry {
            width: g.width,
            height: g.height,
            x0: g.x0,
            y0: g.y0,
            x1: g.x1,
            y1: g.y1,
            owner: g.owner,
        })
    });
    ffi::GeometryResult { status, value }
}

fn map_blit_coordinate(value: i32, from0: i32, from1: i32, to0: i32, to1: i32) -> ffi::IntResult {
    let (status, value) =
        boundary(|| core::geometry::map_blit_coordinate(value, from0, from1, to0, to1));
    ffi::IntResult { status, value }
}

fn buffer_bytes(
    width: i32,
    height: i32,
    block_width: u32,
    block_height: u32,
    bytes_per_block: u32,
) -> ffi::BytesResult {
    let (status, value) = boundary(|| {
        core::geometry::buffer_bytes(width, height, block_width, block_height, bytes_per_block)
    });
    ffi::BytesResult { status, value }
}

// This symbol is absent from production builds. The disposable C++ test calls
// it through the generated ABI, then verifies ordinary calls still succeed.
#[cfg(feature = "fault_injection")]
fn panic_probe() -> ffi::MaterialResult {
    let (status, value) = boundary(|| panic!("controlled Rust boundary test"));
    ffi::MaterialResult { status, value }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn failure_does_not_commit_a_partial_result() {
        let input = liquid_control_defaults(true).value;
        let (status, value) = boundary(|| {
            let mut candidate = input;
            candidate.blur_sigma = 42.0;
            std::hint::black_box(candidate);
            panic!("controlled panic before returning candidate");
            #[allow(unreachable_code)]
            Some(candidate)
        });
        assert_eq!(status, ffi::Status::InternalFailure);
        assert_eq!(value, ffi::Material::default());
        assert_eq!(input.blur_sigma, 6.0);
        assert_eq!(sanitize_material(input).value, input);
    }

    #[test]
    fn errors_are_distinct_from_panics() {
        assert_eq!(
            map_blit_coordinate(0, 1, 1, 0, 100).status,
            ffi::Status::InvalidGeometry
        );
        assert_eq!(
            sample_geometry(f64::NAN, 0.0, 1.0, 1.0, 0).status,
            ffi::Status::InvalidGeometry
        );
        assert_eq!(liquid_control_defaults(false).status, ffi::Status::Ok);
    }
}
