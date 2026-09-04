#include "quality_metrics.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <stdexcept>

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 7) throw std::runtime_error("Use compare-quality.ps1 (validated metadata required)");
        const size_t frames = std::stoull(argv[4]), width = std::stoull(argv[5]), height = std::stoull(argv[6]);
        if (!frames || !width || !height || width > 8192 || height > 8192) throw std::runtime_error("Invalid dimensions/count");
        const size_t bytes = width * height * 3;
        const std::filesystem::path baseline = argv[1], candidate = argv[2], target = argv[3];
        const auto open = [&](const std::filesystem::path& p) {
            if (std::filesystem::file_size(p) != bytes * frames) throw std::runtime_error("Frame archive size mismatch");
            std::ifstream file(p, std::ios::binary); file.exceptions(std::ios::badbit | std::ios::failbit); return file;
        };
        auto bi = open(baseline / L"input.rgb"), bo = open(baseline / L"output.rgb");
        auto ci = open(candidate / L"input.rgb"), co = open(candidate / L"output.rgb");
        if (std::filesystem::exists(target)) throw std::runtime_error("Comparison output already exists");
        std::filesystem::create_directories(target);
        std::ofstream csv(target / L"comparison.csv"); csv.exceptions(std::ios::badbit | std::ios::failbit);
        csv << "index,baseline_candidate_mae,input_change,baseline_residual_change,candidate_residual_change,residual_increase,review_flag\n";
        std::vector<uint8_t> input(bytes), other_input(bytes), b(bytes), c(bytes), pi, pb, pc;
        double total = 0, maximum = 0; size_t flagged = 0;
        for (size_t n = 0; n < frames; ++n) {
            bi.read(reinterpret_cast<char*>(input.data()), bytes); ci.read(reinterpret_cast<char*>(other_input.data()), bytes);
            bo.read(reinterpret_cast<char*>(b.data()), bytes); co.read(reinterpret_cast<char*>(c.data()), bytes);
            if (input != other_input) throw std::runtime_error("Decoded input differs; comparison is not aligned");
            const double delta = quality_mae(b, c);
            const double input_change = n ? quality_mae(input, pi) : 0;
            const double br = n ? quality_residual_change(b, input, pb, pi) : 0;
            const double cr = n ? quality_residual_change(c, input, pc, pi) : 0;
            const bool review = delta > 5 || (n && cr - br > 3) || (n && input_change < 1 && cr > 3);
            csv << std::fixed << std::setprecision(4) << n << ',' << delta << ',' << input_change << ','
                << br << ',' << cr << ',' << cr - br << ',' << review << '\n';
            total += delta; maximum = std::max(maximum, delta); flagged += review;
            pi = input; pb = b; pc = c;
        }
        csv.close();
        std::ofstream summary(target / L"comparison.json"); summary.exceptions(std::ios::badbit | std::ios::failbit);
        summary << "{\"status\":\"complete\",\"frames\":" << frames << ",\"mean_mae\":" << total / frames
            << ",\"max_frame_mae\":" << maximum << ",\"review_frames\":" << flagged
            << ",\"scale\":\"RGB 0-255\",\"motion_compensated\":false,\"quality_pass\":null}\n";
        std::cout << "Comparison complete; flags require visual review\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
