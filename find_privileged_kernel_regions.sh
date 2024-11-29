#!/bin/bash

# Check if the kernel file exists
if [ ! -f "build/stellux" ]; then
  echo "Kernel not found!"
  exit 1
fi

# Function to resolve an address to a symbol (demangling included)
resolve_address() {
    local addr=$1
    # Get the full addr2line output
    local output=$(addr2line -e "build/stellux" -f -p "$addr")
    
    # Extract the function name and file:line info
    local function_name=$(echo "$output" | awk -F' at ' '{print $1}')
    local file_info=$(echo "$output" | awk -F' at ' '{print $2}')
    
    # Demangle the function name
    local demangled_name=$(echo "$function_name" | c++filt)
    
    # Remove the parameters from the demangled function name
    local simple_name=$(echo "$demangled_name" | awk -F'(' '{print $1}')
    
    # Extract just the filename from the file path
    local filename=$(basename "$file_info")
    
    # Print the sanitized and demangled output without parameters
    echo "$simple_name at $filename"
}

# Create an associative array to store section index to name mapping
declare -A section_map

# Populate section_map from readelf -S output
while read -r line; do
  if [[ $line =~ \[[[:space:]]*([0-9]+)\][[:space:]]+([.a-zA-Z0-9_]+) ]]; then
    index=${BASH_REMATCH[1]}
    name=${BASH_REMATCH[2]}
    section_map["$index"]=$name
  fi
done < <(readelf -S "build/stellux")

# Now read symbols from readelf -s and map them to sections
printf "%-60s %-20s %-20s\n" "SYMBOL" "ADDRESS" "SECTION"
readelf -s "build/stellux" | awk 'NR > 3 {print $2 " " $8 " " $7}' | while read -r address symbol section_index; do
  if [[ $section_index =~ ^[0-9]+$ ]] && [ "${section_map[$section_index]+isset}" ]; then
    section_name="${section_map[$section_index]}"
    if [[ $section_name == .k* ]]; then  # Check if section_name starts with .k
      # Resolve and demangle the symbol
      resolved_symbol=$(resolve_address "$address")
      printf "%-60s %-20s %-20s\n" "$resolved_symbol" "$address" "$section_name"
    fi
  fi
done | sort -k2  # Sort by the address field
