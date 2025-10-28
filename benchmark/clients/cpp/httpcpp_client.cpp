#include <climits>

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <string_view>
#include <chrono>
#include <numeric>
#include <iomanip>
#include <sstream>
#include <algorithm>

#include <boost/program_options.hpp>
#include <httpcpp/httpcpp.hpp>

namespace po = boost::program_options;
using namespace httpcpp;

struct Config {
    std::string host;
    uint16_t port;
    std::string transport_type = "tcp";
    uint64_t num_requests = 1000;
    std::string data_file = "benchmark_data.bin";
    std::string output_file = "latencies_httpcpp.bin";
    bool verify = true;
    bool unsafe_res = false;
};

struct BenchmarkData {
    uint64_t num_requests;
    std::vector<uint64_t> sizes;
    std::string data_block;
};

bool parse_args(int argc, char* argv[], Config& config) {
    try {
        po::options_description desc("httpcpp Benchmark Client Options");
        desc.add_options()
            ("help,h", "Show this help message")
            ("host", po::value<std::string>(&config.host)->required(), "The server host (e.g., 127.0.0.1) or path to Unix socket.")
            ("port", po::value<uint16_t>(&config.port)->required(), "The server port (ignored for Unix sockets).")
            ("transport", po::value<std::string>(&config.transport_type)->default_value("tcp"), "Transport to use: 'tcp' or 'unix'")
            ("num-requests", po::value<uint64_t>(&config.num_requests)->default_value(1000), "Number of requests to make.")
            ("data-file", po::value<std::string>(&config.data_file)->default_value("benchmark_data.bin"), "Path to the pre-generated data file.")
            ("output-file", po::value<std::string>(&config.output_file)->default_value("latencies_httpcpp.bin"), "File to save raw latency data to.")
            ("no-verify", po::bool_switch()->default_value(false), "Disable checksum validation.")
            ("unsafe", po::bool_switch()->default_value(false), "Use the unsafe/zero-copy response model.")
        ;

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("help")) {
            std::cout << desc << std::endl;
            return false;
        }

        po::notify(vm);
        config.verify = !vm["no-verify"].as<bool>();
        config.unsafe_res = vm["unsafe"].as<bool>();

    } catch (const po::error& e) {
        std::cerr << "Error parsing arguments: " << e.what() << std::endl;
        return false;
    }
    return true;
}

bool read_benchmark_data(const std::string& filename, BenchmarkData& data) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        std::cerr << "Error: Could not open benchmark data file " << filename << std::endl;
        return false;
    }

    file.read(reinterpret_cast<char*>(&data.num_requests), sizeof(uint64_t));
    data.sizes.resize(data.num_requests);
    file.read(reinterpret_cast<char*>(data.sizes.data()), data.num_requests * sizeof(uint64_t));

    auto current_pos = file.tellg();
    file.seekg(0, std::ios::end);
    auto end_pos = file.tellg();
    file.seekg(current_pos);

    size_t data_block_size = end_pos - current_pos;
    data.data_block.resize(data_block_size);
    file.read(data.data_block.data(), data_block_size);

    return file.good();
}

static inline uint64_t rotr64(uint64_t x, int s) {
    const int N = sizeof(uint64_t) * CHAR_BIT;
    int r = s % N;
    if (r < 0) {
        r += N;
    }
    if (r == 0) {
        return x;
    }

    return (x >> r) | (x << (N - r));
}

uint64_t xor_checksum(const char* data, size_t len) {
    uint64_t checksum = 0;
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < len; ++i) {
        checksum = rotr64(checksum, 7) ^ p[i];
    }
    return checksum;
}

uint64_t get_nanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}

template <typename Client>
void run_benchmark(Client& client, const Config& config, const BenchmarkData& data, std::vector<int64_t>& latencies) {
    std::vector<std::byte> payload_buffer;

    for (uint64_t i = 0; i < config.num_requests; ++i) {
        size_t req_size = data.sizes[i % data.sizes.size()];
        std::string_view body_slice(data.data_block.data(), req_size);

        HttpRequest request{};
        request.path = "/";

        // Prepare payload
        payload_buffer.resize(body_slice.size());
        std::transform(body_slice.begin(), body_slice.end(), payload_buffer.begin(), [](char c){ return std::byte(c); });

        if (config.verify) {
            uint64_t checksum = xor_checksum(reinterpret_cast<const char*>(payload_buffer.data()), payload_buffer.size());
            std::stringstream ss;
            ss << std::hex << std::setw(16) << std::setfill('0') << checksum;
            std::string checksum_hex = ss.str();
            payload_buffer.insert(payload_buffer.end(),
                                  reinterpret_cast<const std::byte*>(checksum_hex.data()),
                                  reinterpret_cast<const std::byte*>(checksum_hex.data()) + checksum_hex.size());
        }

        request.body = payload_buffer;
        request.headers.emplace_back("Content-Length", std::to_string(payload_buffer.size()));

        uint64_t client_receive_time = 0;

        if (config.unsafe_res) {
            auto result = client.post_unsafe(request);
            client_receive_time = get_nanoseconds();
            if (!result) { std::cerr << "Request failed!" << std::endl; break; }

            const auto& res = *result;
            if (config.verify) {
                auto res_payload = res.body.subspan(0, res.body.size() - 35);
                auto res_checksum = res.body.subspan(res.body.size() - 35, 16);
                uint64_t calculated = xor_checksum(reinterpret_cast<const char*>(res_payload.data()), res_payload.size());
                uint64_t received = 0;
                std::string_view hex_view(reinterpret_cast<const char*>(res_checksum.data()), res_checksum.size());
                std::from_chars(hex_view.data(), hex_view.data() + hex_view.size(), received, 16);
                if (calculated != received) std::cerr << "Warning: Checksum mismatch!" << std::endl;
            }
            auto ts_span = res.body.subspan(res.body.size() - 19);
            std::string_view ts_view(reinterpret_cast<const char*>(ts_span.data()), ts_span.size());
            latencies[i] = client_receive_time - std::stoull(std::string(ts_view));

        } else { // Safe response
            auto result = client.post_safe(request);
            client_receive_time = get_nanoseconds();
            if (!result) { std::cerr << "Request failed!" << std::endl; break; }

            const auto& res = *result;
            if (config.verify) {
                std::span<const std::byte> body_span(res.body);
                auto res_payload = body_span.subspan(0, body_span.size() - 35);
                auto res_checksum = body_span.subspan(body_span.size() - 35, 16);
                uint64_t calculated = xor_checksum(reinterpret_cast<const char*>(res_payload.data()), res_payload.size());;
                uint64_t received = 0;
                std::string_view hex_view(reinterpret_cast<const char*>(res_checksum.data()), res_checksum.size());
                std::from_chars(hex_view.data(), hex_view.data() + hex_view.size(), received, 16);
                if (calculated != received) std::cerr << "Warning: Checksum mismatch!" << std::endl;
            }
            std::span<const std::byte> body_span(res.body);
            auto ts_span = body_span.subspan(body_span.size() - 19);
            std::string_view ts_view(reinterpret_cast<const char*>(ts_span.data()), ts_span.size());
            latencies[i] = client_receive_time - std::stoull(std::string(ts_view));
        }
    }
}

int main(int argc, char* argv[]) {
    Config config;
    if (!parse_args(argc, argv, config)) {
        return 1;
    }

    BenchmarkData data;
    if (!read_benchmark_data(config.data_file, data)) {
        return 1;
    }

    std::vector<int64_t> latencies(config.num_requests);

    if (config.transport_type == "tcp") {
        HttpClient<Http1Protocol<TcpTransport>> client;
        if (!client.connect(config.host.c_str(), config.port)) {
             std::cerr << "Failed to connect" << std::endl; return 1;
        }
        run_benchmark(client, config, data, latencies);
        (void)client.disconnect();
    } else if (config.transport_type == "unix") {
        HttpClient<Http1Protocol<UnixTransport>> client;
        if (!client.connect(config.host.c_str(), 0)) {
            std::cerr << "Failed to connect" << std::endl; return 1;
        }
        run_benchmark(client, config, data, latencies);
        (void)client.disconnect();
    }

    std::ofstream out_file(config.output_file, std::ios::binary);
    if (out_file) {
        out_file.write(reinterpret_cast<const char*>(latencies.data()), latencies.size() * sizeof(int64_t));
    }

    std::cout << "httpcpp_client: completed " << config.num_requests << " requests." << std::endl;

    return 0;
}