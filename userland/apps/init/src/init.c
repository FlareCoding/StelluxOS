#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stlibc/stlibc.h>

int main() {
    // Test malloc/free functionality
    int* arr = (int*)malloc(sizeof(int) * 10);
    printf("StelluxOS Init Process\n");
    printf("======================\n");
    printf("Allocated array at: %p\n", arr);
    
    for (int i = 0; i < 10; i++) {
        arr[i] = i * i;  // Store squares
    }
    
    printf("Array contents (squares): ");
    for (int i = 0; i < 10; i++) {
        printf("%d\n", arr[i]);
    }
    printf("\n");

    free(arr);

    printf("mypid: %d\n", getpid());
    
    return 0;
}
