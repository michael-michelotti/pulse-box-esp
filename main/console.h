#ifndef CONSOLE_H
#define CONSOLE_H

#include <stddef.h>

/**
 * Process a text command and write the response into a buffer.
 * Shared by UART console and TCP command server.
 *
 * @param cmd_line    Null-terminated command string (e.g. "effect rainbow")
 * @param resp        Buffer to write response text into
 * @param resp_size   Size of the response buffer
 */
void process_user_command(const char *cmd_line, char *resp, size_t resp_size);

void console_init(void);
void console_task(void *pvParameters);

#endif
