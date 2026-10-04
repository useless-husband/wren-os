#ifndef KERNEL_CONSOLE_H
#define KERNEL_CONSOLE_H
#include "types.h"

void console_init(void);
void console_input(char c);   /* called from the UART interrupt */

#endif
