import argparse
import pathlib
import struct
import statistics
import math


def parse_arguments() -> argparse.Namespace:
    """Parses command-line arguments for the latency analysis script."""
    parser = argparse.ArgumentParser(
        description="Analyze raw latency data from benchmark runs."
    )
    parser.add_argument(
        "--build-dir",
        type=pathlib.Path,
        default=pathlib.Path("./build_release"),
        help="Path to the build directory containing the 'latencies' subdir (default: ./build_release)",
    )
    parser.add_argument(
        "files",
        nargs='*', # 0 or more file patterns/names
        type=str,
        help="Optional: Specific file patterns (e.g., '*tcp*.bin') or filenames to analyze within the latencies directory. If omitted, analyzes all '*.bin' files.",
    )
    return parser.parse_args()


def find_latency_files(args: argparse.Namespace) -> list[pathlib.Path]:
    """Finds the .bin latency files in the specified directory."""
    latencies_dir = args.build_dir / "latencies"
    if not latencies_dir.is_dir():
        print(f"Error: Latencies directory not found at '{latencies_dir}'")
        return []

    files_to_process: list[pathlib.Path] = []
    if args.files:
        # User specified specific files or patterns
        for pattern in args.files:
            found_files = list(latencies_dir.glob(pattern))
            if not found_files:
                print(f"Warning: No files found matching pattern '{pattern}' in '{latencies_dir}'")
            files_to_process.extend(found_files)
    else:
        # Default: process all .bin files
        files_to_process = list(latencies_dir.glob("*.bin"))

    if not files_to_process:
        print(f"No '.bin' files found to analyze in '{latencies_dir}'")

    # Ensure we only have files (glob might return directories if pattern allows)
    # and remove duplicates if multiple patterns match the same file.
    unique_files = sorted(list(set(f for f in files_to_process if f.is_file())))
    return unique_files


def read_latency_data(file_path: pathlib.Path) -> tuple[list[int], int]:
    """Reads all int64_t latency values from a binary file."""
    total_count = 0
    data: list[int] = []
    try:
        with open(file_path, 'rb') as f:
            content = f.read()
            # Calculate expected count based on file size and int64_t size (8 bytes)
            total_count = len(content) // 8
            # Unpack all values; '<q' is little-endian signed 64-bit int
            data = [val[0] for val in struct.iter_unpack('<q', content)]
            if len(data) != total_count:
                print(f"Warning: File size {len(content)} bytes is not a multiple of 8 for {file_path}. Read {len(data)} entries.")
                total_count = len(data) # Adjust count if file size is weird

    except FileNotFoundError:
        print(f"Error: File not found: {file_path}")
        return [], 0
    except Exception as e:
        print(f"Error reading or unpacking file {file_path}: {e}")
        return [], 0
    return data, total_count


def filter_and_analyze(raw_data: list[int]) -> tuple[dict[str, float] | None, int]:
    """Filters out negative latencies and calculates summary statistics."""
    valid_latencies = []
    excluded_count = 0
    for value in raw_data:
        if value > 0:
            valid_latencies.append(value)
        else:
            excluded_count += 1

    if not valid_latencies:
        return None, excluded_count

    stats: dict[str, float] = {}
    try:
        stats['min'] = float(min(valid_latencies))
        stats['max'] = float(max(valid_latencies))

        # Calculate quartiles (P25, P50=Median, P75)
        # n=4 divides data into 4 parts, giving 3 points: P25, P50, P75
        quartiles = statistics.quantiles(valid_latencies, n=4)
        stats['p25'] = quartiles[0]
        stats['p50'] = quartiles[1] # Median
        stats['p75'] = quartiles[2]

        # Calculate specific percentiles
        # Use n=100 for P90, P99; n=1000 for P99.9 for potentially better precision with integer data
        # Note: indices are 0-based
        if len(valid_latencies) >= 10:
            stats['p90'] = statistics.quantiles(valid_latencies, n=10)[8] # 9th point = P90
        else:
            stats['p90'] = stats['max'] # Fallback for very small samples

        if len(valid_latencies) >= 100:
            stats['p99'] = statistics.quantiles(valid_latencies, n=100)[98] # 99th point = P99
        else:
            stats['p99'] = stats['max'] # Fallback

        if len(valid_latencies) >= 1000:
            stats['p999'] = statistics.quantiles(valid_latencies, n=1000)[998] # 999th point = P99.9
        else:
            stats['p999'] = stats['max'] # Fallback

    except statistics.StatisticsError as e:
        print(f"Error calculating statistics: {e}")
        return None, excluded_count
    except IndexError:
        # This might happen if quantiles returns fewer points than expected
        # (e.g., very small sample size makes higher percentiles meaningless)
        # We already have fallbacks using 'max', but catch just in case.
        print(f"Warning: Could not calculate all percentiles due to sample size {len(valid_latencies)}.")
        # Ensure max is set if fallbacks didn't cover everything
        if 'p90' not in stats: stats['p90'] = stats['max']
        if 'p99' not in stats: stats['p99'] = stats['max']
        if 'p999' not in stats: stats['p999'] = stats['max']

    return stats, excluded_count


def convert_ns_to_human_readable(nanoseconds: float) -> str:
    """Converts a nanosecond value to a human-readable string (ms, µs, or ns)."""
    if nanoseconds >= 1_000_000:
        return f"{nanoseconds / 1_000_000:.3f} ms"
    elif nanoseconds >= 1_000:
        return f"{nanoseconds / 1_000:.1f} µs"
    else:
        return f"{nanoseconds:.0f} ns"


def format_statistics(
        stats: dict[str, float],
        total_count: int,
        excluded_count: int,
        file_path: pathlib.Path
) -> str:
    """Formats the calculated statistics into a readable string."""
    lines = [
        f"--- Analysis for: {file_path.name} ---",
        f"Total data points: {total_count}",
        f"Excluded negative values: {excluded_count}",
    ]
    if stats is None:
        lines.append("No valid data points found for analysis.")
        return "\n".join(lines)

    lines.append("-" * (len(file_path.name) + 18)) # Separator line
    # Five-number summary + Percentiles
    lines.append(f"Min:    {convert_ns_to_human_readable(stats['min'])}")
    lines.append(f"P25 (Q1): {convert_ns_to_human_readable(stats['p25'])}")
    lines.append(f"P50 (Median): {convert_ns_to_human_readable(stats['p50'])}")
    lines.append(f"P75 (Q3): {convert_ns_to_human_readable(stats['p75'])}")
    lines.append(f"P90:    {convert_ns_to_human_readable(stats['p90'])}")
    lines.append(f"P99:    {convert_ns_to_human_readable(stats['p99'])}")
    lines.append(f"P99.9:  {convert_ns_to_human_readable(stats['p999'])}")
    lines.append(f"Max:    {convert_ns_to_human_readable(stats['max'])}")
    lines.append("-" * (len(file_path.name) + 18)) # Footer separator

    return "\n".join(lines)


def main() -> None:
    """Main function to orchestrate the latency analysis."""
    args = parse_arguments()
    latency_files = find_latency_files(args)

    if not latency_files:
        return # find_latency_files already printed an error or 'no files found' message

    for file_path in latency_files:
        raw_data, total_count = read_latency_data(file_path)

        if total_count == 0:
            # read_latency_data handles printing errors if reading fails
            # Print a specific message if the file was read but empty/invalid
            if not raw_data: # Check if list is empty, implies successful read but 0 elements
                print(f"\n--- Analysis for: {file_path.name} ---")
                print("File was empty or contained no valid 64-bit integers.")
                print("-" * (len(file_path.name) + 18))
            continue # Move to the next file

        stats, excluded_count = filter_and_analyze(raw_data)

        formatted_output = format_statistics(stats, total_count, excluded_count, file_path)
        print("\n" + formatted_output)


if __name__ == "__main__":
    main()
