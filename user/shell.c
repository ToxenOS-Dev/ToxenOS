// ToxenOS shell - runs in ring 3
#include <stdint.h>


void print(const char* msg)
{
    __asm__ volatile(
        "movl $1, %%eax\n"
        "movl %0, %%ebx\n"
        "int $0x80\n"
        :
        : "r"(msg)
        : "eax", "ebx"
    );
}

char getchar()
{
    int c;
    __asm__ volatile(
        "movl $2, %%eax\n"
        "int $0x80\n"
        "movl %%eax, %0\n"
        : "=r"(c)
        :
        : "eax"
    );
    return (char)c;
}

void exit()
{
    __asm__ volatile(
        "movl $0, %%eax\n"
        "int $0x80\n"
        ::: "eax"
    );
}

void yield()
{
    __asm__ volatile("int $0x80" :: "a"(4));  // new syscall: SYS_YIELD
}

void erase()
{
    __asm__ volatile(
        "movl $5, %%eax\n"
        "int $0x80\n"
        ::: "eax"
    );
}

void set_color(uint8_t color)
{
    __asm__ volatile(
        "movl $6, %%eax\n"
        "movl %0, %%ebx\n"
        "int $0x80\n"
        :
        : "r"((uint32_t)color)
        : "eax", "ebx"
    );
}

void _start()
{
    set_color(0x06);   // brown
    print("Tox");
    set_color(0x07);   // light grey
    print("> ");

    while (1)
    {
        char c = getchar();
        if (c == 0) continue;

        if (c == '\n')
        {
            print("\n");
            print("Tox> ");
        }
        else if (c == 8)  // backspace
        {
            erase();
        }
        else
        {
            char buf[2] = {c, 0};
            print(buf);
        }
    }
}