#!/bin/zsh
# publi.sh will bump up the project/app version number
# according to the tag chosen: major, minor, default (patch)
# build and upload the bin file

last_version_string=$(<version.txt)
last=("${(@s/./)last_version_string}")

next=(0 0 0)

if [ "$1" = "major" ]; then
    next[1]=$((last[1] + 1))
    next[2]=0
    next[3]=0
elif [ "$1" = "minor" ]; then
    next[1]=${last[1]}
    next[2]=$((last[2]+1))
    next[3]=0
else 
    next[1]=${last[1]}
    next[2]=${last[2]}
    next[3]=$((last[3]+1))
fi

last_string="${last[1]}.${last[2]}.${last[3]}"
next_string="${next[1]}.${next[2]}.${next[3]}"

echo "last version $last_string"
echo "next version $next_string"
echo "$next_string" > version.txt

# shellcheck disable=SC1090
source "$IDF_PATH"/export.sh;

idf.py app;

bucket="heartflow-bin/espressif-devkitc-v4"
file="tractor.$next_string.bin"
gs_filepath="gs://$bucket/$file"
gs_public_url="https://storage.googleapis.com/$bucket/$file"

gsutil cp -n -a public-read build/tractor.bin $gs_filepath

echo "FILE PUBLIC URL:"
echo $gs_public_url

