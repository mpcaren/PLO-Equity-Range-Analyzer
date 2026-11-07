fn main() {
    // fluent-dark: the standard widgets we use (ScrollView, AboutSlint) match the dark theme
    let config = slint_build::CompilerConfiguration::new().with_style("fluent-dark".into());
    slint_build::compile_with_config("ui/app.slint", config).expect("compile the Slint UI");
}
