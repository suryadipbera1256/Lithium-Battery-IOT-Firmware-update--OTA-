# Enterprise Battery Fleet Diagnostics Firmware 

![Version](https://img.shields.io/badge/Version-v1.0.0-blue)
![Platform](https://img.shields.io/badge/Platform-ESP32-orange)
![Connectivity](https://img.shields.io/badge/Connectivity-AWS_IoT_Core-green)
![Framework](https://img.shields.io/badge/Framework-PlatformIO-lightgrey)

## 📌 Overview
This repository contains the enterprise-grade firmware for the **Pointo AI** Battery Management System (BMS) telemetry unit. Built on the ESP32 micro-controller and Quectel EC200U-CN LTE modem, it utilizes **AWS IoT Core** with Mutual TLS (mTLS) for highly secure, real-time data streaming and over-the-air (OTA) updates via AWS IoT Jobs..

## ✨ Key Features
* **CAN Bus Integration:** Real-time data parsing from BMS (Voltage, Current, SOC, Cell parameters) via standard CAN protocol (250kbps/500kbps).
* **Enterprise Security (mTLS):** X.509 certificate-based authentication with AWS IoT Core.
* **AWS OTA Updates:** Automated firmware updates streaming directly to ESP32 flash via presigned S3 URLs using AWS IoT Jobs.
* **Advanced Telemetry:** Dual location tracking (GNSS/GPS with LBS fallback) and odometry calculation.
* **Environmental Monitoring:** Integrated BME680 (Temp, Humidity, Pressure, VOC) and MQ2/MQ8 gas sensors for battery safety.

## 🗂️ Project Structure
```text
pointo-ai-firmware/
├── certs/                  # UFS certificates (Ignored in Git)
├── include/                # Header files (config.h, EC200U_AWS_OTA.h)
├── lib/                    # Custom/3rd-party libraries
├── scripts/                # Python automation scripts (factory_flash.py)
├── src/                    # Main C++ source files (main.cpp)
├── .env                    # Environment variables (Ignored in Git)
├── .gitignore              # Git ignore rules
├── platformio.ini          # PlatformIO build configuration
├── requirements.txt        # Python dependencies
└── README.md               # Project documentation