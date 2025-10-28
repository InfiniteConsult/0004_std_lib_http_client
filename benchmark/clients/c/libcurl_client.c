#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h> // Use standard boolean types for C23
#include <curl/curl.h>
#include <limits.h>

// Struct definitions are preserved from your original implementation
typedef struct {
    char* host;
    int port;
    char* transport;
    uint64_t num_requests;
    char* data_file;
    char* output_file;
    bool verify;
    uint32_t seed;
    size_t request_body_size;
} Config;

typedef struct {
    uint64_t num_requests;
    uint64_t* sizes;
    char* data_block;
    size_t data_block_size;
} BenchmarkData;

typedef struct {
    char* data;
    size_t len;
    size_t capacity;
} GrowableBuffer;

// NEW: Context for the read callback (zero-copy path).
typedef struct {
    const char* body_ptr;
    size_t total_size;
    size_t sent_size;
} ReadContext;

// This struct is passed to the write_callback.
typedef struct {
    GrowableBuffer* response_buffer;
    int64_t* latencies;
    uint64_t current_index;
    const Config* config;
} ResponseData;


// (parse_args, read_benchmark_data, xor_checksum, get_nanoseconds are unchanged)
bool parse_args(int argc, char* argv[], Config* config) {
    config->transport = "tcp";
    config->num_requests = 1000;
    config->data_file = "benchmark_data.bin";
    config->output_file = "latencies_libcurl.bin";
    config->verify = true;
    config->seed = 1234;
    config->request_body_size = 128;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <host> <port> [options]\n", argv[0]);
        return false;
    }

    config->host = argv[1];
    config->port = atoi(argv[2]);

    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--transport") == 0 && i + 1 < argc) {
            config->transport = argv[++i];
        }
        else if (strcmp(argv[i], "--num-requests") == 0 && i + 1 < argc) {
            config->num_requests = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--data-file") == 0 && i + 1 < argc) {
            config->data_file = argv[++i];
        } else if (strcmp(argv[i], "--output-file") == 0 && i + 1 < argc) {
            config->output_file = argv[++i];
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            config->seed = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--request-body-size") == 0 && i + 1 < argc) {
            config->request_body_size = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-verify") == 0) {
            config->verify = false;
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

    if (fread(&data->num_requests, sizeof(uint64_t), 1, file) != 1) {
        fclose(file);
        return false;
    }

    data->sizes = malloc(data->num_requests * sizeof(uint64_t));
    if (!data->sizes) {
        fclose(file);
        return false;
    }
    if (fread(data->sizes, sizeof(uint64_t), data->num_requests, file) != data->num_requests) {
        free(data->sizes);
        fclose(file);
        return false;
    }

    long current_pos = ftell(file);
    fseek(file, 0, SEEK_END);
    data->data_block_size = ftell(file) - current_pos;
    fseek(file, current_pos, SEEK_SET);

    data->data_block = malloc(data->data_block_size);
    if (!data->data_block) {
        free(data->sizes);
        fclose(file);
        return false;
    }
    if (fread(data->data_block, 1, data->data_block_size, file) != data->data_block_size) {
        free(data->sizes);
        free(data->data_block);
        fclose(file);
        return false;
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

size_t read_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    ReadContext* ctx = (ReadContext*)userdata;
    size_t buffer_size = size * nitems;
    size_t remaining = ctx->total_size - ctx->sent_size;
    size_t bytes_to_copy = (buffer_size < remaining) ? buffer_size : remaining;

    if (bytes_to_copy) {
        memcpy(buffer, ctx->body_ptr + ctx->sent_size, bytes_to_copy);
        ctx->sent_size += bytes_to_copy;
    }
    return bytes_to_copy;
}

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t real_size = size * nmemb;
    ResponseData* res_data = (ResponseData*)userdata;
    GrowableBuffer* buf = res_data->response_buffer;

    if (buf->len + real_size > buf->capacity) {
        size_t new_capacity = buf->capacity == 0 ? 4096 : buf->capacity * 2;
        while (new_capacity < buf->len + real_size) {
            new_capacity *= 2;
        }
        char* new_data = realloc(buf->data, new_capacity);
        if (!new_data) {
            fprintf(stderr, "Failed to reallocate response buffer\n");
            return 0;
        }
        buf->data = new_data;
        buf->capacity = new_capacity;
    }

    memcpy(buf->data + buf->len, ptr, real_size);
    buf->len += real_size;

    return real_size;
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

    curl_global_init(CURL_GLOBAL_ALL);
    CURL* curl_handle = curl_easy_init();
    if (!curl_handle) {
        fprintf(stderr, "Failed to initialize curl handle\n");
        return 1;
    }

    if (strcmp(config.transport, "tcp") == 0) {
        char url[256];
        snprintf(url, sizeof(url), "http://%s:%d/", config.host, config.port);
        curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    } else if (strcmp(config.transport, "unix") == 0) {
        curl_easy_setopt(curl_handle, CURLOPT_UNIX_SOCKET_PATH, config.host);
        curl_easy_setopt(curl_handle, CURLOPT_URL, "http://localhost/");
    } else {
        fprintf(stderr, "Error: Invalid transport type '%s'\n", config.transport);
        return 1;
    }

    GrowableBuffer response_buffer = {0};
    ResponseData response_data = {&response_buffer, latencies, 0, &config};
    ReadContext read_context = {0};
    char* payload_buffer = NULL; // For the verify path only

    // Set curl options that are the same for all requests
    curl_easy_setopt(curl_handle, CURLOPT_POST, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &response_data);

    // Seems to be slower
    // long buffer_size_bytes = 1024L * 1024L + 128L * 1024L; // 1 MiB + 128 KiB
    // curl_easy_setopt(curl_handle, CURLOPT_UPLOAD_BUFFERSIZE, buffer_size_bytes);
    // curl_easy_setopt(curl_handle, CURLOPT_BUFFERSIZE, buffer_size_bytes);

    for (uint64_t i = 0; i < config.num_requests; ++i) {
        response_buffer.len = 0; // Reset buffer for the new response
        response_data.current_index = i;

        size_t req_size = benchmark_data.sizes[i % benchmark_data.num_requests];
        const char* body_slice = benchmark_data.data_block;

        if (config.verify) {
            // **NON-ZERO-COPY PATH (with verification)**
            // Disable the read callback in case it was set from a previous iteration
            curl_easy_setopt(curl_handle, CURLOPT_READFUNCTION, NULL);

            uint64_t checksum = xor_checksum(body_slice, req_size);
            size_t payload_size = req_size + 16;
            payload_buffer = realloc(payload_buffer, payload_size + 1);
            memcpy(payload_buffer, body_slice, req_size);
            snprintf(payload_buffer + req_size, 17, "%016" PRIx64, checksum);

            // Use POSTFIELDS, which makes a copy of our `payload_buffer`
            curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, payload_buffer);
            curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDSIZE, payload_size);
        } else {
            // **ZERO-COPY PATH (no verification)**
            // Disable POSTFIELDS in case it was set from a previous iteration
            curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, NULL);

            // Set up the context for our read callback
            read_context.body_ptr = body_slice;
            read_context.total_size = req_size;
            read_context.sent_size = 0;

            // Enable the read callback
            curl_easy_setopt(curl_handle, CURLOPT_READFUNCTION, read_callback);
            curl_easy_setopt(curl_handle, CURLOPT_READDATA, &read_context);
            curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)req_size);
        }

        CURLcode res = curl_easy_perform(curl_handle);
        uint64_t client_receive_time = get_nanoseconds();

        if (res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            break;
        }

        const char* res_body = response_buffer.data;
        size_t res_body_len = response_buffer.len;

        if (config.verify) {
            if (res_body_len < 35) {
                 fprintf(stderr, "Warning: Response body too short for verification on request %lu!\n", i);
            } else {
                const char* res_payload = res_body;
                size_t res_payload_len = res_body_len - 35;
                const char* res_checksum_hex = res_body + res_payload_len;

                uint64_t calculated_checksum = xor_checksum(res_payload, res_payload_len);
                uint64_t received_checksum = 0;
                sscanf(res_checksum_hex, "%16lx", &received_checksum);

                if (calculated_checksum != received_checksum) {
                    fprintf(stderr, "Warning: Response checksum mismatch on request %lu!\n", i);
                }
            }
        }

        if (res_body_len < 19) {
            fprintf(stderr, "Warning: Response body too short for timestamp on request %lu!\n", i);
        } else {
            const char* server_timestamp_str = res_body + (res_body_len - 19);
            uint64_t server_timestamp = atoll(server_timestamp_str);
            latencies[i] = client_receive_time - server_timestamp;
        }
    }

    curl_easy_cleanup(curl_handle);
    curl_global_cleanup();

    FILE* out_file = fopen(config.output_file, "wb");
    if (out_file) {
        fwrite(latencies, sizeof(int64_t), config.num_requests, out_file);
        fclose(out_file);
    }

    // Clean up all allocated memory
    free(payload_buffer);
    free(response_buffer.data);
    free(latencies);
    free(benchmark_data.sizes);
    free(benchmark_data.data_block);

    printf("libcurl_client: completed %lu requests.\n", (unsigned long)config.num_requests);

    return 0;
}