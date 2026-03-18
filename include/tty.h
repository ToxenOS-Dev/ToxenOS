#ifndef TTY_H
#define TTY_H

void tty_init();
void tty_switch(int tty);
void tty_draw_indicator();
int  tty_current();
void tty_set_pid(int tty, int pid);
int  tty_get_pid(int tty);
extern int tty_for_pid[];
void tty_assign_pid(int pid, int tty);
int  tty_of_pid(int pid);


#endif