#include "emulator.h"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    std::filesystem::path binary = "Spooky Pop";
    std::filesystem::path external_root = "external";
    bool run_initializers = true;
    bool run_main = true;
    bool trace_shims = false;
    std::uint64_t tick_budget = 50'000'000;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--binary" && i + 1 < argc) {
            binary = argv[++i];
        } else if (arg == "--external" && i + 1 < argc) {
            external_root = argv[++i];
        } else if (arg == "--run-inits") {
            run_initializers = true;
        } else if (arg == "--skip-inits") {
            run_initializers = false;
        } else if (arg == "--skip-main") {
            run_main = false;
        } else if (arg == "--trace-shims") {
            trace_shims = true;
        } else if (arg == "--ticks" && i + 1 < argc) {
            tick_budget = std::strtoull(argv[++i], nullptr, 10);
        } else {
            std::cerr << "usage: spookypop_loader [--binary <path>] [--external <path>] [--run-inits] [--skip-inits] [--skip-main] [--trace-shims] [--ticks <n>]\n";
            return 2;
        }
    }

    try {
        Emulator emulator(EmulatorOptions{
            .binary_path = std::move(binary),
            .external_root = std::move(external_root),
            .trace_shims = trace_shims,
        });
        return emulator.Run(run_initializers, run_main, tick_budget);
    } catch (const std::exception& exception) {
        std::cerr << "fatal: " << exception.what() << '\n';
        return 1;
    }
}
