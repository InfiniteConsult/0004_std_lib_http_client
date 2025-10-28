use std::env;
use std::error::Error;
use std::fs::File;
use std::io::{Read, Write};
use std::time::{SystemTime, UNIX_EPOCH};

struct Config {
    host: String,
    port: u16,
    transport_type: String,
    num_requests: u64,
    data_file: String,
    output_file: String,
    verify: bool,
}

#[derive(Debug)]
struct BenchmarkData {
    sizes: Vec<u64>,
    data_block: Vec<u8>,
}

fn parse_args() -> Result<Config, Box<dyn Error>> {
    let args: Vec<String> = env::args().collect();
    if args.len() < 3 {
        return Err("Usage: reqwest_client <host> <port> [options]".into());
    }

    let mut config = Config {
        host: args[1].clone(),
        port: args[2].parse()?,
        transport_type: "tcp".to_string(),
        num_requests: 1000,
        data_file: "benchmark_data.bin".to_string(),
        output_file: "latencies_reqwest.bin".to_string(),
        verify: true,
    };

    let mut i = 3;
    while i < args.len() {
        match args[i].as_str() {
            "--transport" => {
                config.transport_type = args[i + 1].clone();
                i += 2;
            }
            "--num-requests" => {
                config.num_requests = args[i + 1].parse()?;
                i += 2;
            }
            "--data-file" => {
                config.data_file = args[i + 1].clone();
                i += 2;
            }
            "--output-file" => {
                config.output_file = args[i + 1].clone();
                i += 2;
            }
            "--no-verify" => {
                config.verify = false;
                i += 1;
            }
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
    let sizes = sizes_bytes
        .chunks_exact(8)
        .map(|chunk| u64::from_le_bytes(chunk.try_into().unwrap()))
        .collect();

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
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos() as u64
}

fn main() -> Result<(), Box<dyn Error>> {
    let config = parse_args()?;
    let data = read_benchmark_data(&config.data_file)?;
    let mut latencies = vec![0i64; config.num_requests as usize];

    if config.transport_type == "unix" {
        eprintln!("Error: The reqwest crate does not support Unix domain sockets out of the box.");
        return Err("Unsupported transport type".into());
    }

    let client = reqwest::blocking::Client::new();
    let base_url = format!("http://{}:{}", config.host, config.port);

    for i in 0..config.num_requests {
        let req_size = data.sizes[i as usize % data.sizes.len()] as usize;
        let body_slice = &data.data_block[..req_size];

        // reqwest requires owned block?
        let mut payload = body_slice.to_vec();
        if config.verify {
            let checksum = xor_checksum(body_slice);
            payload.extend_from_slice(format!("{:016x}", checksum).as_bytes());
        }

        let response = client.post(&base_url).body(payload).send()?;
        let client_receive_time = get_nanoseconds();

        if response.status() != 200 {
            return Err(format!("Request failed with status: {}", response.status()).into());
        }

        let body = response.bytes()?.to_vec();

        if config.verify {
            if body.len() < 35 {
                eprintln!("Warning: Response body too short on request {}", i);
            } else {
                let res_payload = &body[..body.len() - 35];
                let res_checksum_hex = std::str::from_utf8(&body[body.len() - 35..body.len() - 19])?;

                let calculated = xor_checksum(res_payload);
                let received = u64::from_str_radix(res_checksum_hex, 16)?;

                if calculated != received {
                    eprintln!("Warning: Checksum mismatch on request {}", i);
                }
            }
        }

        let server_timestamp_str = std::str::from_utf8(&body[body.len() - 19..])?;
        let server_timestamp = server_timestamp_str.parse::<u64>()?;
        latencies[i as usize] = (client_receive_time - server_timestamp) as i64;
    }

    let mut out_file = File::create(&config.output_file)?;
    let latencies_bytes: &[u8] = unsafe {
        std::slice::from_raw_parts(latencies.as_ptr() as *const u8, latencies.len() * 8)
    };
    out_file.write_all(latencies_bytes)?;

    println!("reqwest_client: completed {} requests.", config.num_requests);

    Ok(())
}