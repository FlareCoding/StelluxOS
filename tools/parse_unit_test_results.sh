#!/bin/bash

# File that contains the output log
log_file="unit_tests.out"

# Initialize arrays to store test results
passed_tests=()
failed_tests=()

# Check if log file exists
if [[ ! -f "$log_file" ]]; then
    echo "Error: $log_file not found."
    exit 1
fi

# Read the log file line by line
while IFS= read -r line; do
    # Look for passed tests
    if [[ "$line" == *"Test"* && "$line" == *"passed"* ]]; then
        # Extract test name inside the quotes
        test_name=$(echo "$line" | sed -n 's/.*Test "\(.*\)" passed.*/\1/p')
        passed_tests+=("$test_name")

    # Look for failed or critical failure tests
    elif [[ "$line" == *"Test"* && ( "$line" == *"failed"* || "$line" == *"critical failure"* ) ]]; then
        # Extract test name inside the quotes
        test_name=$(echo "$line" | sed -n 's/.*Test "\(.*\)".*/\1/p')
        failed_tests+=("$test_name")
    fi
done < "$log_file"

# Print the results
echo "Test Results:"
echo "============="

# Print passed tests
if [ ${#passed_tests[@]} -gt 0 ]; then
    echo -e "\nPassed Tests:"
    for test in "${passed_tests[@]}"; do
        echo "  - $test"
    done
else
    echo "No tests passed."
fi

# Print failed tests
if [ ${#failed_tests[@]} -gt 0 ]; then
    echo -e "\nFailed Tests:"
    for test in "${failed_tests[@]}"; do
        echo "  - $test"
    done
else
    echo "No tests failed."
fi

# Exit with code 1 if any test failed, otherwise 0
if [ ${#failed_tests[@]} -gt 0 ]; then
    echo -e "\n[FAIL] Some tests failed."
    exit 1
else
    echo -e "\n[OK] All tests passed successfully."
    exit 0
fi