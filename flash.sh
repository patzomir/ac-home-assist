source source-idf.sh

cd firmware/zigbee-hub
idf.py set-target esp32c6
idf.py build
# idf.py flash --erase-all
cd ../../
