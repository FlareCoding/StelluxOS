#!/bin/bash

# Check if the file exists
if [ ! -f "kernel/bin/kernel.elf" ]; then
  echo "Kernel not found!"
  exit 1
fi

# Create an associative array to store section index to name mapping
declare -A section_map

# Populate section_map from readelf -S output
while read -r line; do
  if [[ $line =~ \[[[:space:]]*([0-9]+)\][[:space:]]+([.a-zA-Z0-9_]+) ]]; then
    index=${BASH_REMATCH[1]}
    name=${BASH_REMATCH[2]}
    section_map["$index"]=$name
  fi
done < <(readelf -S "kernel/bin/kernel.elf")

# Now read symbols from readelf -s and map them to sections
printf "%-30s %-20s %-20s\n" "SYMBOL" "ADDRESS" "SECTION"
readelf -s "kernel/bin/kernel.elf" | awk 'NR > 3 {print $2 " " $8 " " $7}' | while read -r address symbol section_index; do
  if [[ $section_index =~ ^[0-9]+$ ]] && [ "${section_map[$section_index]+isset}" ]; then
    section_name="${section_map[$section_index]}"
    if [[ $section_name == .k* ]]; then  # Check if section_name starts with .k
      printf "%-30s %-20s %-20s\n" "$symbol" "$address" "$section_name"
    fi
  fi
done | sort -k2  # Sort by the address field
