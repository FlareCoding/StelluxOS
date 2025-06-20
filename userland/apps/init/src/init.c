#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stlibc/stlibc.h>

int main() {
    // Test malloc/free functionality
    int* arr = (int*)malloc(sizeof(int) * 10);
    printf("StelluxOS Init Process\n");
    printf("======================\n");
    printf("Allocated array at: 0x%p\n", arr);
    
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

    float x = 4.5;
    float y = 5.6;
    float z = x * y;
    

    printf("%f * %f = %f\n", x, y, z);
    
    for (int i = 0; i < 100000000; i++) {
        x++;
    }

    return 0;
}
