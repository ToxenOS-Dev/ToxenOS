#ifndef KEYBOARD_H
#define KEYBOARD_H
void keyboard_init();
char keyboard_getchar();
int  keyboard_available();
void keyboard_set_sigint_target(int pid);
void keyboard_inject(char c);   // inject a character from USB/other source
#endif
