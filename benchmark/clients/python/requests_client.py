import argparse
import struct
import time
import functools
import operator
import sys


from urllib.parse import quote


import requests
import requests_unixsocket


MASK64 = 0xFFFFFFFFFFFFFFFF


def parse_args():
    parser = argparse.ArgumentParser(description="Benchmark client for the 'requests' library.")

    parser.add_argument("host", help="The server host (e.g., 127.0.0.1) or path to Unix socket.")
    parser.add_argument("port", type=int, help="The server port (ignored for Unix sockets).")

    parser.add_argument("--transport", type=str, default="tcp", choices=["tcp", "unix"], help="Transport to use.")
    parser.add_argument("--num-requests", type=int, default=1000, help="Number of requests to make.")
    parser.add_argument("--seed", type=int, default=1234, help="Seed for the PRNG to generate the request body.")
    parser.add_argument("--request-body-size", type=int, default=128, help="Size of the request body to generate.")
    parser.add_argument("--data-file", type=str, default="benchmark_data.bin", help="Path to the pre-generated data file.")
    parser.add_argument("--output-file", type=str, default="latencies_requests.bin", help="File to save raw latency data to.")

    parser.add_argument('--no-verify', action='store_false', dest='verify', help="Disable checksum validation.")
    parser.set_defaults(verify=True)

    return parser.parse_args()


def read_benchmark_data(filename="benchmark_data.bin"):
    with open(filename, "rb") as f:
        num_requests_bytes = f.read(8)
        num_requests = struct.unpack('<Q', num_requests_bytes)[0]

        sizes_bytes = f.read(num_requests * 8)
        request_sizes = [size[0] for size in struct.iter_unpack('<Q', sizes_bytes)]

        data_block = f.read()

    return request_sizes, data_block


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
        session = requests_unixsocket.Session()
        base_url = f"http+unix://{quote(args.host, safe='')}"
    else:
        session = requests.Session()
        base_url = f"http://{args.host}:{args.port}"

    with session:
        for i in range(args.num_requests):
            req_size = request_sizes[i % len(request_sizes)]
            body_slice = data_block_view[:req_size]

            payload = bytearray(body_slice)
            if args.verify:
                checksum = xor_checksum(body_slice)
                payload.extend(f'{checksum:016x}'.encode('ascii'))

            try:
                response = session.post(f"{base_url}/", data=bytes(payload))
                client_receive_time = time.time_ns()
            except requests.exceptions.RequestException as e:
                sys.exit(f"Error during request {i}: {e}")

            if response.status_code != 200:
                sys.exit(f"Error: Received status code {response.status_code} on request {i}")

            response_body = response.content
            if args.verify:
                res_payload = response_body[:-35]
                res_checksum_hex = response_body[-35:-19]
                calculated_checksum = xor_checksum(res_payload)
                received_checksum = int(res_checksum_hex, 16)
                if calculated_checksum != received_checksum:
                    print(f"Warning: Response checksum mismatch on request {i}!", file=sys.stderr)

            server_timestamp_str = response_body[-19:]
            server_timestamp = int(server_timestamp_str)
            latencies[i] = client_receive_time - server_timestamp

    with open(args.output_file, "wb") as f:
        f.write(struct.pack(f'<{len(latencies)}q', *latencies))

    print(f"requests_client: completed {args.num_requests} requests and saved latencies to {args.output_file}.")


if __name__ == "__main__":
    main()