# chip8
Chip8 emulator for Windows/Linux written in C

Compile on Windows: gcc -I src/include -L src/lib -o main src/main.c -lmingw32 -lSDL2main -lSDL2

Compile on Linux: gcc -I src/include -o main src/main.c -lSDL2main -lSDL2

Run the emulator: main.exe "rom_path.ch8"