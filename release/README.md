# Prebuilt firmware (v0.2.4 control reliability)

ESP-IDF 5.5.1 · dual OTA slots · SoftAP web UI

## Flash
```bash
python -m pip install esptool
./flash.sh /dev/ttyACM0
```

## After flash
1. Join Wi-Fi **BoostGauge-XXXX**
2. Open **http://192.168.4.1/**
3. Use OTA page for wireless updates later
