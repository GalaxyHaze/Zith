const ITERATIONS: u64 = @@ITERATIONS@@;
const EXPECTED: u64 = @@EXPECTED@@;

fn main() -> std::process::ExitCode {
    let mut state: u64 = 88172645463325252;

    for _ in 0..ITERATIONS {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
    }

    if state == EXPECTED {
        std::process::ExitCode::SUCCESS
    } else {
        std::process::ExitCode::FAILURE
    }
}
