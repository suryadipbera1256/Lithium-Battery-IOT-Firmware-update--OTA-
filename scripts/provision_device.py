"""
=========================================================
ZERO-TOUCH PROVISIONING SCRIPT
Orchestrates: AT Bridge -> Cert Flash -> Prod Firmware
=========================================================
"""
import subprocess
import time
import sys

def run_command(command, description):
    print(f"\n{'='*65}")
    print(f"🚀 STEP: {description}")
    print(f"🔧 CMD: {command}")
    print(f"{'='*65}\n")
    
    # Run command and pipe output directly to terminal
    result = subprocess.run(command, shell=True)
    
    if result.returncode != 0:
        print(f"\n❌ [FATAL ERROR] Pipeline halted at: {description}")
        sys.exit(1)
    print(f"\n✅ [SUCCESS] {description} completed successfully.\n")

def main():
    print("🌟 Starting Automated Zero-Touch Provisioning Pipeline 🌟\n")
    
    # Step 1: Upload AT Bridge Firmware
    run_command("pio run -e provision -t upload", "Deploying AT Bridge Firmware")
    
    # Hardware Stabilization Delay
    print("⏳ Waiting 5 seconds for ESP32 & Modem to stabilize in Bridge mode...")
    time.sleep(5)
    
    # Step 2: Flash AWS Certificates to Modem UFS
    run_command("python scripts/factory_flash.py", "Flashing AWS mTLS Certificates")
    
    # Step 3: Upload Production Firmware & Inject Env Vars (.env)
    run_command("pio run -e prod -t upload", "Compiling & Deploying Production Firmware (with .env injection)")
    
    # Step 4: Open Serial Monitor
    print("\n🎉 Provisioning Complete! Device is Production-Ready.")
    print("📡 Launching Serial Monitor...\n")
    subprocess.run("pio device monitor -b 115200", shell=True)

if __name__ == "__main__":
    main()