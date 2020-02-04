#!/bin/zsh
# build and upload the current version bin file
# and then bump up the project/app version number
# according to the tag chosen: major, minor, default (patch)

this_version_string=$(<version.txt)
this=("${(@s/./)this_version_string}")
next=(0 0 0)

if [ "$1" = "major" ]; then
    next=($((this[1] + 1)) 0 0)
elif [ "$1" = "minor" ]; then
    next=("${this[1]}" $((this[2]+1)) 0)
else
    next=("${this[1]}" "${this[2]}" $((this[3]+1)))
fi

this_string="${this[1]}.${this[2]}.${this[3]}"
next_string="${next[1]}.${next[2]}.${next[3]}"

echo "last version $this_string"
echo "next version $next_string"

# shellcheck disable=SC1090
source "$IDF_PATH"/export.sh;

idf.py app;

bucket="heartflow-bin/espressif-devkitc-v4"
file="tractor.$this_string.bin"
gs_filepath="gs://$bucket/$file"
gs_public_url="https://storage.googleapis.com/$bucket/$file"

gsutil cp -n -a public-read build/tractor.bin "$gs_filepath"

echo "FILE PUBLIC URL:"
echo "$gs_public_url"

echo "$next_string" > version.txt

