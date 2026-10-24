use std::{env, path::PathBuf, process::Command};

fn main() {
    let root = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap()).join("../..");
    let status = Command::new("python3")
        .arg(root.join("scripts/embed-rust.py"))
        .arg("core")
        .arg(env::var_os("OUT_DIR").unwrap())
        .status()
        .expect("Python 3 is required to generate material definitions");
    assert!(status.success(), "material generation failed");
    for input in [
        "scripts/embed-rust.py",
        "src/MaterialParams.inc",
        "materials/liquid.json",
    ] {
        println!("cargo:rerun-if-changed={}", root.join(input).display());
    }
}
