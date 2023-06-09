#include "include/SDL2/SDL.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
//#include <sys/time.h>
#include <sys/timeb.h>
//#include <unistd.h>

#ifdef _WIN32
#include <Windows.h>
#endif // _WIN32_

int SCREEN_WIDTH;
int SCREEN_HEIGHT;

char *rom_path;

int RAM_SIZE;
int VRAM_SIZE; // vram size in bytes
int VRAM_START_BYTE; // the byte in emu_ram where vram starts
int FONT_START_BYTE; // the byte in emu_ram where font data starts

int FPS; // frames per second (when FPS is unhooked)
int IPS; // number of chip8 instructions per second
int TIMER_FREQUENCY; // number of times the timers decrement in a second
int UNHOOK_FPS; // when set to 1, refreshes the screen FPS times per second instead of every draw/clear command

uint32_t *palette; // RGBA values for the two screen colours

uint8_t *emu_ram;

int emu_stack_max;
uint16_t *emu_stack;
int emu_stack_top;

int PROGRAM_START_BYTE; // the emu_ram address where the program is loaded and started from
uint16_t PC;

uint8_t delay_timer;
uint8_t sound_timer;   // sound not implimented

uint16_t I;

uint8_t *V;

int COPY_SHIFT = 0; // defines if VY should be copied to VX before a shift instruction
int JUMP_OFFSET_MODE = 0; // defines whether to use the old behavior (0) or new behaviour (1) for the BNNN instruction
int LOAD_STORE_MODE = 1; // defines whether to use the old behavior (0) or new behavior (1) for the FX55 and FX65 instructions

static uint8_t font[80] = {
  0xF0, 0x90, 0x90, 0x90, 0xF0, // 0
0x20, 0x60, 0x20, 0x20, 0x70, // 1
0xF0, 0x10, 0xF0, 0x80, 0xF0, // 2
0xF0, 0x10, 0xF0, 0x10, 0xF0, // 3
0x90, 0x90, 0xF0, 0x10, 0x10, // 4
0xF0, 0x80, 0xF0, 0x10, 0xF0, // 5
0xF0, 0x80, 0xF0, 0x90, 0xF0, // 6
0xF0, 0x10, 0x20, 0x40, 0x40, // 7
0xF0, 0x90, 0xF0, 0x90, 0xF0, // 8
0xF0, 0x90, 0xF0, 0x10, 0xF0, // 9
0xF0, 0x90, 0xF0, 0x90, 0x90, // A
0xE0, 0x90, 0xE0, 0x90, 0xE0, // B
0xF0, 0x80, 0x80, 0x80, 0xF0, // C
0xE0, 0x90, 0x90, 0x90, 0xE0, // D
0xF0, 0x80, 0xF0, 0x80, 0xF0, // E
0xF0, 0x80, 0xF0, 0x80, 0x80 // F
};

void initialize_settings() {
    SCREEN_WIDTH = 64;
    SCREEN_HEIGHT = 32;

    RAM_SIZE = 4096;
    VRAM_SIZE =  (SCREEN_WIDTH * SCREEN_HEIGHT / 8);
    VRAM_START_BYTE = 0xF00;
    FONT_START_BYTE = 0x050;

    FPS = 60;
    IPS = 700;
    TIMER_FREQUENCY = 60;
    UNHOOK_FPS = 0;

    palette = malloc(sizeof(uint32_t) * 2);
    palette[0] = 0x000000FF;
    palette[1] = 0xFFFF00FF;

    emu_ram = malloc(sizeof(uint8_t) * RAM_SIZE);

    emu_stack_max = 16;
    emu_stack = malloc(sizeof(uint16_t) * emu_stack_max);
    emu_stack_top = -1;

    PROGRAM_START_BYTE = 0x200;
    PC = PROGRAM_START_BYTE;

    delay_timer = 255;
    sound_timer = 255;

    V = malloc(sizeof(uint16_t) * 16);
}

SDL_Texture *screen_tex; // SDL texture that holds the contents of the screen
uint8_t *screen_pixels; // array of screen pixels used to build screen_tex
SDL_Renderer* screen_ren; // main SLD renderer

uint8_t *previous_keyboard;
uint8_t *current_keyboard;

uint8_t keypad_states[16] = {0}; // the up/down states of the 16 keys on the chip8 keypad (0 is up, 1 is down)
uint8_t get_key_status = 0; // decides what should be accessing the get_key_key variable
                            // 0 - not in use
                            // 1 - input detection should be deciding what key is being pressed
                            // 2 - a key has been pressed since the get key instruction was started and the instruction should now read the get_key_key variable
int get_key_key = -1; // the keypad id used in the get key instruction. <0 means not valid, 0-15 are the keypad values


static void update_timers() {
  if (delay_timer > 0) {
    delay_timer -= 1;
  }
  if (sound_timer > 0) {
    sound_timer -= 1;
  }
}

static uint16_t emu_stack_peek() {
  if (emu_stack_top < 0) {
    // stack empty
    printf("emu_stack empty read\n");
    return 0;
  }
  else {
    return emu_stack[emu_stack_top];
  }
}

static uint16_t emu_stack_pop() {
  if (emu_stack_top < 0) {
    // stack empty
    printf("emu_stack empty pop\n");
    return 0;
  }
  else {
    uint16_t top_value = emu_stack[emu_stack_top];
    emu_stack_top -= 1;
    return top_value;
  }
}

static void emu_stack_push(uint16_t value) {
  if (emu_stack_top >= emu_stack_max - 1) {
    // stack full
    printf("emu_stack full\n");
  }
  else{
    emu_stack_top += 1;
    emu_stack[emu_stack_top] = value;
  }
  
}

uint16_t reverse16(uint16_t value) {
  return ((value & 0x00FF) << 8) | ((value & 0xFF00) >> 8);
}

uint16_t get_emu_ram_pixel_byte(uint16_t x, uint16_t y) {
  return (y * (SCREEN_WIDTH / 8)) + (x / 8) + VRAM_START_BYTE;
}

uint16_t get_emu_ram_pixel_byte_offset(uint16_t x) {
  return 7 - (x % 8);
}

uint16_t get_emu_ram_pixel(uint16_t ram_byte_index, uint16_t offset) {
  return (emu_ram[ram_byte_index] >> offset) & 1;
}

void set_emu_ram_pixel(uint16_t ram_byte_index, uint16_t offset, bool value) {
  if (value == true) {
    emu_ram[ram_byte_index] |= 1UL << offset;
  }
  else {
    emu_ram[ram_byte_index] &= ~(1UL << offset);
  }
}

void draw_frame();

/* Size of each input chunk to be
   read and allocate for. */
#ifndef  READALL_CHUNK
#define  READALL_CHUNK  262144
#endif

#define  READALL_OK          0  /* Success */
#define  READALL_INVALID    -1  /* Invalid parameters */
#define  READALL_ERROR      -2  /* Stream error */
#define  READALL_TOOMUCH    -3  /* Too much input */
#define  READALL_NOMEM      -4  /* Out of memory */

/* This function returns one of the READALL_ constants above.
   If the return value is zero == READALL_OK, then:
     (*dataptr) points to a dynamically allocated buffer, with
     (*sizeptr) chars read from the file.
     The buffer is allocated for one extra char, which is NUL,
     and automatically appended after the data.
   Initial values of (*dataptr) and (*sizeptr) are ignored.
*/
int readall(FILE *in, char **dataptr, size_t *sizeptr)
{
    char  *data = NULL, *temp;
    size_t size = 0;
    size_t used = 0;
    size_t n;

    //printf("Parameters: %d, %d, %d\n", in, dataptr, sizeptr);

    /* None of the parameters can be NULL. */
    if (in == NULL || dataptr == NULL || sizeptr == NULL)
        return READALL_INVALID;

    /* A read error already occurred? */
    if (ferror(in))
        return READALL_ERROR;

    while (1) {

        if (used + READALL_CHUNK + 1 > size) {
            size = used + READALL_CHUNK + 1;

            /* Overflow check. Some ANSI C compilers
               may optimize this away, though. */
            if (size <= used) {
                free(data);
                return READALL_TOOMUCH;
            }

            temp = realloc(data, size);
            if (temp == NULL) {
                free(data);
                return READALL_NOMEM;
            }
            data = temp;
        }

        n = fread(data + used, 1, READALL_CHUNK, in);
        if (n == 0)
            break;

        used += n;
    }

    if (ferror(in)) {
        free(data);
        return READALL_ERROR;
    }

    temp = realloc(data, used + 1);
    if (temp == NULL) {
        free(data);
        return READALL_NOMEM;
    }
    data = temp;
    data[used] = '\0';

    *dataptr = data;
    *sizeptr = used;

    return READALL_OK;
}

static void initialize_emu_ram() {
    //emu_ram[VRAM_START_BYTE] = 0b11111111;
    //emu_ram[VRAM_START_BYTE + 1] = 0b00001111;
    //emu_ram[VRAM_START_BYTE] = 0b10000000;
    //emu_ram[VRAM_START_BYTE + 7] = 0b10000001;
    //emu_ram[VRAM_START_BYTE + 15] = 0b10000001;
    //emu_ram[VRAM_START_BYTE + VRAM_SIZE - 1] = 0b10000001;

    // for (int i = 0; i < VRAM_SIZE; i++) {
    //     emu_ram[VRAM_START_BYTE + i] = 0b11111111;
    // }

    // emu_ram[VRAM_START_BYTE + 15] = 0b10000001;

    // load font
    memcpy(&emu_ram[FONT_START_BYTE], &font, 80*sizeof(*font));
}

// load rom file into emu_ram
void load_rom(char *rom) {
    FILE *rom_file = fopen(rom, "rb");
    char *rom_data;
    size_t rom_size = 0;
    int suc = readall(rom_file, &rom_data, &rom_size);
    //printf("Rom size: %d\n", rom_size);
    //printf("Rom status: %d\n", suc);
    memcpy(&emu_ram[PROGRAM_START_BYTE], rom_data, rom_size);
}

// runs one Chip8 instruction at the current PC
void run_next_instruction() {
  // fetch (CAREFUL: CHIP8 is big endian C is little endian)
  uint8_t byte1 = emu_ram[PC];
  uint8_t byte2 = emu_ram[PC + 1];
  //uint16_t instruction = (byte2 << 8) | byte1;
  uint16_t instruction = (byte1 << 8) | byte2;
  PC += 2;

  //printf("Instruction: %d\n", instruction);

  // decode
  //uint16_t opcode = (instruction & 0b0000000011110000) >> 4;
  uint16_t opcode = (instruction & 0b1111000000000000) >> 12;

  switch(opcode) {
    case 0x0:
    {
      if (instruction == 0x00E0) {
        // Clear screen
        for (int i = 0; i < VRAM_SIZE; i++) {
          emu_ram[VRAM_START_BYTE + i] = 0;
        }
        if (UNHOOK_FPS == 0) {
          draw_frame();
        }
      }
      else if (instruction == 0x00EE) {
        // Return
        uint16_t NNN = emu_stack_pop();
        PC = NNN;
      }
      else {
        printf("Invalid instruction %d\n", instruction);
      }
      break;
    }
    case 0x1:
    {
      // Jump
      uint16_t NNN = instruction & 0b0000111111111111;
      PC = NNN;
      break;
    }
    case 0x2:
    {
      // Subroutine
      uint16_t NNN = instruction & 0b0000111111111111;
      emu_stack_push(PC);
      PC = NNN;
      break;
    }
    case 0x3:
    {
      // Jump if equal
      uint16_t X = (instruction & 0b0000111100000000) >> 8;
      uint8_t NN = instruction & 0b0000000011111111;
      if (V[X] == NN) {
        PC += 2;
      }
      break;
    }
    case 0x4:
    {
      // Jump if not equal
      uint16_t X = (instruction & 0b0000111100000000) >> 8;
      uint8_t NN = instruction & 0b0000000011111111;
      if (V[X] != NN) {
        PC += 2;
      }
      break;
    }
    case 0x5:
    {
      // Jump if equal
      uint16_t X = (instruction & 0b0000111100000000) >> 8;
      uint16_t Y = (instruction & 0b0000000011110000) >> 4;
      if (V[X] == V[Y]) {
        PC += 2;
      }
      break;
    }
    case 0x6:
    {
      // Set Register
      uint16_t X = (instruction & 0b0000111100000000) >> 8;
      uint8_t NN = instruction & 0b0000000011111111;
      V[X] = NN;
      break;
    }
    case 0x7:
    {
      // Add to Register
      uint16_t X = (instruction & 0b0000111100000000) >> 8;
      uint8_t NN = instruction & 0b0000000011111111;
      V[X] += NN;
      break;
    }
    case 0x8:
    {
      // Logical and arithmetic instructions
      uint16_t X = (instruction & 0b0000111100000000) >> 8;
      uint16_t Y = (instruction & 0b0000000011110000) >> 4;
      uint16_t N = instruction & 0b0000000000001111;
      
      switch(N) {
        case 0x0:
        {
          // Set
          V[X] = V[Y];
          break;
        }
        case 0x1:
        {
          // Binary OR
          V[X] = V[X] | V[Y];
          break;
        }
        case 0x2:
        {
          // Binary AND
          V[X] = V[X] & V[Y];
          break;
        }
        case 0x3:
        {
          // Logical XOR
          V[X] = V[X] ^ V[Y];
          break;
        }
        case 0x4:
        {
          // Add (with overflow flag)
          uint16_t temp = (uint16_t)V[X] + (uint16_t)V[X];
          V[X] = V[X] + V[Y];
          if (temp > 255) {
            V[0xF] = 1;
          }
          else {
            V[0xF] = 0;
          }
          break;
        }
        case 0x5:
        {
          // Subtract VX - VY (with overflow flag)
          bool underflow = V[X] < V[Y];
          V[X] = V[X] - V[Y];
          if (!underflow) {
            V[0xF] = 1;
          }
          else {
            V[0xF] = 0;
          }
          break;
        }
        case 0x7:
        {
          // Subtract VY - VX (with overflow flag)
          bool underflow = V[Y] < V[X];
          V[X] = V[Y] - V[X];
          if (!underflow) {
            V[0xF] = 1;
          }
          else {
            V[0xF] = 0;
          }
          break;
        }
        case 0x6:
        {
          // Right shift
          if (COPY_SHIFT == 1) {
            V[X] = V[Y];
          }
          bool shifted_1 = V[X] & 0b00000001;
          V[X] = V[X] >> 1;
          if (shifted_1) {
            V[0xF] = 1;
          }
          else {
            V[0xF] = 0;
          }
          break;
        }
        case 0xE:
        {
          // Left shift
          if (COPY_SHIFT == 1) {
            V[X] = V[Y];
          }
          bool shifted_1 = V[X] & 0b10000000;
          V[X] = V[X] << 1;
          if (shifted_1) {
            V[0xF] = 1;
          }
          else {
            V[0xF] = 0;
          }
          break;
        }
      }

      break;
    }
    case 0x9:
    {
      // Jump if not equal
      uint16_t X = (instruction & 0b0000111100000000) >> 8;
      uint16_t Y = (instruction & 0b0000000011110000) >> 4;
      if (V[X] != V[Y]) {
        PC += 2;
      }
      break;
    }
    case 0xA:
    {
      // Set Index
      uint16_t NNN = instruction & 0b0000111111111111;
      I = NNN;
      break;
    }
    case 0xB:   // untested
    {
      // Jump with offset
      if (JUMP_OFFSET_MODE == 0) {
        // Old way
        uint16_t NNN = instruction & 0b0000111111111111;
        PC = NNN + V[0x0];
      }
      else {
        // New way
        uint16_t X = (instruction & 0b0000111100000000) >> 8;
        uint16_t XNN = instruction & 0b0000111111111111;
        PC = XNN + V[X];
      }
      break;
    }
    case 0xC:   // untested
    {
      // Random
      uint16_t X = (instruction & 0b0000111100000000) >> 8;
      uint8_t NN = instruction & 0b0000000011111111;
      V[X] = (rand() % 255) & NN;
      break;
    }
    case 0xD:
    {
      // Display
      uint16_t X = (instruction & 0b0000111100000000) >> 8;
      uint16_t Y = (instruction & 0b0000000011110000) >> 4;
      uint16_t N = instruction & 0b0000000000001111;

      uint16_t coord_x = V[X] % SCREEN_WIDTH;
      uint16_t coord_y = V[Y] % SCREEN_HEIGHT;
      V[0xF] = 0;


      for (int row = 0; row < N; row++) {
        uint8_t sprite_byte = emu_ram[I + row];
        for (uint8_t bit_offset = 0; bit_offset < 8; bit_offset++) {
          if(sprite_byte >> (7 - bit_offset) & 0b00000001) {
            // Spite bit is on so this bit should be flipped on screen
            int x = coord_x + bit_offset;
            int y = coord_y + row;
            //APP_LOG(APP_LOG_LEVEL_DEBUG, "XCo: %d, YCo: %d", x, y);
            if (x < SCREEN_WIDTH && y < SCREEN_HEIGHT) {
              if(get_emu_ram_pixel(get_emu_ram_pixel_byte(x, y), get_emu_ram_pixel_byte_offset(x))) {
                // Bit was already on. Flip off and set VF to 1
                set_emu_ram_pixel(get_emu_ram_pixel_byte(x, y), get_emu_ram_pixel_byte_offset(x), 0);
                V[0xF] = 1;
              }
              else {
                // Bit was off. Flip on
                set_emu_ram_pixel(get_emu_ram_pixel_byte(x, y), get_emu_ram_pixel_byte_offset(x), 1);
              }
            }
          }
        }
      }


      if (UNHOOK_FPS == 0) {
        draw_frame();
      }

      break;
    }
    case 0xE:   // untested
    {
      // Skip if key
      uint16_t X = (instruction & 0b0000111100000000) >> 8;
      uint8_t NN = instruction & 0b0000000011111111;

      if (V[X] > 0xF) {
        printf("Warning: skip if key instuction requested invalid key number (crash likely)\n");
      }

      if (NN == 0x9E) {
        // Skip if key pressed
        if(keypad_states[V[X]] == 1) {
            PC += 2;
        }
      }
      else if (NN == 0xA1) {
        // skip if not pressed
        if (keypad_states[V[X]] == 0) {
            PC += 2;
        }
      }
      else {
        printf("Invalid if key instruction\n");
      }
      break;
    }
    case 0xF:
    {
      // Other functions
      uint16_t X = (instruction & 0b0000111100000000) >> 8;
      uint8_t NN = instruction & 0b0000000011111111;
      switch(NN) {
        // Timer Functions
        case 0x07:  // untested
        {
          V[X] = delay_timer;
          break;
        }
        case 0x15:  // untested
        {
          delay_timer = V[X];
          break;
        }
        case 0x18:  // untested
        {
          sound_timer = V[X];
          break;
        }

        // Index function
        case 0x1E:  // untested
        {
          I += V[X];
          // overflow out of address space sets VF to 1 (not the case on original hardware)
          if (I >= 0x1000) {
            V[0xF] = 1;
          }
          break;
        }

        // Get key function
        case 0x0A:  // untested
        {
          if (get_key_status == 2) {    // key has been pressed
            if (get_key_key < 0) {
                printf("Tried to run Get key when no key was set\n");
            }
            else if (get_key_key > 0xF) {
                printf("Tried to run Get key with an invalid key\n");
            }
            else {
                // successfully got key
                V[X] = get_key_key;
                get_key_status = 0;
            }
          }
          else if (get_key_status == 0) {
            // Start waiting for key
            get_key_status = 1;
            PC -= 2;
          }
          else {
            // Waiting for key press
            PC -= 2;
          }
          break;
        }

        // Font character function
        case 0x29:  // untested
        {
          I = FONT_START_BYTE + (5 * V[X]); // this asumes that the first 4 bits of V[X] are empty
          break;
        }

        // Binary-coded decimal conversion function
        case 0x33:
        {
          uint8_t D0 = V[X] / 100;
          uint8_t D1 = (V[X] / 10) % 10;
          uint8_t D2 = V[X] % 10;

          emu_ram[I] = D0;
          emu_ram[I + 1] = D1;
          emu_ram[I + 2] = D2;

          break;
        }

        // Store and load functions
        case 0x55:
        {
          // Store function
          // New method (temp variable)
          uint8_t index = 0;
          for (uint8_t i = 0; i <= X; i++) {
            emu_ram[I + i] = V[i];
          }

          // Correct for old method (doesn't actually run old method, just updates I like it did)
          if (LOAD_STORE_MODE == 0) {
            I = I + X + 1;
          }
          break;
        }
        case 0x65:
        {
          // Load function
          // New method (temp variable)
          uint8_t index = 0;
          for (uint8_t i = 0; i <= X; i++) {
            V[i] = emu_ram[I + i] ;
          }

          // Correct for old method (doesn't actually run old method, just updates I like it did)
          if (LOAD_STORE_MODE == 0) {
            I = I + X + 1;
          }
          break;
        }
      }
      break;
    }
    default:
      //APP_LOG(APP_LOG_LEVEL_ERROR, "Invalid opcode: %d", opcode);
      printf("Invalid opcode: %d\n", opcode);
  }
}

// Set a pixel in the SDL pixel array that is drawn to the screen
void set_pixel_color(uint16_t x, uint16_t y, uint32_t color) {
    int r = screen_pixels[y * SCREEN_WIDTH * 4 + x * 4 + 0] = (uint8_t)((color & 0xFF000000) >> 24); // r
    int g = screen_pixels[y * SCREEN_WIDTH * 4 + x * 4 + 1] = (uint8_t)((color & 0x00FF0000) >> 16); // g
    int b = screen_pixels[y * SCREEN_WIDTH * 4 + x * 4 + 2] = (uint8_t)((color & 0x0000FF00) >> 8); // b
    int a = screen_pixels[y * SCREEN_WIDTH * 4 + x * 4 + 3] = (uint8_t)((color & 0x000000FF) >> 0); // a
}

// update the contents of the SDL pixel array using the contents of the emulated VRAM
void update_screen_pixels() {
    uint16_t x = 0;
    uint16_t y = 0;

    for (int vram_offset = 0; vram_offset < VRAM_SIZE; vram_offset++) {
        // if on a new line on the screen (the second condition makes sure it's not the first line)
        if (vram_offset * (SCREEN_WIDTH/8) % SCREEN_WIDTH == 0 && vram_offset * (SCREEN_WIDTH/8) / SCREEN_WIDTH > 0) {
            y += 1;
            x = 0;
        }

        for (int bit_offset = 0; bit_offset < 8; bit_offset++) {
            uint32_t pixel_color;

            if(emu_ram[VRAM_START_BYTE + vram_offset] >> (7 - bit_offset) & 0b00000001) {
                pixel_color = palette[1];
            }
            else {
                pixel_color = palette[0];
            }
            set_pixel_color(x, y, pixel_color);
            x++;
        }
    }
}

void update_screen_texture() {
    // update texture with new data
    int texture_pitch = 0;
    void* texture_pixels = NULL;
    if (SDL_LockTexture(screen_tex, NULL, &texture_pixels, &texture_pitch) != 0) {
        SDL_Log("Unable to lock texture: %s", SDL_GetError());
    }
    else {
        memcpy(texture_pixels, screen_pixels, texture_pitch * SCREEN_HEIGHT);
    }
    SDL_UnlockTexture(screen_tex);
}

// Draws the SDL pixel array to the screen
void draw_frame() {
    update_screen_pixels(screen_pixels);

    update_screen_texture(screen_tex, screen_pixels);

    SDL_RenderClear(screen_ren);
    SDL_RenderCopy(screen_ren, screen_tex, NULL, NULL);
    SDL_RenderPresent(screen_ren);
}

int timespec_subtract (struct timespec *result, struct timespec *x, struct timespec *y)
{
  struct timespec x_temp;
  x_temp.tv_sec = x->tv_sec;
  x_temp.tv_nsec = x->tv_nsec;
  struct timespec y_temp;
  y_temp.tv_sec = y->tv_sec;
  y_temp.tv_nsec = y->tv_nsec;

  struct timespec *x_temp_ptr = &x_temp;
  struct timespec *y_temp_ptr = &y_temp;

  /* Perform the carry for the later subtraction by updating y. */
  if (x_temp_ptr->tv_nsec < y_temp_ptr->tv_nsec) {
    long nsec = (y_temp_ptr->tv_nsec - x_temp_ptr->tv_nsec) / 1000000000 + 1;
    y_temp_ptr->tv_nsec -= 1000000000 * nsec;
    y_temp_ptr->tv_sec += nsec;
  }
  if (x_temp_ptr->tv_nsec - y_temp_ptr->tv_nsec > 1000000000) {
    int nsec = (x_temp_ptr->tv_nsec - y_temp_ptr->tv_nsec) / 1000000000;
    y_temp_ptr->tv_nsec += 1000000000 * nsec;
    y_temp_ptr->tv_sec -= nsec;
  }

  /* Compute the time remaining to wait.
     tv_usec is certainly positive. */
  result->tv_sec = x_temp_ptr->tv_sec - y_temp_ptr->tv_sec;
  result->tv_nsec = x_temp_ptr->tv_nsec - y_temp_ptr->tv_nsec;

  /* Return 1 if result is negative. */
  return x_temp_ptr->tv_sec < y_temp_ptr->tv_sec;
}


#ifdef _WIN32
int get_clock_time( struct timespec *tv)
{
    time_t rawtime;
 
    time(&rawtime);
    tv->tv_sec = (long)rawtime;
 
    // here starts the microsecond resolution:
 
    LARGE_INTEGER tickPerSecond;
    LARGE_INTEGER tick; // a point in time
 
    // get the high resolution counter's accuracy
    QueryPerformanceFrequency(&tickPerSecond);
 
    // what time is it ?
    QueryPerformanceCounter(&tick);

    //printf("Tick: %ld\n", tick);
 
    // and here we get the current microsecond! \o/
    tv->tv_nsec = (tick.QuadPart % tickPerSecond.QuadPart * 100);
 
    return 0;
}
#else
void get_clock_time(struct timespec *ts) {
    clockid_t realtime_clock = CLOCK_REALTIME;

    if (clock_gettime(realtime_clock, ts) == -1) {
            printf("Time get error\n");
            perror("clock_gettime");
            exit(EXIT_FAILURE);
    }
}
#endif // _WIN32

// int gettimeofday(struct timespec * tp)
// {
//     // Note: some broken versions only have 8 trailing zero's, the correct epoch has 9 trailing zero's
//     // This magic number is the number of 100 nanosecond intervals since January 1, 1601 (UTC)
//     // until 00:00:00 January 1, 1970 
//     static const uint64_t EPOCH = ((uint64_t) 116444736000000000ULL);

//     SYSTEMTIME  system_time;
//     FILETIME    file_time;
//     uint64_t    time;

//     GetSystemTime( &system_time );
//     SystemTimeToFileTime( &system_time, &file_time );
//     time =  ((uint64_t)file_time.dwLowDateTime )      ;
//     time += ((uint64_t)file_time.dwHighDateTime) << 32;

//     tp->tv_sec  = (long) ((time - EPOCH) / 10000000L);
//     tp->tv_nsec = (long) (system_time.wMilliseconds * 1000);
//     return 0;
// }

void parse_args(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Please specify a rom file\n");
        exit(1);
    }
    else {
        rom_path = malloc(sizeof(char) * (strlen(argv[1]) + 1));
        strcpy(rom_path, argv[1]);
        printf("%s\n", rom_path);
    }
}
 
int main(int argc, char *argv[])
{
    initialize_settings();

    parse_args(argc, argv);

	if (SDL_Init(SDL_INIT_EVERYTHING) != 0) {
		fprintf(stderr, "SDL_Init Error: %s\n", SDL_GetError());
		return EXIT_FAILURE;
	}

	SDL_Window* win = SDL_CreateWindow("chip8", 100, 300, 640, 320, SDL_WINDOW_SHOWN);
	if (win == NULL) {
		fprintf(stderr, "SDL_CreateWindow Error: %s\n", SDL_GetError());
		return EXIT_FAILURE;
	}

	screen_ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (screen_ren == NULL) {
		fprintf(stderr, "SDL_CreateRenderer Error: %s\n", SDL_GetError());
		SDL_DestroyWindow(win);
		SDL_Quit();
		return EXIT_FAILURE;
	}


    // create texture
    screen_tex = SDL_CreateTexture(
        screen_ren,
        SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STREAMING,
        SCREEN_WIDTH,
        SCREEN_HEIGHT);
    if (screen_tex == NULL) {
        SDL_Log("Unable to create texture: %s", SDL_GetError());
        return 1;
    }

    // array of pixels of the screen
    int pixel_array_size = SCREEN_WIDTH * SCREEN_HEIGHT * 4 * sizeof(uint8_t);
    screen_pixels = malloc(pixel_array_size);
    for (int y = 0; y < (SCREEN_HEIGHT); y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            for(int rgba = 0; rgba < 4; rgba++) {
                screen_pixels[(y * SCREEN_WIDTH * 4) + (x * 4) + rgba] = 0;
                //count++;
            }
        }
    }

    initialize_emu_ram();

    load_rom(rom_path);
    

    //Main loop flag
    bool quit = false;

    //Event handler
    SDL_Event e;


    struct timespec last_instruction;
    get_clock_time(&last_instruction);
    struct timespec current_instruction;


    struct timespec ips_counter_last;
    get_clock_time(&ips_counter_last);
    struct timespec ips_counter_current;

    struct timespec timer_last;
    get_clock_time(&timer_last);
    struct timespec timer_current;

    struct timespec frame_last;
    get_clock_time(&frame_last);
    struct timespec frame_current;


    int ips_count = 0;
    int timer_count = 0;
    int frame_count = 0;


    draw_frame();

    previous_keyboard = (uint8_t*) SDL_GetKeyboardState(NULL);


	while(!quit) {
        while (SDL_PollEvent( &e ) != 0) {
            switch(e.type) {
                case SDL_QUIT:
                    quit = true;
                    break;
                case SDL_KEYDOWN:
                    ;
                    int temp_key = -1;
                    // key down checks if key is already down because of keyspam
                    if (e.key.keysym.scancode == SDL_SCANCODE_1 && keypad_states[0x1] == 0) { // 1
                        keypad_states[0x1] = 1;
                        temp_key = 0x1;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_2 && keypad_states[0x2] == 0) { // 2
                        keypad_states[0x2] = 1;
                        temp_key = 0x2;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_3 && keypad_states[0x3] == 0) { // 3
                        keypad_states[0x3] = 1;
                        temp_key = 0x3;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_4 && keypad_states[0xC] == 0) { // C
                        keypad_states[0xC] = 1;
                        temp_key = 0xC;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_Q && keypad_states[0x4] == 0) { // 4
                        keypad_states[0x4] = 1;
                        temp_key = 0x4;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_W && keypad_states[0x5] == 0) { // 5
                        keypad_states[0x5] = 1;
                        temp_key = 0x5;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_E && keypad_states[0x6] == 0) { // 6
                        keypad_states[0x6] = 1;
                        temp_key = 0x6;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_R && keypad_states[0xD] == 0) { // D
                        keypad_states[0xD] = 1;
                        temp_key = 0xD;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_A && keypad_states[0x7] == 0) { // 7
                        keypad_states[0x7] = 1;
                        temp_key = 0x7;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_S && keypad_states[0x8] == 0) { // 8
                        keypad_states[0x8] = 1;
                        temp_key = 0x8;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_D && keypad_states[0x9] == 0) { // 9
                        keypad_states[0x9] = 1;
                        temp_key = 0x9;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_F && keypad_states[0xE] == 0) { // E
                        keypad_states[0xE] = 1;
                        temp_key = 0xE;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_Z && keypad_states[0xA] == 0) { // A
                        keypad_states[0xA] = 1;
                        temp_key = 0xA;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_X && keypad_states[0x0] == 0) { // 0
                        keypad_states[0x0] = 1;
                        temp_key = 0x0;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_C && keypad_states[0xB] == 0) { // B
                        keypad_states[0xB] = 1;
                        temp_key = 0xB;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_V && keypad_states[0xF] == 0) { // F
                        keypad_states[0xF] = 1;
                        temp_key = 0xF;
                    }

                    // set key for getkey instruction
                    if (get_key_status == 1 && temp_key >= 0) {
                        get_key_key = temp_key;
                        get_key_status = 2;
                    }
                    break;
                case SDL_KEYUP:
                    // keyup does not check if key is down becuase keyup only ever fires once
                    if (e.key.keysym.scancode == SDL_SCANCODE_1) {  // 1
                        keypad_states[0x1] = 0;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_2) {  // 2
                        keypad_states[0x2] = 0;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_3) { // 3
                        keypad_states[0x3] = 0;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_4) { // C
                        keypad_states[0xC] = 0;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_Q) { // 4
                        keypad_states[0x4] = 0;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_W) { // 5
                        keypad_states[0x5] = 0;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_E) { // 6
                        keypad_states[0x6] = 0;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_R) { // D
                        keypad_states[0xD] = 0;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_A) { // 7
                        keypad_states[0x7] = 0;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_S) { // 8
                        keypad_states[0x8] = 0;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_D) { // 9
                        keypad_states[0x9] = 0;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_F) { // E
                        keypad_states[0xE] = 0;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_Z) { // A
                        keypad_states[0xA] = 0;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_X) { // 0
                        keypad_states[0x0] = 0;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_C) { // B
                        keypad_states[0xB] = 0;
                    }
                    else if (e.key.keysym.scancode == SDL_SCANCODE_V) { // F
                        keypad_states[0xF] = 0;
                    }
                    break;
                
            }  
        }

        current_keyboard = (uint8_t*) SDL_GetKeyboardState(NULL);

        // process changes
        if (current_keyboard[SDL_SCANCODE_D] && !current_keyboard[SDL_SCANCODE_D]) {
            printf("D pressed\n");
        }

        previous_keyboard = current_keyboard;

        get_clock_time(&current_instruction);
        struct timespec delta_time;
        timespec_subtract(&delta_time, &current_instruction, &last_instruction);

        // test timespec
        struct timespec test_time_end;
        struct timespec test_time_start;
        test_time_end.tv_sec = 1676173113;
        test_time_end.tv_nsec = 572109300;
        test_time_start.tv_sec = 1676173112;
        test_time_start.tv_nsec = 934113200;
        struct timespec test_delta_time;
        timespec_subtract(&test_delta_time, &test_time_end, & test_time_start);


        // still only millisecond precision, so IPS can only be 500 or 1000
        if(delta_time.tv_nsec >= ((long)1000000000 / IPS) || (long)delta_time.tv_sec >= 1) {
            run_next_instruction();
            get_clock_time(&last_instruction);
            ips_count++;
        }

        get_clock_time(&ips_counter_current);
        struct timespec delta_time_ips;
        timespec_subtract(&delta_time_ips, &ips_counter_current, &ips_counter_last);


        if ((long)delta_time_ips.tv_sec >= 1) {
            printf("IPS: %d\n", ips_count);

            get_clock_time(&ips_counter_last);
            ips_count = 0;

            printf("Timer: %d\n", timer_count);
            timer_count = 0;

            printf("FPS: %d\n", frame_count);
            frame_count = 0;
        }


        get_clock_time(&timer_current);
        struct timespec delta_time_timer;
        timespec_subtract(&delta_time_timer, &timer_current, &timer_last);


        if(delta_time_timer.tv_nsec >= ((long)1000000000 / TIMER_FREQUENCY) || (long)delta_time_timer.tv_sec >= 1) {
            update_timers();
            get_clock_time(&timer_last);
            timer_count++;
        }



        if (UNHOOK_FPS == 1) {
            get_clock_time(&frame_current);
            struct timespec delta_time_frame;
            timespec_subtract(&delta_time_frame, &frame_current, &frame_last);

            
            if(delta_time_frame.tv_nsec >= ((long)1000000000 / FPS) || (long)delta_time_frame.tv_sec >= 1) {
                draw_frame();
                get_clock_time(&frame_last);
                frame_count++;
            }
        }
	}

	SDL_DestroyTexture(screen_tex);
	SDL_DestroyRenderer(screen_ren);
	SDL_DestroyWindow(win);
	SDL_Quit();

	return EXIT_SUCCESS;
}