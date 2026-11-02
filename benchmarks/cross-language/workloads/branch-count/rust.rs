const ITERATIONS: u64 = @@ITERATIONS@@;
const EXPECTED: u64 = @@EXPECTED@@;

fn main() -> std::process::ExitCode {
    let mut count: u64 = 0;

    for value in 0..ITERATIONS {
        if value % 7 == 0 || value % 11 == 0 {
            count += 1;
        }
    }

    if count == EXPECTED {
        std::process::ExitCode::SUCCESS
    } else {
        std::process::ExitCode::FAILURE
    }
}
