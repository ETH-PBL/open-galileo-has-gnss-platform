import serial
import serial.tools.list_ports
import os
import json
import time

# --- Runtime commands supported by the firmware ---
LAUNCH_COMMANDS = {
    '1': ('LOG_ALL',         'Log all sensors (ISM330, LSM303 ACC/MAG, GNSS PVT) — default'),
    '2': ('LOG_GNSS',        'Log GNSS PVT lines only (sensor lines are dropped)'),
    '3': ('SET_CORR_SPARTN', 'Use SPARTN / PointPerfect corrections (NTRIP)'),
    '4': ('SET_CORR_GAL_HAS','Use Galileo HAS free PPP corrections (disconnects NTRIP)'),
    '5': ('SET_CORR_NONE',   'Disable all corrections (disconnects NTRIP)'),
}

def select_com_port():
    ports = serial.tools.list_ports.comports()
    if not ports:
        print("No serial ports found.")
        return None
    print("\nAvailable COM ports:")
    for i, p in enumerate(ports):
        print(f"  [{i}] {p.device}  —  {p.description}")
    while True:
        choice = input(f"Select port [0-{len(ports)-1}]: ").strip()
        if choice.isdigit() and 0 <= int(choice) < len(ports):
            return ports[int(choice)].device
        print("  Invalid choice, try again.")

def select_launch_commands():
    print("\nOptional startup commands (sent once after connecting):")
    print("  [Enter] Skip — send no commands")
    for key, (cmd, desc) in LAUNCH_COMMANDS.items():
        print(f"  [{key}] {cmd}  —  {desc}")
    choices = input("Select commands to send (e.g. 1 3, or Enter to skip): ").strip()
    commands = []
    for token in choices.split():
        if token in LAUNCH_COMMANDS:
            commands.append(LAUNCH_COMMANDS[token][0])
        else:
            print(f"  Warning: '{token}' is not a valid choice, skipped.")
    return commands

def build_log_filename(commands):
    tags = []
    for cmd in commands:
        if cmd == 'LOG_GNSS':
            tags.append('gnss')
        elif cmd == 'LOG_ALL':
            tags.append('all')
        elif cmd == 'SET_CORR_SPARTN':
            tags.append('spartn')
        elif cmd == 'SET_CORR_GAL_HAS':
            tags.append('has')
        elif cmd == 'SET_CORR_NONE':
            tags.append('nocorr')
    suffix = ('_' + '_'.join(tags)) if tags else ''
    timestamp = time.strftime('%Y%m%d_%H%M%S')
    return f'log{suffix}_{timestamp}.jsonl'

# --- Setup ---
BAUD_RATE = 250000
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
LOG_DIR = os.path.join(SCRIPT_DIR, 'log_data')
os.makedirs(LOG_DIR, exist_ok=True)

serial_port = select_com_port()
if serial_port is None:
    raise SystemExit("No port available.")

launch_commands = select_launch_commands()
log_filename = build_log_filename(launch_commands)
LOG_FILE = os.path.join(LOG_DIR, log_filename)

print(f"\nLog file : {LOG_FILE}")
print(f"Commands : {launch_commands if launch_commands else '(none)'}")
print("Press Ctrl+C to stop.\n")

_FIX_LABELS = {(0, 0): 'No fix', (0, 1): 'No fix', (0, 2): 'No fix',
               (1, 0): '3D fix', (1, 1): 'Float',   (1, 2): 'RTK fix'}

def fix_label(fix_type, flags):
    carr = (flags >> 6) & 0x3
    has_fix = 1 if fix_type > 0 else 0
    return _FIX_LABELS.get((has_fix, carr), '?')

line_count = 0
time_start = time.time()
last_print = time_start
last_fix = 'No fix'

try:
    ser = serial.Serial(serial_port, BAUD_RATE, timeout=1)
    print(f"Connected to {serial_port} at {BAUD_RATE} baud.")

    for cmd in launch_commands:
        ser.write((cmd + '\r\n').encode('utf-8'))
        time.sleep(0.1)
        print(f"  Sent: {cmd}")

    with open(LOG_FILE, mode='a', encoding='utf-8') as file:
        while True:
            if ser.in_waiting > 0:
                try:
                    raw_line = ser.readline().decode('utf-8', errors='ignore').strip()
                    if raw_line:
                        parts = raw_line.split(',')
                        if not parts:
                            continue

                        sensor_type = parts[0]
                        data_dict = None

                        try:
                            if sensor_type == 'TEST' and len(parts) >= 2:
                                data_dict = {"sensor": "TEST", "message": parts[1]}
                            elif sensor_type == 'ISM330' and len(parts) >= 8:
                                data_dict = {
                                    "sensor": "ISM330", "ts": parts[1],
                                    "acc_x": int(parts[2]), "acc_y": int(parts[3]), "acc_z": int(parts[4]),
                                    "gyr_x": int(parts[5]), "gyr_y": int(parts[6]), "gyr_z": int(parts[7])
                                }
                            elif sensor_type == 'LSM_ACC' and len(parts) >= 5:
                                data_dict = {
                                    "sensor": "LSM_ACC", "ts": parts[1],
                                    "acc_x": int(parts[2]), "acc_y": int(parts[3]), "acc_z": int(parts[4])
                                }
                            elif sensor_type == 'LSM_MAG' and len(parts) >= 5:
                                data_dict = {
                                    "sensor": "LSM_MAG", "ts": parts[1],
                                    "mag_x": int(parts[2]), "mag_y": int(parts[3]), "mag_z": int(parts[4])
                                }
                            elif sensor_type == 'GNSS_PVT' and len(parts) >= 30:
                                data_dict = {
                                    "sensor": "GNSS_PVT",
                                    "epoch_us": parts[1], "year": int(parts[2]), "month": int(parts[3]),
                                    "day": int(parts[4]), "hour": int(parts[5]), "min": int(parts[6]),
                                    "sec": int(parts[7]), "nano": int(parts[8]), "iTOW": int(parts[9]),
                                    "valid": int(parts[10]), "tAcc": int(parts[11]), "fixType": int(parts[12]),
                                    "flags": int(parts[13]), "numSV": int(parts[14]), "lon": int(parts[15]),
                                    "lat": int(parts[16]), "height": int(parts[17]), "hMSL": int(parts[18]),
                                    "hAcc": int(parts[19]), "vAcc": int(parts[20]), "velN": int(parts[21]),
                                    "velE": int(parts[22]), "velD": int(parts[23]), "gSpeed": int(parts[24]),
                                    "heading": int(parts[25]), "sAcc": int(parts[26]),
                                    "headingAcc": int(parts[27]), "pDOP": int(parts[28]),
                                    "correction_source": parts[29]
                                }
                        except ValueError:
                            pass

                        if data_dict:
                            if data_dict['sensor'] == 'GNSS_PVT':
                                last_fix = fix_label(data_dict['fixType'], data_dict['flags'])
                            json_line = json.dumps(data_dict)
                            file.write(json_line + '\n')
                            line_count += 1
                            now = time.time()
                            if now - last_print >= 5.0:
                                file.flush()
                                last_print = now
                                print(f"Recording ({int(now - time_start)}s)  Lines logged: {line_count}  GNSS: {last_fix}")

                except Exception as e:
                    print(f"Error processing line: {e}")

except serial.SerialException:
    print(f"\nError: Could not open {serial_port}.")

except KeyboardInterrupt:
    print("\nData collection stopped by user.")
    print(f"Total recording time : {int(time.time() - time_start)} seconds")
    print(f"Total lines logged   : {line_count}")

finally:
    if 'ser' in locals() and ser.is_open:
        ser.close()
        print("Serial port closed.")
