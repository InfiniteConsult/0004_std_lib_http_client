use std::env;
use std::error::Error;
use std::fs::File;
use std::io::{Read, Write};
use std::time::{SystemTime, UNIX_EPOCH};

// Import our library components
use httprust::{HttpClient, HttpProtocol, HttpMethod, HttpRequest, HttpHeaderView, Http1Protocol, TcpTransport, UnixTransport, Transport};



struct Config {
    host: String,
    port: u16,
    transport_type: String,
    num_requests: u64,
    data_file: String,
    output_file: String,
    verify: bool,
    unsafe_res: bool,
}

#[derive(Debug)]
struct BenchmarkData {
    sizes: Vec<u64>,
    data_block: Vec<u8>,
}

fn parse_args() -> Result<Config, Box<dyn Error>> {
    let args: Vec<String> = env::args().collect();
    if args.len() < 3 {
        return Err("Usage: httprust_client <host> <port> [options]".into());
    }

    let mut config = Config {
        host: args[1].clone(),
        port: args[2].parse()?,
        transport_type: "tcp".to_string(),
        num_requests: 1000,
        data_file: "benchmark_data.bin".to_string(),
        output_file: "latencies_httprust.bin".to_string(),
        verify: true,
        unsafe_res: false,
    };

    let mut i = 3;
    while i < args.len() {
        match args[i].as_str() {
            "--transport" => { config.transport_type = args[i + 1].clone(); i += 2; }
            "--num-requests" => { config.num_requests = args[i + 1].parse()?; i += 2; }
            "--data-file" => { config.data_file = args[i + 1].clone(); i += 2; }
            "--output-file" => { config.output_file = args[i + 1].clone(); i += 2; }
            "--no-verify" => { config.verify = false; i += 1; }
            "--unsafe" => { config.unsafe_res = true; i += 1; }
            _ => i += 1,
        }
    }
    Ok(config)
}

fn read_benchmark_data(filename: &str) -> Result<BenchmarkData, Box<dyn Error>> {
    let mut file = File::open(filename)?;

    let mut num_requests_buf = [0u8; 8];
    file.read_exact(&mut num_requests_buf)?;
    let num_requests = u64::from_le_bytes(num_requests_buf);

    let mut sizes_bytes = vec![0u8; (num_requests as usize) * 8];
    file.read_exact(&mut sizes_bytes)?;
    let sizes = sizes_bytes.chunks_exact(8).map(|c| u64::from_le_bytes(c.try_into().unwrap())).collect();

    let mut data_block = Vec::new();
    file.read_to_end(&mut data_block)?;

    Ok(BenchmarkData { sizes, data_block })
}

fn xor_checksum(data: &[u8]) -> u64 {
    data.iter().fold(0u64, |acc, &byte| {
        acc.rotate_right(7) ^ u64::from(byte)
    })
}

fn get_nanoseconds() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos() as u64
}

fn run_benchmark<T: Transport + Default>(
    client: &mut HttpClient<Http1Protocol<T>>,
    config: &Config,
    data: &BenchmarkData,
    latencies: &mut [i64],
) -> Result<(), Box<dyn Error>> {
    for i in 0..config.num_requests {
        let req_size = data.sizes[i as usize % data.sizes.len()] as usize;
        let body_slice = &data.data_block[..req_size];

        let mut payload = body_slice.to_vec();
        if config.verify {
            let checksum = xor_checksum(body_slice);
            payload.extend_from_slice(format!("{:016x}", checksum).as_bytes());
        }

        let content_len_str = payload.len().to_string();
        let mut request = HttpRequest {
            method: HttpMethod::Get, // Will be overridden by post_* call
            path: "/",
            body: &payload,
            headers: vec![HttpHeaderView { key: "Content-Length", value: &content_len_str }],
        };

        let client_receive_time: u64;
        let server_timestamp: u64;

        if config.unsafe_res {
            let res = client.post_unsafe(&mut request)?;
            client_receive_time = get_nanoseconds();
            if res.status_code != 200 { return Err(format!("Request failed with status: {}", res.status_code).into()); }

            if config.verify {
                let res_payload = &res.body[..res.body.len() - 35];
                let res_checksum_hex = std::str::from_utf8(&res.body[res.body.len() - 35..res.body.len() - 19])?;
                if xor_checksum(res_payload) != u64::from_str_radix(res_checksum_hex, 16)? {
                    eprintln!("Warning: Checksum mismatch on request {}", i);
                }
            }
            let server_timestamp_str = std::str::from_utf8(&res.body[res.body.len() - 19..])?;
            server_timestamp = server_timestamp_str.parse::<u64>()?;
        } else { // Safe response
            let res = client.post_safe(&mut request)?;
            client_receive_time = get_nanoseconds();
            if res.status_code != 200 { return Err(format!("Request failed with status: {}", res.status_code).into()); }

            if config.verify {
                let res_payload = &res.body[..res.body.len() - 35];
                let res_checksum_hex = std::str::from_utf8(&res.body[res.body.len() - 35..res.body.len() - 19])?;
                if xor_checksum(res_payload) != u64::from_str_radix(res_checksum_hex, 16)? {
                    eprintln!("Warning: Checksum mismatch on request {}", i);
                }
            }
            let server_timestamp_str = std::str::from_utf8(&res.body[res.body.len() - 19..])?;
            server_timestamp = server_timestamp_str.parse::<u64>()?;
        }

        latencies[i as usize] = (client_receive_time - server_timestamp) as i64;
    }
    Ok(())
}


fn main() -> Result<(), Box<dyn Error>> {
    let config = parse_args()?;
    let data = read_benchmark_data(&config.data_file)?;
    let mut latencies = vec![0i64; config.num_requests as usize];

    if config.transport_type == "tcp" {
        let mut client = HttpClient::<Http1Protocol<TcpTransport>>::new();
        client.connect(&config.host, config.port)?;
        run_benchmark(&mut client, &config, &data, &mut latencies)?;
    } else if config.transport_type == "unix" {
        let mut client = HttpClient::<Http1Protocol<UnixTransport>>::new();
        client.connect(&config.host, config.port)?;
        run_benchmark(&mut client, &config, &data, &mut latencies)?;
    } else {
        return Err("Unsupported transport type".into());
    }

    let mut out_file = File::create(&config.output_file)?;
    let latencies_bytes: &[u8] = unsafe {
        std::slice::from_raw_parts(latencies.as_ptr() as *const u8, latencies.len() * 8)
    };
    out_file.write_all(latencies_bytes)?;

    println!("httprust_client: completed {} requests.", config.num_requests);

    Ok(())
}