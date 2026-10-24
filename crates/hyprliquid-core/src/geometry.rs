#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SampleGeometry {
    pub width: i32,
    pub height: i32,
    pub x0: i32,
    pub y0: i32,
    pub x1: i32,
    pub y1: i32,
    pub owner: [i32; 4],
}

pub fn integer_coordinate(value: f64) -> bool {
    value.is_finite() && value >= f64::from(i32::MIN) && value <= f64::from(i32::MAX)
}

pub fn dimension(value: f64) -> bool {
    // Leave room for rounding a half-resolution buffer with (size + 1) / 2.
    value.is_finite() && value >= 1.0 && value < f64::from(i32::MAX) && value.floor() == value
}

pub fn sample_geometry(x: f64, y: f64, w: f64, h: f64, padding: i32) -> Option<SampleGeometry> {
    if !w.is_finite() || !h.is_finite() || w <= 0.0 || h <= 0.0 || padding < 0 {
        return None;
    }
    let width = w.ceil() + 2.0 * f64::from(padding);
    let height = h.ceil() + 2.0 * f64::from(padding);
    let left = x.floor();
    let top = y.floor();
    let right = (x + w).ceil();
    let bottom = (y + h).ceil();
    let x0 = left - f64::from(padding);
    let y0 = top - f64::from(padding);
    let x1 = x0 + width;
    let y1 = y0 + height;
    if !dimension(width)
        || !dimension(height)
        || ![left, top, right, bottom, x0, y0, x1, y1]
            .into_iter()
            .all(integer_coordinate)
    {
        return None;
    }
    Some(SampleGeometry {
        width: width as i32,
        height: height as i32,
        x0: x0 as i32,
        y0: y0 as i32,
        x1: x1 as i32,
        y1: y1 as i32,
        owner: [left as i32, top as i32, right as i32, bottom as i32],
    })
}

pub fn map_blit_coordinate(value: i32, from0: i32, from1: i32, to0: i32, to1: i32) -> Option<i32> {
    // Differences of two i32 values fit i64, but their product may not. i128
    // preserves truncating division for all valid inputs, including reversed axes.
    let divisor = i128::from(from1) - i128::from(from0);
    if divisor == 0 {
        return None;
    }
    let result = i128::from(to0)
        + (i128::from(value) - i128::from(from0)) * (i128::from(to1) - i128::from(to0)) / divisor;
    i32::try_from(result).ok()
}

pub fn buffer_bytes(
    width: i32,
    height: i32,
    block_width: u32,
    block_height: u32,
    bytes_per_block: u32,
) -> Option<u64> {
    if width <= 0 || height <= 0 || block_width == 0 || block_height == 0 || bytes_per_block == 0 {
        return None;
    }
    let blocks = u64::try_from(width).ok()?.div_ceil(u64::from(block_width));
    let rows = u64::try_from(height)
        .ok()?
        .div_ceil(u64::from(block_height));
    blocks
        .checked_mul(u64::from(bytes_per_block))?
        .checked_mul(rows)
}
