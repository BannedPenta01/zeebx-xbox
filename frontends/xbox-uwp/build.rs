fn main() {
    // Um `staticlib` não repassa `cargo:rustc-link-lib` ao ligador do aplicativo. O Visual Studio
    // nomeia as bibliotecas na linha dele — ver `AdditionalDependencies` no `.vcxproj`. Estas
    // linhas existem para o `cargo test` deste pacote, que liga o binário de teste **no host**
    // (x86_64-pc-windows-msvc) e precisa do que o SQLite embutido e o Windows pedem:
    //
    // - `ws2_32`: sockets, que o `ureq`/`rustls` do núcleo referenciam mesmo sem a tela de
    //   atualização ligada (o `ring` faz as chamadas de entropia);
    // - `userenv`, `bcrypt`, `ntdll`: o que o `std` do Windows puxa;
    // - `advapi32`, `ole32`, `oleaut32`: o registro e o COM, usados pelo `ab_glyph`/`resvg`.
    //
    // Fora do Windows estas linhas não têm sentido — e um `cargo test --workspace` no Linux
    // pararia aqui.
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows") {
        return;
    }
    for biblioteca in [
        "ws2_32", "userenv", "bcrypt", "ntdll", "advapi32", "ole32", "oleaut32", "shell32",
    ] {
        println!("cargo:rustc-link-lib={biblioteca}");
    }
}
