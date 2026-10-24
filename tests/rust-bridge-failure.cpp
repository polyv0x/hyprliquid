#include "hyprliquid-bridge/ffi.rs.h"
#include <cassert>
#include <iostream>

int main() {
    namespace ffi = hyprliquid::ffi;
    const auto before = ffi::liquid_control_defaults(true);
    assert(before.status == ffi::Status::Ok && before.value.blur_sigma == 6);
    const auto failure = ffi::panic_probe();
    assert(failure.status == ffi::Status::InternalFailure);
    assert(failure.value.blur_sigma == 0);
    const auto invalid = ffi::map_blit_coordinate(0, 1, 1, 0, 100);
    assert(invalid.status == ffi::Status::InvalidGeometry);
    const auto after = ffi::liquid_control_defaults(true);
    assert(after.status == ffi::Status::Ok && after.value.blur_sigma == before.value.blur_sigma);
    std::cout << "PASS: caught Rust panic and invalid geometry return explicit CXX statuses; subsequent calls succeed\n";
}
