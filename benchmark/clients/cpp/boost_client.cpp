#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <string_view>
#include <chrono>
#include <numeric>
#include <iomanip>
#include <sstream>

#include <boost/program_options.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

namespace po = boost::program_options;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using local = asio::local::stream_protocol;

struct Config {
    std::string host;
    uint16_t port;
    std::string transport_type = "tcp";
    uint64_t num_requests = 1000;
    std::string data_file = "benchmark_data.bin";
    std::string output_file = "latencies_boost.bin";
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
        po::options_description desc("Boost.Beast Benchmark Client Options");
        desc.add_options()
            ("help,h", "Show this help message")
            ("host", po::value<std::string>(&config.host)->required(), "The server host (e.g., 127.0.0.1) or path to Unix socket.")
            ("port", po::value<uint16_t>(&config.port)->required(), "The server port (ignored for Unix sockets).")
            ("transport", po::value<std::string>(&config.transport_type)->default_value("tcp"), "Transport to use: 'tcp' or 'unix'")
            ("num-requests", po::value<uint64_t>(&config.num_requests)->default_value(1000), "Number of requests to make.")
            ("data-file", po::value<std::string>(&config.data_file)->default_value("benchmark_data.bin"), "Path to the pre-generated data file.")
            ("output-file", po::value<std::string>(&config.output_file)->default_value("latencies_boost.bin"), "File to save raw latency data to.")
            ("no-verify", po::bool_switch()->default_value(false), "Disable checksum validation.")
            ("unsafe", po::bool_switch()->default_value(false), "Use non-owning span_body for requests (zero-copy send).")
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

uint64_t xor_checksum(std::string_view data) {
    return std::accumulate(data.begin(), data.end(), std::uint64_t{0}, [](uint64_t acc, char c) {
        return std::rotr(acc, 7) ^ static_cast<unsigned char>(c);
    });
}

uint64_t get_nanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}

template<class Stream>
void run_benchmark(Stream& stream, const Config& config, const BenchmarkData& data, std::vector<int64_t>& latencies) {
    std::string payload_buffer;
    beast::error_code ec;
    beast::flat_buffer buffer;

    for (uint64_t i = 0; i < config.num_requests; ++i) {
        size_t req_size = data.sizes[i % data.sizes.size()];
        std::string_view body_slice(data.data_block.data(), req_size);

        if (config.verify) {
            http::request<http::string_body> req{http::verb::post, "/", 11};
            req.set(http::field::host, config.host);
            req.keep_alive(true);
            uint64_t checksum = xor_checksum(body_slice);
            std::stringstream ss;
            ss << std::hex << std::setw(16) << std::setfill('0') << checksum;
            payload_buffer.assign(body_slice);
            payload_buffer.append(ss.str());
            req.body() = payload_buffer;
            req.prepare_payload();
            http::write(stream, req, ec);
        } else if (config.unsafe_res) {
            http::request<http::span_body<const char>> req{http::verb::post, "/", 11};
            req.set(http::field::host, config.host);
            req.keep_alive(true);
            req.body() = {body_slice.data(), body_slice.size()};
            req.prepare_payload();
            http::write(stream, req, ec);
        } else {
            http::request<http::string_body> req{http::verb::post, "/", 11};
            req.set(http::field::host, config.host);
            req.keep_alive(true);
            req.body().assign(body_slice.data(), body_slice.size());
            req.prepare_payload();
            http::write(stream, req, ec);
        }

        if(ec) { std::cerr << "Write failed: " << ec.message() << std::endl; break; }

        http::response_parser<http::empty_body> parser;
        parser.skip(true);
        http::read_header(stream, buffer, parser, ec);
        if(ec) { std::cerr << "Read header failed: " << ec.message() << std::endl; break; }

        if (parser.content_length()) {
            size_t body_size = *parser.content_length();
            buffer.reserve(body_size);
            while(buffer.size() < body_size) {
                size_t bytes_to_read = body_size - buffer.size();
                size_t bytes_read = stream.read_some(buffer.prepare(bytes_to_read), ec);
                buffer.commit(bytes_read);
                if(ec == http::error::end_of_stream) break;
                if(ec) { std::cerr << "Read body failed: " << ec.message() << std::endl; return; }
            }
        }
        auto client_receive_time = get_nanoseconds();
        std::string_view body(static_cast<const char*>(buffer.data().data()), buffer.size());

        if (config.verify) {
            if (body.length() < 35) {
                std::cerr << "Warning: Response body too short on request " << i << std::endl;
            } else {
                auto res_payload = body.substr(0, body.length() - 35);
                auto res_checksum_hex = body.substr(body.length() - 35, 16);
                uint64_t calculated = xor_checksum(res_payload);
                uint64_t received = 0;
                std::stringstream ss;
                ss << std::hex << res_checksum_hex;
                ss >> received;
                if (calculated != received) {
                    std::cerr << "Warning: Response checksum mismatch on request " << i << std::endl;
                }
            }
        }

        auto server_timestamp_str = body.substr(body.length() - 19);
        uint64_t server_timestamp = std::stoull(std::string(server_timestamp_str));
        latencies[i] = client_receive_time - server_timestamp;

        buffer.consume(buffer.size());
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

    asio::io_context ioc;
    beast::error_code ec;

    std::vector<int64_t> latencies(config.num_requests);

    if (config.transport_type == "tcp") {
        tcp::resolver resolver(ioc);
        tcp::socket socket(ioc);
        auto const results = resolver.resolve(config.host, std::to_string(config.port));
        asio::connect(socket, results.begin(), results.end());
        run_benchmark(socket, config, data, latencies);
        socket.shutdown(tcp::socket::shutdown_both, ec);
    } else if (config.transport_type == "unix") {
        local::socket socket(ioc);
        socket.connect(config.host); // For Unix, host is the path
        run_benchmark(socket, config, data, latencies);
        socket.shutdown(local::socket::shutdown_both, ec);
    }

    std::ofstream out_file(config.output_file, std::ios::binary);
    if (out_file) {
        out_file.write(reinterpret_cast<const char*>(latencies.data()), latencies.size() * sizeof(int64_t));
    }

    std::cout << "boost_client: completed " << config.num_requests << " requests." << std::endl;

    return 0;
}