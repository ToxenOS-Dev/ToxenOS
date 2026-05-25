#include <stdint.h>
#include "../include/keyboard.h"
#include "../include/irq.h"
#include "../include/pic.h"
#include "../include/process.h"

static inline uint8_t inb(uint16_t port) {
    uint8_t r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(port)); return r;
}

static char scancode_table[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=',8,
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',
    'z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',
};

static char shift_table[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+',8,
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
    'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
    'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' ',
};

#define BUFFER_SIZE 64
static char buffer[BUFFER_SIZE];
static int  buf_head = 0, buf_tail = 0;
static int  ctrl_pressed = 0, shift_pressed = 0, extended = 0;

// pid of the process that should receive Ctrl+C (-1 = none / kill foreground)
static int sigint_target = -1;

void keyboard_set_sigint_target(int pid)
{
    sigint_target = pid;
}

static void keyboard_handler() {
    uint8_t sc = inb(0x60);
    if (sc == 0x1D) { ctrl_pressed  = 1; return; }
    if (sc == 0x9D) { ctrl_pressed  = 0; return; }
    if (sc == 0x2A || sc == 0x36) { shift_pressed = 1; return; }
    if (sc == 0xAA || sc == 0xB6) { shift_pressed = 0; return; }
    if (sc == 0xE0) { extended = 1; return; }
    if (extended) {
        extended = 0;
        char c = 0;
        if (sc == 0x48) c = 0x01;  // up
        if (sc == 0x50) c = 0x02;  // down
        if (sc == 0x4B) c = 0x03;  // left — NOTE: not Ctrl+C, this is arrow
        if (sc == 0x4D) c = 0x04;  // right
        if (c) {
            int next = (buf_tail+1)%BUFFER_SIZE;
            if (next != buf_head) { buffer[buf_tail]=c; buf_tail=next; }
        }
        return;
    }
    if (sc & 0x80) return;
    char c = shift_pressed ? shift_table[sc] : scancode_table[sc];
    if (c == 0) return;
    if (ctrl_pressed) {
        if (c >= 'a' && c <= 'z') c = c-'a'+1;
        else if (c >= 'A' && c <= 'Z') c = c-'A'+1;
        else return;
        // Ctrl+C (0x03): kill the foreground child process
        if (c == 0x03) {
            int target = sigint_target;
            if (target > 0 && target < MAX_PROCESSES &&
                processes[target].state != PROCESS_DEAD)
            {
                processes[target].state = PROCESS_DEAD;
                // Wake any process waiting on the killed process
                for (int i = 0; i < MAX_PROCESSES; i++) {
                    if (processes[i].state == PROCESS_WAITING &&
                        processes[i].waiting_for == target) {
                        processes[i].state       = PROCESS_READY;
                        processes[i].waiting_for = -1;
                    }
                }
            }
            sigint_target = -1;
            return;  // don't put ^C in the key buffer
        }
    }
    int next = (buf_tail+1)%BUFFER_SIZE;
    if (next != buf_head) { buffer[buf_tail]=c; buf_tail=next; }
}

void keyboard_init() { irq_register(1, keyboard_handler); }

void keyboard_inject(char c) {
    int next = (buf_tail+1)%BUFFER_SIZE;
    if (next != buf_head) { buffer[buf_tail]=c; buf_tail=next; }
}

char keyboard_getchar() {
    while (buf_head == buf_tail) __asm__ volatile("hlt");
    char c = buffer[buf_head];
    buf_head = (buf_head+1)%BUFFER_SIZE;
    return c;
}

int keyboard_available() { return buf_head != buf_tail; }
