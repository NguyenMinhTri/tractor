#!/bin/zsh

idf > /dev/null
mac=$(esptool.py read_mac | grep -o -E ..:..:..:..:..:.. | head -n 1 | tr -d :)
echo $mac
gcloud --project=heartflow iot devices create $mac --region=us-central1 --registry=alpha_registry --device-type=non-gateway --public-key path=$1,type=es256-pem
