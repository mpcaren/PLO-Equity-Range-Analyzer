// With `--features c-reference`, compile the original C engine so the
// differential tests in tests/c_reference.rs can call it side by side.
fn main() {
    #[cfg(feature = "c-reference")]
    {
        let src = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("..").join("src");
        cc::Build::new()
            .file(src.join("plo5.c"))
            .file(src.join("spr.c"))
            .include(&src)
            .flag_if_supported("-std=c11")
            .flag_if_supported("-pthread")
            .opt_level(3)
            .warnings(false)
            .compile("plo5c");
        if !cfg!(target_os = "windows") {
            println!("cargo:rustc-link-lib=pthread");
            println!("cargo:rustc-link-lib=m");
        }
        println!("cargo:rerun-if-changed=../src/plo5.c");
        println!("cargo:rerun-if-changed=../src/spr.c");
        println!("cargo:rerun-if-changed=../src/plo5.h");
    }
}
