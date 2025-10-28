import argparse
import struct
import time
import sys
import functools
import operator
import traceback


from httppy.tcp_transport import TcpTransport
from httppy.unix_transport import UnixTransport
from httppy.http1_protocol import Http1Protocol
from httppy.httppy import HttpClient
from httppy.http_protocol import HttpRequest, HttpMethod


MASK64 = 0xFFFFFFFFFFFFFFFF

def parse_args():
    parser = argparse.ArgumentParser(description="Benchmark client for the 'httppy' library.")

    parser.add_argument("host", help="The server host (e.g., 127.0.0.1) or path to Unix socket.")
    parser.add_argument("port", type=int, help="The server port (ignored for Unix sockets).")

    parser.add_argument("--transport", type=str, default="tcp", choices=["tcp", "unix"], help="Transport to use.")
    parser.add_argument("--num-requests", type=int, default=1000, help="Number of requests to make.")
    parser.add_argument("--data-file", type=str, default="benchmark_data.bin", help="Path to the pre-generated data file.")
    parser.add_argument("--output-file", type=str, default="latencies_httppy.bin", help="File to save raw latency data to.")

    parser.add_argument('--no-verify', action='store_false', dest='verify', help="Disable checksum validation.")
    parser.add_argument('--unsafe', action='store_true', help="Use the unsafe/zero-copy response model.")
    parser.set_defaults(verify=True, unsafe=False)

    return parser.parse_args()


def read_benchmark_data(filename="benchmark_data.bin"):
    try:
        with open(filename, "rb") as f:
            num_requests_bytes = f.read(8)
            num_requests = struct.unpack('<Q', num_requests_bytes)[0]
            sizes_bytes = f.read(num_requests * 8)
            request_sizes = [size[0] for size in struct.iter_unpack('<Q', sizes_bytes)]
            data_block = f.read()
        return request_sizes, data_block
    except FileNotFoundError:
        sys.exit(f"Error: Data file '{filename}' not found. Please run the data_generator first.")


def rotr64(x: int, s: int) -> int:
    n = 64
    r = s % n
    if r < 0:
        r += n
    if r == 0:
        return x
    rotated = ((x >> r) | (x << (n - r))) & MASK64
    return rotated


def xor_checksum(data: bytes | memoryview) -> int:
    checksum = 0
    for byte in data:
        checksum = rotr64(checksum, 7) ^ byte
    return checksum


def main():
    args = parse_args()
    request_sizes, data_block = read_benchmark_data(args.data_file)
    data_block_view = memoryview(data_block)
    latencies = [0] * args.num_requests

    if args.transport == "unix":
        transport = UnixTransport()
    else:
        transport = TcpTransport()

    protocol = Http1Protocol(transport)
    client = HttpClient(protocol)

    try:
        client.connect(args.host, args.port)

        for i in range(args.num_requests):
            req_size = request_sizes[i % len(request_sizes)]
            body_slice = data_block_view[:req_size]


            payload = bytearray(body_slice)

            # If verify is on, calculate and append the checksum
            if args.verify:
                checksum = xor_checksum(body_slice)
                payload.extend(f'{checksum:016x}'.encode('ascii'))

            request = HttpRequest(
                path="/",
                body=bytes(payload),
                headers=[("Content-Length", str(len(payload)))]
            )

            # Send the request
            if args.unsafe:
                response = client.post_unsafe(request)
            else:
                response = client.post_safe(request)
            client_receive_time = time.time_ns()

            response_body = response.body

            if args.verify:
                res_payload = response_body[:-35]
                res_checksum_hex = response_body[-35:-19]

                calculated_checksum = xor_checksum(bytes(res_payload))

                received_checksum_str = bytes(res_checksum_hex).decode('ascii')
                received_checksum = int(received_checksum_str, 16)

                if calculated_checksum != received_checksum:
                    print(f"Warning: Response checksum mismatch on request {i}!", file=sys.stderr)

            # Convert the memoryview to a string before parsing
            server_timestamp_view = response_body[-19:]
            server_timestamp_str = bytes(server_timestamp_view).decode('ascii')
            server_timestamp = int(server_timestamp_str)

            latencies[i] = client_receive_time - server_timestamp



    except Exception as e:
        sys.exit(f"An error occurred: {e}")
    finally:
        client.disconnect()

    # 3. Save Results
    with open(args.output_file, "wb") as f:
        f.write(struct.pack(f'<{len(latencies)}q', *latencies))

    print(f"httppy_client: completed {args.num_requests} requests and saved latencies to {args.output_file}.")


if __name__ == "__main__":
    main()