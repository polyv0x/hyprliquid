//! Pure numerical material and geometry logic. No host pointers or callbacks.
#![forbid(unsafe_code)]

pub mod geometry;
pub mod material;

#[derive(Clone, Copy, Debug)]
pub struct Descriptor {
    pub name: &'static str,
    pub default: f32,
    pub min: f32,
    pub max: f32,
}

include!(concat!(env!("OUT_DIR"), "/material.rs"));

fn sanitize_scalar(value: f32, fallback: f32, min: f32, max: f32) -> f32 {
    if value.is_finite() {
        value.clamp(min, max)
    } else {
        fallback
    }
}
