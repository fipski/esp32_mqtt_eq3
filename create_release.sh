#!/bin/bash

if [ -d release ]; then
	rm -R release
fi
mkdir release

cp build/bootloader/bootloader.bin release
cp build/esp32_mqtt_eq3.bin release
cp build/ota_data_initial.bin release
cp build/partition_table/partition-table.bin release

