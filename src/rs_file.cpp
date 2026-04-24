#include "vdm_rs/reed_solomon.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace
{

struct Manifest
{
    std::size_t k = 0;
    std::size_t t = 0;
    std::size_t shard_size = 0;
    std::size_t file_size = 0;
};

// Wraps user-facing paths and tokens in single quotes for messages.
[[nodiscard]] auto quoted(std::string_view text) -> std::string
{
    return "'" + std::string(text) + "'";
}

// Builds a consistent filesystem-related runtime_error message.
[[nodiscard]] auto make_error(std::string_view message, std::string_view detail)
    -> std::runtime_error
{
    return std::runtime_error(std::string(message) + " " + quoted(detail));
}

// Formats the filename used for the shard at the given index.
[[nodiscard]] auto shard_filename(std::size_t index) -> std::string
{
    std::ostringstream oss;
    oss << "shard_" << std::setw(3) << std::setfill('0') << index << ".bin";
    return oss.str();
}

// Reads an entire file into memory.
[[nodiscard]] auto slurp_file(const fs::path& path) -> std::vector<std::uint8_t>
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw make_error("failed to open input file", path.string());
    }

    input.seekg(0, std::ios::end);
    const auto end_pos = input.tellg();
    if (end_pos < 0) {
        throw make_error("failed to stat input file", path.string());
    }

    const auto file_size = static_cast<std::size_t>(end_pos);
    input.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> data(file_size);
    if (!data.empty()) {
        input.read(reinterpret_cast<char*>(data.data()),
                   static_cast<std::streamsize>(data.size()));
        if (!input) {
            throw make_error("failed to read input file", path.string());
        }
    }

    return data;
}

// Writes a byte buffer to disk, replacing any existing file.
void write_file(const fs::path& path, const std::vector<std::uint8_t>& data)
{
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw make_error("failed to open output file", path.string());
    }

    if (!data.empty()) {
        output.write(reinterpret_cast<const char*>(data.data()),
                     static_cast<std::streamsize>(data.size()));
        if (!output) {
            throw make_error("failed to write output file", path.string());
        }
    }
}

// Persists the encode parameters needed for later reconstruction.
void write_manifest(const fs::path& output_dir, const Manifest& manifest)
{
    std::ofstream output(output_dir / "manifest.txt");
    if (!output) {
        throw make_error("failed to write manifest in", output_dir.string());
    }

    output << "k " << manifest.k << '\n';
    output << "t " << manifest.t << '\n';
    output << "shard_size " << manifest.shard_size << '\n';
    output << "file_size " << manifest.file_size << '\n';
}

// Reads the shard manifest written during encode.
[[nodiscard]] auto read_manifest(const fs::path& input_dir) -> Manifest
{
    std::ifstream input(input_dir / "manifest.txt");
    if (!input) {
        throw make_error("failed to open manifest in", input_dir.string());
    }

    Manifest manifest;
    std::string key;
    while (input >> key) {
        if (key == "k") {
            input >> manifest.k;
        } else if (key == "t") {
            input >> manifest.t;
        } else if (key == "shard_size") {
            input >> manifest.shard_size;
        } else if (key == "file_size") {
            input >> manifest.file_size;
        } else {
            throw make_error("unknown manifest key", key);
        }
    }

    if (manifest.k == 0 || manifest.t == 0 || manifest.shard_size == 0) {
        throw std::runtime_error("manifest is missing required fields");
    }

    return manifest;
}

// Computes ceil(numerator / denominator) for positive sizes.
[[nodiscard]] auto ceil_div(std::size_t numerator, std::size_t denominator)
    -> std::size_t
{
    return (numerator + denominator - 1) / denominator;
}

// Prints a simple throughput summary for encode or decode work.
void print_codec_timing(std::string_view label, std::size_t payload_bytes,
                        std::chrono::steady_clock::duration elapsed)
{
    const auto seconds = std::chrono::duration<double>(elapsed).count();
    const auto mib = static_cast<double>(payload_bytes) / (1024.0 * 1024.0);
    const auto mib_per_second = seconds > 0.0 ? mib / seconds : 0.0;
    const auto milliseconds
        = std::chrono::duration<double, std::milli>(elapsed).count();

    std::cout << label << " codec time: " << std::fixed << std::setprecision(3)
              << milliseconds << " ms (" << std::setprecision(2) << mib_per_second
              << " MiB/s over " << mib << " MiB payload)\n";
}

// Splits a file into data shards, computes parity, and writes everything to disk.
void encode_file(const fs::path& input_file, const fs::path& output_dir, std::size_t k,
                 std::size_t t)
{
    const auto codec = ReedSolomonCodec::create(k, t);
    const auto input = slurp_file(input_file);
    const auto shard_size = std::max<std::size_t>(1, ceil_div(input.size(), k));

    fs::create_directories(output_dir);

    std::vector<std::vector<std::uint8_t>> data_shards(
        k, std::vector<std::uint8_t>(shard_size, 0));
    std::vector<std::vector<std::uint8_t>> parity_shards(
        t, std::vector<std::uint8_t>(shard_size, 0));

    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto shard_index = i / shard_size;
        const auto offset = i % shard_size;
        data_shards[shard_index][offset] = input[i];
    }

    std::vector<Shard> data_views;
    data_views.reserve(k);
    for (auto& shard : data_shards) {
        data_views.push_back(Shard { shard.data(), shard.size() });
    }

    std::vector<Shard> parity_views;
    parity_views.reserve(t);
    for (auto& shard : parity_shards) {
        parity_views.push_back(Shard { shard.data(), shard.size() });
    }

    const auto encode_start = std::chrono::steady_clock::now();
    codec.compute_parity(data_views, parity_views);
    const auto encode_elapsed = std::chrono::steady_clock::now() - encode_start;

    write_manifest(
        output_dir,
        Manifest {
            .k = k, .t = t, .shard_size = shard_size, .file_size = input.size() });

    for (std::size_t i = 0; i < k; ++i) {
        write_file(output_dir / shard_filename(i), data_shards[i]);
    }
    for (std::size_t i = 0; i < t; ++i) {
        write_file(output_dir / shard_filename(k + i), parity_shards[i]);
    }

    std::cout << "encoded " << quoted(input_file.string()) << " into "
              << codec.total_shards() << " shards in " << quoted(output_dir.string())
              << '\n';
    print_codec_timing("encode", input.size(), encode_elapsed);
}

// Reconstructs the original file from whatever shards are present on disk.
void decode_file(const fs::path& input_dir, const fs::path& output_file)
{
    const auto manifest = read_manifest(input_dir);
    const auto codec = ReedSolomonCodec::create(manifest.k, manifest.t);
    const auto n = codec.total_shards();

    std::vector<std::vector<std::uint8_t>> shard_storage(
        n, std::vector<std::uint8_t>(manifest.shard_size, 0));
    std::vector<Shard> shard_views;
    shard_views.reserve(n);
    std::vector<bool> present(n, false);

    for (std::size_t i = 0; i < n; ++i) {
        shard_views.push_back(
            Shard { shard_storage[i].data(), shard_storage[i].size() });

        const auto path = input_dir / shard_filename(i);
        if (!fs::exists(path)) {
            continue;
        }

        auto bytes = slurp_file(path);
        if (bytes.size() != manifest.shard_size) {
            std::ostringstream oss;
            oss << "shard " << quoted(path.string()) << " has size " << bytes.size()
                << ", expected " << manifest.shard_size;
            throw std::runtime_error(oss.str());
        }
        std::copy(bytes.begin(), bytes.end(), shard_storage[i].begin());
        present[i] = true;
    }

    const auto decode_start = std::chrono::steady_clock::now();
    if (!codec.reconstruct(shard_views, present)) {
        throw std::runtime_error("not enough shards present to reconstruct file");
    }
    const auto decode_elapsed = std::chrono::steady_clock::now() - decode_start;

    std::vector<std::uint8_t> output;
    output.reserve(manifest.file_size);
    for (std::size_t i = 0; i < manifest.k && output.size() < manifest.file_size; ++i) {
        const auto bytes_to_copy
            = std::min(manifest.shard_size, manifest.file_size - output.size());
        output.insert(output.end(), shard_storage[i].begin(),
                      shard_storage[i].begin()
                          + static_cast<std::ptrdiff_t>(bytes_to_copy));
    }

    write_file(output_file, output);
    std::cout << "decoded " << quoted(output_file.string()) << " from "
              << quoted(input_dir.string()) << '\n';
    print_codec_timing("decode", manifest.file_size, decode_elapsed);
}

// Prints the small command-line interface for the file demo tool.
void print_usage(std::string_view program_name)
{
    std::cout << "usage:\n";
    std::cout << "  " << program_name << " encode <input-file> <output-dir> <k> <t>\n";
    std::cout << "  " << program_name << " decode <input-dir> <output-file>\n";
}

} // namespace

// Dispatches to the encode or decode subcommand.
int main(int argc, char* argv[])
{
    try {
        if (argc < 2) {
            print_usage(argv[0]);
            return 1;
        }

        const std::string_view command = argv[1];
        if (command == "encode") {
            if (argc != 6) {
                print_usage(argv[0]);
                return 1;
            }
            encode_file(argv[2], argv[3], std::stoull(argv[4]), std::stoull(argv[5]));
            return 0;
        }
        if (command == "decode") {
            if (argc != 4) {
                print_usage(argv[0]);
                return 1;
            }
            decode_file(argv[2], argv[3]);
            return 0;
        }

        print_usage(argv[0]);
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << '\n';
        return 1;
    }
}
