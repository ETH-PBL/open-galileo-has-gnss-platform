import socket
import time

# The STM32's IP address and the TCP port it is listening on.
SERVER_IP = "192.168.137.42"  # This must match the IP_ADDRESS in your lwip.c
SERVER_PORT = 7

def main():
    # This outer loop will handle reconnecting if the connection is lost
    while True:
        try:
            # Create a TCP/IP socket (SOCK_STREAM for TCP)
            print(f"Attempting to connect to {SERVER_IP}:{SERVER_PORT}...")
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect((SERVER_IP, SERVER_PORT))
            print("Successfully connected to STM32. Receiving data...\n")

            # TCP is a stream, so we need to handle partial messages.
            # We'll buffer incoming data and process it line by line.
            buffer = b''
            while True:
                # Receive data from the server
                data = sock.recv(1024)
                if not data:
                    # Connection closed by the server
                    print("Connection closed by STM32. Reconnecting...")
                    break # Break inner loop to trigger reconnection

                buffer += data

                # Process any complete lines from the buffer
                while b'\n' in buffer:
                    line, buffer = buffer.split(b'\n', 1)
                    print(line.decode('utf-8', errors='ignore').strip())

        except socket.error as e:
            print(f"Connection failed: {e}. Retrying in 5 seconds...")
            time.sleep(5)
        except KeyboardInterrupt:
            print("\nListener stopped by user (Ctrl+C).")
            break
        except Exception as e:
            print(f"\nAn unexpected error occurred: {e}. Retrying in 5 seconds...")
            time.sleep(5)
        finally:
            # Cleanly close the socket before retrying or exiting
            if 'sock' in locals() and sock:
                sock.close()

if __name__ == "__main__":
    main()