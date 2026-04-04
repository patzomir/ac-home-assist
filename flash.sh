source ~/.espressif/v5.5.3/esp-idf/export.sh && idf.py build 2>&1
cd firmware/zigbee-hub
idf.py set-target esp32c6
idf.py build
cd ../../
