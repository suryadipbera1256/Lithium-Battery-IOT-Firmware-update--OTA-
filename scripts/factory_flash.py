"""
=========================================================
FACTORY FLASH SCRIPT (Python)
Automates the upload of AWS Root CA, Device Certificate,
and Private Key to the EC200U-CN Modem's UFS memory.
=========================================================
"""
import serial
import time
import os

# Configuration variables (Adjust COM port according to your OS)
SERIAL_PORT = 'COM3' 
BAUD_RATE = 115200

# Helper function to send AT commands and decode response
def send_at_command(ser, cmd, delay=1):
    print(f"Sending: {cmd}")
    ser.write((cmd + '\r\n').encode())
    time.sleep(delay)
    response = ser.read_all().decode(errors='ignore')
    print(f"Response: {response}")
    return response

# Uploads a file stream directly to modem memory via AT+QFUPL with Smart Replace
def upload_file(ser, filename, filepath):
    # Verify file exists locally
    if not os.path.exists(filepath):
        print(f"Error: {filepath} not found locally!")
        return
    # ========================================================
    # NEW LOGIC: Check, Log, and Replace specific certificate
    # ========================================================
    print(f"\n--- Checking existence of {filename} in UFS ---")
    
    # 1. Check if file exists
    ser.write((f'AT+QFLST="{filename}"\r\n').encode())
    time.sleep(1)
    check_resp = ser.read_all().decode(errors='ignore')
    
    # 2. If/Else logic for Replacement
    if "+QFLST:" in check_resp:
        print(f"[LOG] {filename} already exists. Deleting old version for replacement...")
        ser.write((f'AT+QFDEL="{filename}"\r\n').encode())
        time.sleep(1)
        ser.read_all() # Clear buffer after deletion
        print(f"[LOG] Old {filename} successfully deleted.")
    else:
        print(f"[LOG] {filename} does not exist. Proceeding to fresh upload.")
    # ========================================================

    file_size = os.path.getsize(filepath)
    print(f"--- Uploading new {filename} ({file_size} bytes) ---")
    
    # Prepare the modem to receive the file
    cmd = f'AT+QFUPL="{filename}",{file_size},100'
    ser.write((cmd + '\r\n').encode())
    time.sleep(2)
    
    response = ser.read_all().decode(errors='ignore')
    
    # Check for CONNECT prompt before sending raw bytes
    if "CONNECT" in response:
        with open(filepath, 'rb') as f:
            ser.write(f.read()) # Dispatch binary stream
        time.sleep(3)
        print(ser.read_all().decode(errors='ignore'))
        print(f"[SUCCESS] Successfully uploaded {filename} to UFS.")
    else:
        print(f"[ERROR] Failed to get CONNECT prompt for {filename}. Response: {response}")

def main():
    try:
        # Establish serial connection
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=2)
        print("Connected to serial port, waiting for ESP32 to boot...")
        
        # ESP32 রিস্টার্ট হওয়ার সময় গারবেজ এড়ানোর জন্য ৩ সেকেন্ড অপেক্ষা
        time.sleep(3)
        ser.read_all() # Clear bootloader logs
        
        print("Contacting modem...")
        
        # Wake up modem / Check AT readiness
        send_at_command(ser, "AT")
        
        # Sequentially upload the three required AWS mTLS certificates
        upload_file(ser, "rootCA.pem", "certs/rootCA.pem")
        upload_file(ser, "cert.pem", "certs/cert.pem")
        upload_file(ser, "privkey.pem", "certs/privkey.pem")
        
        # Verify the files are present in the modem's filesystem
        print("\n--- Verifying Uploaded Files in UFS ---")
        send_at_command(ser, "AT+QFLST")
        
        ser.close()
        print("Factory flashing process completed successfully.")
        
    except Exception as e:
        print(f"Serial Connection Error: {e}")

if __name__ == "__main__":
    main()