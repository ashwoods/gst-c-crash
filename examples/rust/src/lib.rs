//! What the example programs share.

/// The command-line arguments after the program's name, with `--option=value`
/// split into `--option` and `value`, as argparse and GOption accept it.
pub fn args() -> impl Iterator<Item = String> {
    std::env::args()
        .skip(1)
        .flat_map(|arg| match arg.split_once('=') {
            Some((option, value)) if option.starts_with("--") => {
                vec![option.to_string(), value.to_string()]
            }
            _ => vec![arg],
        })
}
