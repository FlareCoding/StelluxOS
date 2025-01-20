#!/bin/bash

# Default path to the kernel ELF file
DEFAULT_KERNEL_ELF="build/stellux"

# Determine the ELF file to use (use the parameter if provided, otherwise fallback to the default)
KERNEL_ELF="${1:-$DEFAULT_KERNEL_ELF}"

# Function to resolve an address to a symbol
resolve_address() {
    local addr=$1
    # Get the full addr2line output
    local output=$(addr2line -e "$KERNEL_ELF" -f -p "$addr")
    
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

# Read the backtrace from the user
echo "Please paste the backtrace (end with an empty line):"
backtrace=()
while IFS= read -r line; do
    [[ -z $line ]] && break
    backtrace+=("$line")
done

# Parse the backtrace and resolve addresses
echo "======= RESOLVED BACKTRACE ======="
for line in "${backtrace[@]}"; do
    if [[ $line =~ 0x([a-fA-F0-9]+) ]]; then
        address="${BASH_REMATCH[1]}"
        symbol_info=$(resolve_address $address)
        echo "$address: $symbol_info"
    fi
done