CFLAGS = -m64 -g -ffreestanding -O0 -Wall -Wextra
LDFLAGS = -nostdlib -T kstellux.ld -o kernel.elf

kernel.elf: entry/kernel_entry.o
	$(LD) $(LDFLAGS) $^

entry/kernel_entry.o: entry/kernel_entry.c entry/entry_params.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o *.bin *.elf entry/*.o
