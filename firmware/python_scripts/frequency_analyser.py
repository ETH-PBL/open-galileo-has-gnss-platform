import os
import json
import datetime
import statistics

# --- Configuration ---
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
LOG_DIR = os.path.join(SCRIPT_DIR, 'log_data')
INPUT_LOG = os.path.join(LOG_DIR, 'log_m1_has.jsonl')

def main():
    if not os.path.exists(INPUT_LOG):
        print(f"Log file not found: {INPUT_LOG}")
        print("Please run the serial_logger.py script first.")
        return
    
    sensor_timestamps = {}
    gnss_mismatches = 0
    gnss_checked = 0

    print(f"Loading data from: {INPUT_LOG}...\n")

    with open(INPUT_LOG, 'r', encoding='utf-8') as f:
        for line in f:
            if not line.strip():
                continue
            
            try:
                data = json.loads(line)
                sensor = data.get('sensor')
                
                if not sensor or sensor == 'TEST':
                    continue

                # --- GNSS Time Validation ---
                if sensor == 'GNSS_PVT':
                    ts = int(data.get('epoch_us', 0))
                    
                    try:
                        year = int(data['year'])
                        # Guard against uninitialized GNSS calendar data
                        month = max(1, int(data['month']))
                        day = max(1, int(data['day']))
                        hour = int(data['hour'])
                        minute = int(data['min'])
                        sec = int(data['sec'])
                        nano = int(data['nano'])
                        
                                
                        dt = datetime.datetime(year, month, day, hour, minute, sec, tzinfo=datetime.timezone.utc)
                        expected_us = (int(dt.timestamp()) * 1000000) + int(round(nano / 1000.0))
                        
                        if ts != expected_us:
                            gnss_mismatches += 1
                            print(f"GNSS Time Mismatch: epoch_us={ts} vs calculated {expected_us} (UTC: {dt.isoformat()}) + {nano}ns")
                        gnss_checked += 1
                            
                    except (KeyError, ValueError, OverflowError):
                        pass
                else:
                    ts = int(data.get('ts', 0))
                    
                if sensor not in sensor_timestamps:
                    sensor_timestamps[sensor] = []
                    
                sensor_timestamps[sensor].append(ts)
                
            except json.JSONDecodeError:
                continue

    # --- Gap & Frequency Analysis ---
    print("============================================================")
    print(" SENSOR MEASUREMENT FREQUENCY & GAP ANALYSIS")
    print("============================================================\n")

    for sensor, timestamps in sensor_timestamps.items():
        timestamps.sort()
        total_measurements = len(timestamps)
        
        print(f"[{sensor}]")
        print(f"  Total Measurements : {total_measurements}")
        
        if total_measurements < 2:
            print(f"  Data Quality       : Insufficient data for frequency analysis.\n")
            continue
            
        # Calculate intervals (delta between consecutive timestamps)
        intervals = [timestamps[i] - timestamps[i-1] for i in range(1, total_measurements)]
        
        # Median interval is safer for finding the baseline since gaps skew the average
        sorted_intervals = sorted(intervals)
        median_interval = sorted_intervals[len(sorted_intervals) // 2]
        
        # Use microseconds directly assuming the embedded timestamps are strictly tracked in µs 
        avg_interval = sum(intervals) / len(intervals) if intervals else 0
        avg_frequency = 1000000.0 / avg_interval if avg_interval > 0 else 0
        
        min_interval = min(intervals) if intervals else 0
        max_interval = max(intervals) if intervals else 0
        std_dev = statistics.stdev(intervals) if len(intervals) >= 2 else 0.0
        
        # Threshold for a dropped packet/gap (e.g. interval is more than 1.5x the median)
        gaps = [i for i in intervals if i > (median_interval * 1.5)]
        
        print(f"  Avg Frequency      : {avg_frequency:.2f} Hz")
        print(f"  Avg Interval       : {avg_interval / 1000.0:.2f} ms")
        print(f"  Median Interval    : {median_interval / 1000.0:.2f} ms")
        print(f"  Min/Max Interval   : {min_interval / 1000.0:.2f} ms / {max_interval / 1000.0:.2f} ms")
        print(f"  Interval Std Dev   : {std_dev / 1000.0:.2f} ms")
        
        if gaps:
            print(f"  Data Quality       : WARNING - {len(gaps)} missing measurement(s)/gap(s) detected. Max gap: {max(gaps) / 1000.0:.2f} ms.")
        else:
            print(f"  Data Quality       : EXCELLENT - No missing measurements detected.")
            
        # Print GNSS validation results if applicable
        if sensor == 'GNSS_PVT' and gnss_checked > 0:
            if gnss_mismatches == 0:
                print(f"  Time Validation    : PASSED - {gnss_checked}/{gnss_checked} calculated epoch_us match UTC fields.")
            else:
                print(f"  Time Validation    : FAILED - {gnss_mismatches} mismatches found out of {gnss_checked}!")
        
        print("")

if __name__ == "__main__":
    main()