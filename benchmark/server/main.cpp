#include <utility>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/program_options.hpp>
#include <iostream>
#include <random>
#include <spanstream>
#include <array>
#include <format>

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
namespace po    = boost::program_options;
using tcp       = net::ip::tcp;
using local     = net::local::stream_protocol;

struct Config {
    std::string    transport_type   = "tcp";
    uint32_t       seed             = 1234;
    int            num_responses    = 100;
    size_t         min_length       = 1024;
    size_t         max_length       = 1024 * 1024;
    std::string    host             = "127.0.0.1";
    unsigned short port             = 8080;
    std::string    unix_socket_path = "/tmp/httpc_benchmark.sock";
    bool           verify           = false;
};

struct ResponseCache {
    std::string                                   data_block;
    std::vector<beast::string_view>               body_views;
    std::vector<http::response<http::empty_body>> header_templates;
};

bool parse_args(int argc, char* argv[], Config& config) {
    try {
        po::options_description desc("Benchmark Server Options");
        // clang-format off
        desc.add_options()
            ("help,h", "Show this help message")
            ("transport", po::value<std::string>(&config.transport_type)->default_value("tcp"), "Transport to use: 'tcp' or 'unix'")
            ("seed", po::value<uint32_t>(&config.seed)->default_value(1234), "Seed for the PRNG")
            ("verify", po::value<bool>(&config.verify)->default_value(true), "Include checksum calculations")
            ("num-responses", po::value<int>(&config.num_responses)->default_value(100), "Number of response templates to generate")
            ("min-length", po::value<size_t>(&config.min_length)->default_value(1024), "Minimum response body size in bytes")
            ("max-length", po::value<size_t>(&config.max_length)->default_value(1024 * 1024), "Maximum response body size in bytes")
            ("host", po::value<std::string>(&config.host)->default_value("127.0.0.1"), "Host to bind for TCP transport")
            ("port", po::value<unsigned short>(&config.port)->default_value(8080), "Port to bind for TCP transport")
            ("unix-socket-path", po::value<std::string>(&config.unix_socket_path)->default_value("/tmp/httpc_benchmark.sock"), "Path for the Unix domain socket")
        ;
        // clang-format on

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);

        if (vm.count("help")) {
            std::cout << desc << std::endl;
            return false;
        }

        if (config.transport_type != "tcp" && config.transport_type != "unix") {
            std::cerr << "Error: --transport must be either 'tcp' or 'unix'." << std::endl;
            return false;
        }

    } catch (po::error const& e) {
        std::cerr << "Error parsing arguments: " << e.what() << std::endl;
        return false;
    }

    return true;
}

uint64_t xor_checksum(beast::string_view body) {
    uint64_t             sum  = 0;
    unsigned char const* data = reinterpret_cast<unsigned char const*>(body.data());
    for (size_t i = 0; i < body.size(); ++i) {
        sum = std::rotr(sum, 7) ^ data[i];
    }
    return sum;
}

std::string get_timestamp_str() {
    auto const now = std::chrono::high_resolution_clock::now();
    auto const ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    return std::to_string(ns);
}

ResponseCache generate_responses(Config const& config) {
    ResponseCache cache;
    std::mt19937  gen(config.seed);

    if (config.min_length > config.max_length) {
        std::cerr << "Error: --min-length cannot be greater than --max-length." << std::endl;
        return {};
    }

    cache.data_block.resize(config.max_length);
    std::uniform_int_distribution<char> char_dist(32, 126);
    for (char& c : cache.data_block) {
        c = char_dist(gen);
    }

    std::uniform_int_distribution<size_t> len_dist(config.min_length, config.max_length);

    cache.body_views.reserve(config.num_responses);
    cache.header_templates.reserve(config.num_responses);

    for (int i = 0; i < config.num_responses; ++i) {
        size_t body_len     = len_dist(gen);
        size_t start_offset = 0;
        if (config.max_length > body_len) {
            std::uniform_int_distribution<size_t> offset_dist(0, config.max_length - body_len);
            start_offset = offset_dist(gen);
        }

        cache.body_views.emplace_back(&cache.data_block[start_offset], body_len);

        http::response<http::empty_body> header_template;
        header_template.version(11);
        header_template.result(http::status::ok);
        header_template.set(http::field::server, "BenchmarkServer");
        header_template.set(http::field::content_type, "application/octet-stream");
        cache.header_templates.push_back(header_template);
    }

    std::cout << "Generated " << config.num_responses << " response views into a single data block."
              << std::endl;
    return cache;
}

template <class Stream> void do_session(Stream& stream, ResponseCache const& cache, Config const& config) {
    // *** Use flat_buffer, like the client ***
    beast::flat_buffer buffer;
    buffer.reserve(1024 * 1024 + 16); // Reserve once

    beast::error_code ec;
    std::size_t       count = 0;

    if constexpr (std::is_same_v<typename Stream::protocol_type, tcp>) { // Check if it's TCP
        stream.set_option(tcp::no_delay(true), ec);                      // Set the option
        if (ec) {
            // Log a warning if setting fails, but continue the session
            std::cerr << "Warning: Failed to set TCP_NODELAY on accepted socket: " << ec.message()
                      << std::endl;
            ec.clear(); // Clear the error code to allow the session to proceed
        }
    }

    for (;;) { // Loop for multiple requests
        http::request<http::string_body> request;

        // *** 2. Read ONLY the headers into the buffer ***
        http::read(stream, buffer, request, ec);

        // --- Error Handling for header read ---
        if (ec == http::error::end_of_stream || ec == http::error::end_of_stream) {
            break;
        } // Graceful close
        if (ec) {
            std::cerr << "Session read error: " << ec.message() << std::endl;
            break;
        }

        if (request.method() == http::verb::post && !request.has_content_length()) {
            std::cerr << "Error: POST request missing Content-Length." << std::endl;
            break;
        }

        // --- Body View Creation ---
        // *** Create view from the accumulated temporary storage ***
        beast::string_view req_body_view(request.body());

        // --- Verification Logic ---
        if (config.verify && req_body_view.size() >= 16) {
            // --- Checksum Calculation ---
            size_t payload_len           = (req_body_view.size() >= 16) ? req_body_view.size() - 16 : 0;
            auto   payload_view          = req_body_view.substr(0, payload_len);
            auto   received_checksum_hex = req_body_view.substr(payload_len);

            uint64_t calculated_checksum = xor_checksum(payload_view);
            uint64_t received_checksum   = 0;
            if (received_checksum_hex.size() == 16) {
                std::ispanstream(received_checksum_hex) >> std::hex >> received_checksum;
            } else {
                std::cerr << "Warning: Received checksum hex size is not 16 (" << received_checksum_hex.size()
                          << ")" << std::endl;
            }

            if (calculated_checksum != received_checksum) {
                std::cerr << "Warning: Checksum mismatch from client!" << std::endl;
            }
        }

        // --- Response Sending Logic (Unchanged) ---
        auto const& header_template = cache.header_templates[0];
        auto const& body_view       = cache.body_views[0]; // Server's response body
        std::string ts_str          = get_timestamp_str();

        if (config.verify) {
            uint64_t checksum_val = xor_checksum(body_view);

            http::response<http::string_body> res;
            res.base() = header_template;
            res.body().reserve(body_view.size() + 16 + ts_str.size());
            res.body().append(body_view.data(), body_view.size());

            std::format_to(back_inserter(res.body()), "{:016X}", checksum_val);
            res.body().append(ts_str);
            res.prepare_payload();
            http::write(stream, res, ec);
        } else {
            http::response<http::span_body<char const>> res;
            res.base() = header_template;
            // res.body() = body_view;
            res.set(http::field::content_length, std::to_string(body_view.size() + ts_str.size()));
            http::serializer<false, decltype(res)::body_type> sr{res};
            http::write_header(stream, sr, ec);
            if (!ec) {
                std::array<net::const_buffer, 2> buffers_to_write = {
                    net::buffer(body_view.data(), body_view.size()),
                    net::buffer(ts_str.data(), ts_str.size())
                };
                // Pass the explicitly typed array to asio::write
                write(stream, buffers_to_write, ec);
            }
        }

        if (ec) {
            std::cerr << "Session write error: " << ec.message() << std::endl;
            break;
        }

        // --- Loop Control ---
        bool const keep_alive = request.keep_alive();

        // *** Buffer should be empty now after incremental consumption ***
        if (buffer.size() > 0) {
            std::cerr << "Warning: Buffer not empty at end of loop iteration. Consuming remaining "
                      << buffer.size() << " bytes." << std::endl;
            buffer.consume(buffer.size());
        }

        ++count;
        if (count == config.num_responses) {
            break;
        } // Exit after N requests
        if (!keep_alive) {
            break;
        } // Exit if client doesn't want keep-alive

    } // End of for(;;) loop

    // --- Shutdown Logic (Unchanged) ---
    if constexpr (std::is_same_v<typename Stream::protocol_type, tcp>) {
        stream.shutdown(tcp::socket::shutdown_send, ec);
    } else {
        stream.shutdown(local::socket::shutdown_send, ec);
    }
} // End of do_session

template <class Acceptor, class Endpoint>
void do_listen(net::io_context& ioc, Endpoint const& endpoint, ResponseCache const& cache,
               Config const& config) {
    beast::error_code ec;
    Acceptor          acceptor(ioc);

    acceptor.open(endpoint.protocol(), ec);
    if (ec) {
        std::cerr << "Failed to open acceptor: " << ec.message() << std::endl;
        return;
    }

    acceptor.set_option(net::socket_base::reuse_address(true), ec);
    if (ec) {
        std::cerr << "Failed to set reuse_address: " << ec.message() << std::endl;
        return;
    }

    acceptor.bind(endpoint, ec);
    if (ec) {
        std::cerr << "Failed to bind to endpoint: " << ec.message() << std::endl;
        return;
    }

    acceptor.listen(net::socket_base::max_listen_connections, ec);
    if (ec) {
        std::cerr << "Failed to listen on endpoint: " << ec.message() << std::endl;
        return;
    }

    std::cout << "Server listening for connections..." << std::endl;
    for (;;) {
        auto socket = acceptor.accept(ioc, ec);
        if (ec) {
            std::cerr << "Failed to accept connection: " << ec.message() << std::endl;
            break;
        }
        do_session(socket, cache, config);
        break;
    }
}

int main(int argc, char* argv[]) {
    Config config;
    if (!parse_args(argc, argv, config)) {
        return 1;
    }

    auto response_cache = generate_responses(config);
    if (response_cache.body_views.empty()) {
        return 1;
    }

    net::io_context ioc(1);

    if (config.transport_type == "tcp") {
        auto const endpoint = tcp::endpoint{net::ip::make_address(config.host), config.port};
        do_listen<tcp::acceptor, tcp::endpoint>(ioc, endpoint, response_cache, config);
    } else if (config.transport_type == "unix") {
        std::remove(config.unix_socket_path.c_str());
        auto const endpoint = local::endpoint{config.unix_socket_path};
        do_listen<local::acceptor, local::endpoint>(ioc, endpoint, response_cache, config);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cout << "Server shutting down..." << std::endl;
}