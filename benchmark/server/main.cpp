#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <random>
#include <numeric>
#include <memory>
#include <fstream>
#include <iomanip>
#include <type_traits>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio.hpp>
#include <boost/program_options.hpp>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace po = boost::program_options;
using tcp = net::ip::tcp;
using local = net::local::stream_protocol;

struct Config {
    std::string transport_type = "tcp";
    uint32_t seed = 1234;
    int num_responses = 100;
    size_t min_length = 1024;
    size_t max_length = 1024 * 1024;
    std::string host = "127.0.0.1";
    unsigned short port = 8080;
    std::string unix_socket_path = "/tmp/httpc_benchmark.sock";
    bool verify = false;
};

struct ResponseCache {
    std::string data_block;
    std::vector<beast::string_view> body_views;
    std::vector<http::response<http::empty_body>> header_templates;
};

bool parse_args(int argc, char* argv[], Config& config) {
    try {
        po::options_description desc("Benchmark Server Options");
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

    } catch (const po::error& e) {
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
    auto const ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    return std::to_string(ns);
}

ResponseCache generate_responses(const Config& config) {
    ResponseCache cache;
    std::mt19937 gen(config.seed);

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
        size_t body_len = len_dist(gen);
        size_t start_offset = 0;
        if (config.max_length > body_len) {
            std::uniform_int_distribution<size_t> offset_dist(0, config.max_length - body_len);
            start_offset = offset_dist(gen);
        }

        beast::string_view body_view(&cache.data_block[start_offset], body_len);
        cache.body_views.push_back(body_view);

        http::response<http::empty_body> header_template;
        header_template.version(11);
        header_template.result(http::status::ok);
        header_template.set(http::field::server, "BenchmarkServer");
        header_template.set(http::field::content_type, "application/octet-stream");
        cache.header_templates.push_back(header_template);
    }

    std::cout << "Generated " << config.num_responses << " response views into a single data block." << std::endl;
    return cache;
}

template<class Stream>
void do_session(Stream& stream, const ResponseCache& cache, const Config& config) {
    // *** Use flat_buffer, like the client ***
    beast::flat_buffer buffer;
    buffer.reserve(1024 * 1024 + 16); // Reserve once

    beast::error_code ec;
    std::size_t count = 0;
    // Temporary storage for potentially fragmented body
    std::vector<char> full_body_storage;


    if constexpr (std::is_same_v<typename Stream::protocol_type, tcp>) { // Check if it's TCP
        stream.set_option(tcp::no_delay(true), ec); // Set the option
        if(ec) {
            // Log a warning if setting fails, but continue the session
            std::cerr << "Warning: Failed to set TCP_NODELAY on accepted socket: " << ec.message() << std::endl;
            ec.clear(); // Clear the error code to allow the session to proceed
        }
    }

    for (;;) { // Loop for multiple requests
        // *** 1. Parser for HEADERS ONLY ***
        http::request_parser<http::empty_body> header_parser;
        header_parser.skip(true); // Ensure body is skipped by this parser

        // *** 2. Read ONLY the headers into the buffer ***
        http::read_header(stream, buffer, header_parser, ec);

        // --- Error Handling for header read ---
        if (ec == http::error::end_of_stream) { break; } // Graceful close
        if (ec) { std::cerr << "Session header read error: " << ec.message() << std::endl; break; }

        // --- Get Content Length ---
        if (!header_parser.content_length()) {
            if(header_parser.get().method() == http::verb::post) {
                std::cerr << "Error: POST request missing Content-Length." << std::endl;
                break;
            }
        }
        size_t body_len = header_parser.content_length().value_or(0);

        // *** 3. Manually Read Body - Consume from Buffer First ***
        size_t total_body_read = 0;
        full_body_storage.clear(); // Clear storage for this request
        if (body_len > 0) {
            full_body_storage.reserve(body_len); // Reserve space

            // Consume initial body part possibly read by read_header
            size_t available_in_buffer = buffer.size();
            size_t from_buffer = std::min(available_in_buffer, body_len);

            if (from_buffer > 0) {
                // Copy data from buffer to our temporary storage
                const char* initial_chunk_ptr = static_cast<const char*>(buffer.data().data());
                full_body_storage.insert(full_body_storage.end(), initial_chunk_ptr, initial_chunk_ptr + from_buffer);
                total_body_read += from_buffer;
                // Consume this initial chunk from the flat_buffer
                buffer.consume(from_buffer);
            }

            // Read the REMAINDER (if any) directly from the stream
            while (total_body_read < body_len) {
                 size_t bytes_to_read = body_len - total_body_read;
                 // Use read_some on the stream, temporarily using flat_buffer's storage via prepare/commit
                 size_t bytes_read = stream.read_some(buffer.prepare(bytes_to_read), ec);
                 buffer.commit(bytes_read); // Commit makes data available in buffer.data()

                 if (bytes_read > 0) {
                     // Copy newly read data to our temporary storage
                     const char* new_chunk_ptr = static_cast<const char*>(buffer.data().data());
                     full_body_storage.insert(full_body_storage.end(), new_chunk_ptr, new_chunk_ptr + bytes_read);
                     // Consume the newly read chunk from the flat_buffer immediately
                     buffer.consume(bytes_read);
                 }
                 total_body_read += bytes_read;

                 if (ec == net::error::eof || ec == http::error::end_of_stream) {
                     if (total_body_read < body_len) {
                        std::cerr << "Error: Connection closed prematurely during body read. Read "
                                  << total_body_read << "/" << body_len << " bytes." << std::endl;
                        ec = http::error::partial_message;
                     }
                     break; // Exit inner loop
                 }
                 if (ec) {
                     std::cerr << "Session body read error: " << ec.message() << std::endl;
                     break; // Exit inner loop on error
                 }
                 if (bytes_read == 0 && total_body_read < body_len){
                     std::cerr << "Warning: read_some returned 0 but EOF/error not detected." << std::endl;
                     ec = http::error::partial_message;
                     break;
                 }
            } // End while(total_body_read < body_len)
        } // End if(body_len > 0)

        // Check for body read errors after the loop
        if (ec && ec != net::error::eof && ec != http::error::end_of_stream) {
             std::cerr << "Exiting session due to body read error: " << ec.message() << std::endl;
             break; // Exit outer loop
        }
        // Check if we got less body than expected
        if (total_body_read < body_len) {
             std::cerr << "Error: Read less body data (" << total_body_read
                       << ") than Content-Length (" << body_len << ")." << std::endl;
             // Adjust length to what was actually read for view creation
             body_len = total_body_read;
             // break; // Optionally exit outer loop entirely if partial body is unacceptable
        }

        // --- Body View Creation ---
        // *** Create view from the accumulated temporary storage ***
        beast::string_view req_body_view { full_body_storage.data(), full_body_storage.size() };


        // --- Verification Logic ---
        if (config.verify && req_body_view.size() >= 16) {
            // --- Checksum Calculation ---
            size_t payload_len = (req_body_view.size() >= 16) ? req_body_view.size() - 16 : 0;
            auto payload_view = req_body_view.substr(0, payload_len);
            auto received_checksum_hex = req_body_view.substr(payload_len);

            uint64_t calculated_checksum = xor_checksum(payload_view);
            uint64_t received_checksum = 0;
            if (received_checksum_hex.size() == 16) {
                 std::stringstream ss;
                 ss << std::hex << std::string(received_checksum_hex);
                 ss >> received_checksum;
            } else {
                 std::cerr << "Warning: Received checksum hex size is not 16 (" << received_checksum_hex.size() << ")" << std::endl;
            }

            if (calculated_checksum != received_checksum) {
                std::cerr << "Warning: Checksum mismatch from client!" << std::endl;
            }
        }

        // --- Response Sending Logic (Unchanged) ---
        const auto& header_template = cache.header_templates[0];
        const auto& body_view = cache.body_views[0]; // Server's response body
        std::string ts_str = get_timestamp_str();

        if (config.verify) {
             uint64_t checksum_val = xor_checksum(body_view);
             std::stringstream ss;
             ss << std::hex << std::setw(16) << std::setfill('0') << checksum_val;
             std::string checksum_str = ss.str();
             http::response<http::string_body> res;
             res.base() = header_template;
             res.body().reserve(body_view.size() + checksum_str.size() + ts_str.size());
             res.body().append(body_view.data(), body_view.size());
             res.body().append(checksum_str);
             res.body().append(ts_str);
             res.prepare_payload();
             http::write(stream, res, ec);
        } else {
              http::response<http::span_body<const char>> res;
              res.base() = header_template;
              res.body() = { body_view.data(), body_view.size() };
              res.set(http::field::content_length, std::to_string(body_view.size() + ts_str.size()));
              http::serializer<false, decltype(res)::body_type> sr{res};
              http::write_header(stream, sr, ec);
              if (!ec) {
                  std::array<net::const_buffer, 2> buffers = {
                      net::buffer(body_view.data(), body_view.size()),
                      net::buffer(ts_str)
                  };
                  net::write(stream, buffers, ec);
              }
        }

        if (ec) { std::cerr << "Session write error: " << ec.message() << std::endl; break; }

        // --- Loop Control ---
        bool const keep_alive = header_parser.get().keep_alive();

        // *** Buffer should be empty now after incremental consumption ***
        if(buffer.size() > 0) {
             std::cerr << "Warning: Buffer not empty at end of loop iteration. Consuming remaining " << buffer.size() << " bytes." << std::endl;
             buffer.consume(buffer.size());
        }

        ++count;
        if (count == config.num_responses) { break; } // Exit after N requests
        if (!keep_alive) { break; } // Exit if client doesn't want keep-alive

    } // End of for(;;) loop

    // --- Shutdown Logic (Unchanged) ---
    if constexpr (std::is_same_v<typename Stream::protocol_type, tcp>) {
        stream.shutdown(tcp::socket::shutdown_send, ec);
    } else {
        stream.shutdown(local::socket::shutdown_send, ec);
    }
} // End of do_session


template<class Acceptor, class Endpoint>
void do_listen(net::io_context& ioc, const Endpoint& endpoint, const ResponseCache& cache, const Config& config) {
    beast::error_code ec;
    Acceptor acceptor(ioc);

    acceptor.open(endpoint.protocol(), ec);
    if(ec) { std::cerr << "Failed to open acceptor: " << ec.message() << std::endl; return; }

    acceptor.set_option(net::socket_base::reuse_address(true), ec);
    if(ec) { std::cerr << "Failed to set reuse_address: " << ec.message() << std::endl; return; }

    acceptor.bind(endpoint, ec);
    if(ec) { std::cerr << "Failed to bind to endpoint: " << ec.message() << std::endl; return; }

    acceptor.listen(net::socket_base::max_listen_connections, ec);
    if(ec) { std::cerr << "Failed to listen on endpoint: " << ec.message() << std::endl; return; }

    std::cout << "Server listening for connections..." << std::endl;
    for(;;)
    {
        auto socket = acceptor.accept(ioc, ec);
        if(ec)
        {
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

    net::io_context ioc;

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
    return 0;
}