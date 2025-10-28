#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <limits.h>
#include <inttypes.h>

#include <httpc/httpc.h>


typedef struct {
    char* host;
    int port;
    int transport_type;
    uint64_t num_requests;
    char* data_file;
    char* output_file;
    bool verify;
    bool unsafe_res;
    HttpIoPolicy io_policy;
} Config;

typedef struct {
    uint64_t num_requests;
    uint64_t* sizes;
    char* data_block;
    size_t data_block_size;
} BenchmarkData;

bool parse_args(int argc, char* argv[], Config* config) {
    config->transport_type = HttpTransportType.TCP;
    config->num_requests = 1000;
    config->data_file = "benchmark_data.bin";
    config->output_file = "latencies_httpc.bin";
    config->verify = true;
    config->unsafe_res = false;
    config->io_policy = HTTP_IO_COPY_WRITE;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <host> <port> [options]\n", argv[0]);
        return false;
    }

    config->host = argv[1];
    config->port = atoi(argv[2]);

    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--num-requests") == 0 && i + 1 < argc) {
            config->num_requests = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--data-file") == 0 && i + 1 < argc) {
            config->data_file = argv[++i];
        } else if (strcmp(argv[i], "--output-file") == 0 && i + 1 < argc) {
            config->output_file = argv[++i];
        } else if (strcmp(argv[i], "--transport") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "unix") == 0) {
                config->transport_type = HttpTransportType.UNIX;
            }
        } else if (strcmp(argv[i], "--io-policy") == 0 && i + 1 < argc) {
             i++;
            if (strcmp(argv[i], "vectored") == 0) {
                config->io_policy = HTTP_IO_VECTORED_WRITE;
            }
        } else if (strcmp(argv[i], "--no-verify") == 0) {
            config->verify = false;
        } else if (strcmp(argv[i], "--unsafe") == 0) {
            config->unsafe_res = true;
        }
    }
    return true;
}

bool read_benchmark_data(const char* filename, BenchmarkData* data) {
    FILE* file = fopen(filename, "rb");
    if (!file) {
        perror("Could not open benchmark data file");
        return false;
    }
    if (fread(&data->num_requests, sizeof(uint64_t), 1, file) != 1) { fclose(file); return false; }

    data->sizes = malloc(data->num_requests * sizeof(uint64_t));
    if (!data->sizes) { fclose(file); return false; }
    if (fread(data->sizes, sizeof(uint64_t), data->num_requests, file) != data->num_requests) {
        free(data->sizes); fclose(file); return false;
    }

    long current_pos = ftell(file);
    fseek(file, 0, SEEK_END);
    data->data_block_size = ftell(file) - current_pos;
    fseek(file, current_pos, SEEK_SET);

    data->data_block = malloc(data->data_block_size);
    if (!data->data_block) { free(data->sizes); fclose(file); return false; }
    if (fread(data->data_block, 1, data->data_block_size, file) != data->data_block_size) {
        free(data->sizes); free(data->data_block); fclose(file); return false;
    }
    fclose(file);
    return true;
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
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
}

int main(int argc, char* argv[]) {
    Config config;
    if (!parse_args(argc, argv, &config)) {
        return 1;
    }

    BenchmarkData benchmark_data;
    if (!read_benchmark_data(config.data_file, &benchmark_data)) {
        return 1;
    }

    int64_t* latencies = malloc(config.num_requests * sizeof(int64_t));
    if (!latencies) {
        fprintf(stderr, "Failed to allocate latencies array\n");
        return 1;
    }

    HttpResponseMemoryPolicy res_mem_policy = config.unsafe_res ? HTTP_RESPONSE_UNSAFE_ZERO_COPY : HTTP_RESPONSE_SAFE_OWNING;

    struct HttpClient client;
    Error err = http_client_init(&client, config.transport_type, HttpProtocolType.HTTP1, res_mem_policy, config.io_policy);
    if (err.type != ErrorType.NONE) {
        fprintf(stderr, "Failed to initialize http client\n");
        return 1;
    }

    err = client.connect(&client, config.host, config.port);
    if (err.type != ErrorType.NONE) {
        fprintf(stderr, "Failed to connect to http server\n");
        http_client_destroy(&client);
        return 1;
    }

    size_t max_req_size = 0;
    for (uint64_t i = 0; i < benchmark_data.num_requests; ++i) {
        if (benchmark_data.sizes[i] > max_req_size) {
            max_req_size = benchmark_data.sizes[i];
        }
    }
    size_t max_payload_size = max_req_size + 16;
    char* payload_buffer = malloc(max_payload_size + 1);
    char content_len_str[32];

    for (uint64_t i = 0; i < config.num_requests; ++i) {
        size_t req_size = benchmark_data.sizes[i % benchmark_data.num_requests];
        const char* body_slice = benchmark_data.data_block;

        HttpRequest request = {0};
        request.path = "/";

        if (config.verify) {
            uint64_t checksum = xor_checksum(body_slice, req_size);
            size_t payload_size = req_size + 16;
            memcpy(payload_buffer, body_slice, req_size);
            snprintf(payload_buffer + req_size, 17, "%016" PRIx64, checksum);

            request.body = payload_buffer;
            snprintf(content_len_str, sizeof(content_len_str), "%zu", payload_size);
        } else {
            request.body = body_slice;
            snprintf(content_len_str, sizeof(content_len_str), "%zu", req_size);
        }

        request.headers[0].key = "Content-Length";
        request.headers[0].value = content_len_str;
        request.num_headers = 1;

        HttpResponse response = {0};
        err = client.post(&client, &request, &response);
        uint64_t client_receive_time = get_nanoseconds();

        if (err.type != ErrorType.NONE) {
            fprintf(stderr, "Request failed on iteration %lu\n", (unsigned long)i);
            break;
        }

        if (config.verify) {
            if (response.body_len < 35) {
                fprintf(stderr, "Warning: Response body too short for verification on request %lu!\n", i);
            } else {
                size_t res_payload_len = response.body_len - 35;
                const char* res_checksum_hex = response.body + res_payload_len;
                uint64_t calculated_checksum = xor_checksum(response.body, res_payload_len);
                uint64_t received_checksum = 0;
                sscanf(res_checksum_hex, "%16" SCNx64, &received_checksum);
                if (calculated_checksum != received_checksum) {
                    fprintf(stderr, "Warning: Response checksum mismatch on request %lu!\n", i);
                }
            }
        }

        const char* server_timestamp_str = response.body + (response.body_len - 19);
        uint64_t server_timestamp = atoll(server_timestamp_str);
        latencies[i] = client_receive_time - server_timestamp;

        if (res_mem_policy == HTTP_RESPONSE_SAFE_OWNING) {
            http_response_destroy(&response);
        }
    }

    http_client_destroy(&client);

    FILE* out_file = fopen(config.output_file, "wb");
    if (out_file) {
        fwrite(latencies, sizeof(int64_t), config.num_requests, out_file);
        fclose(out_file);
    }

    free(payload_buffer);
    free(latencies);
    free(benchmark_data.sizes);
    free(benchmark_data.data_block);

    printf("httpc_client: completed %lu requests.\n", (unsigned long)config.num_requests);

    return 0;
}