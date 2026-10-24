use std::{env, fs, path::PathBuf, process::Command};

fn main() {
    let root = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap()).join("../..");
    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    let status = Command::new("python3")
        .arg(root.join("scripts/embed-rust.py"))
        .arg("bridge")
        .arg(&out)
        .status()
        .expect("Python 3 is required to generate bridge definitions");
    assert!(status.success(), "bridge generation failed");
    // Generated declarations live in OUT_DIR, never in the source checkout.
    let source = out.join("ffi.rs");
    cxx_build::bridge(&source)
        .std("c++23")
        .pic(true)
        .flag_if_supported("-fno-gnu-unique")
        .compile("hyprliquid-cxx");
    // CXX uses the input path in its header name. Export a stable forwarding
    // header alongside its shared headers for Make's host adapter.
    let relative: PathBuf = source
        .components()
        .filter_map(|part| match part {
            std::path::Component::Normal(value) => Some(value),
            _ => None,
        })
        .collect();
    let shared = out.ancestors().nth(4).unwrap().join("cxxbridge");
    let header = shared
        .join("hyprliquid-bridge")
        .join(relative)
        .with_extension("rs.h");
    // Atomic replacement also permits a standalone Cargo check while Make is
    // compiling consumers of the same (feature-independent) production ABI.
    for (source, name) in [
        (header, "ffi.rs.h"),
        (out.join("LiquidConversions.hpp"), "LiquidConversions.hpp"),
    ] {
        let destination = shared.join("hyprliquid-bridge").join(name);
        let temporary = destination.with_extension(format!("tmp-{}", std::process::id()));
        fs::copy(source, &temporary).unwrap();
        fs::rename(temporary, destination).unwrap();
    }
    for input in [
        "scripts/embed-rust.py",
        "src/MaterialParams.inc",
        "materials/liquid.json",
        "crates/hyprliquid-bridge/src/ffi.rs.in",
    ] {
        println!("cargo:rerun-if-changed={}", root.join(input).display());
    }
}
