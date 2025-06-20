#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <math.h>
#include <stlibc/stlibc.h>

int main() {
    // Test malloc/free functionality
    int* arr = (int*)malloc(sizeof(int) * 10);
    if (!arr) {
        return -1;
    }
    printf("StelluxOS Init Process\n");
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

    float x = 4.5;
    float y = 5.6;
    float z = x * y;

    printf("%f * %f = %f\n", x, y, z);
    printf("sqrt(25) = %f\n", sqrt(25));
    printf("sqrt(62) = %f\n", sqrt(62));

    return 0;
}
