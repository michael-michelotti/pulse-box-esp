#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "driver/uart.h"
#include "effects.h"
#include "led_math.h"

#define CONSOLE_UART_NUM    UART_NUM_0
#define CONSOLE_BUF_SIZE    256

/* Global handles to control which effect, effect parameters, and mapping are currently active */
extern EffectParams_t effect_params;
extern const Effect_t *current_effect;
extern const Mapping_t *current_mapping;

static char cmd_buffer[CONSOLE_BUF_SIZE];
static uint16_t rx_index;
static uint8_t welcomed;
static uint8_t newline_flag;

static const char welcome_message[] =
        "Welcome to the PulseBox LED control CLI.\r\n"
        "Type 'help' for a menu.\r\n> ";

static const char help_text[] =
        "Command Menu:\r\n"
        "  effect <rainbow|bass|twinkle|solid|splash|fire|breathe|wipe|spectrum>\r\n"
        "  color <r> <g> <b>     - set color (integer 0-255)\r\n"
        "  palette <rainbow|fire> - set palette\r\n"
        "  speed <float>          - set speed (decimal 0-1)\r\n"
        "  brightness <%>         - global brightness 0-100%\r\n"
        "  direction <degrees>    - set direction (0-360)\r\n"
        "  status                 - show current settings\r\n"
        "  help                   - show this menu\r\n";

static void console_print(const char *msg, uint16_t len)
{
    uart_write_bytes(CONSOLE_UART_NUM, msg, len);
}

static void process_user_command(void)
{
    char *cmd = strtok(cmd_buffer, " ");
    if (cmd == NULL) return;
    char *arg = strtok(NULL, " ");

    /*** EFFECT CONTROLS ***/
    if (strcmp(cmd, "effect") == 0 && arg != NULL) {
        if (strcmp(arg, "rainbow") == 0) {
            current_effect = &rainbow_effect;
        }
        else if (strcmp(arg, "bass") == 0) {
            current_effect = &bass_pulse_effect;
        }
        else if (strcmp(arg, "twinkle") == 0) {
            current_effect = &twinkle_effect;
        }
        else if (strcmp(arg, "solid") == 0) {
            current_effect = &solid_effect;
        }
        else if (strcmp(arg, "splash") == 0) {
            current_effect = &bass_splash_effect;
        }
        else if (strcmp(arg, "fire") == 0) {
            current_effect = &fire_effect;
        }
        else if (strcmp(arg, "breathe") == 0) {
            current_effect = &breathe_effect;
        }
        else if (strcmp(arg, "wipe") == 0) {
            current_effect = &wipe_effect;
        }
        else if (strcmp(arg, "spectrum") == 0) {
            current_effect = &spectrum_effect;
        }
        else {
            char msg[64];
            snprintf(msg, sizeof(msg), "unknown effect '%s'\r\n", arg);
            console_print(msg, strlen(msg));
            return;
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "changed effect to %s\r\n", current_effect->name);
        console_print(msg, strlen(msg));
    }

    /*** HELP MENU ***/
    else if (strcmp(cmd, "help") == 0) {
        console_print(help_text, strlen(help_text));
    }

    /*** CURRENT STATUS PRINTOUT ***/
    else if (strcmp(cmd, "status") == 0) {
        char status[128];
        snprintf(status, sizeof(status),
                "Current Status:\r\n"
                "  effect: %s\r\n"
                "  color: r: %d, g: %d, b: %d\r\n"
                "  speed: %.2f/1.00\r\n"
                "  brightness: %u%%\r\n",
                current_effect->name,
                effect_params.color_set->colors[0].r,
                effect_params.color_set->colors[0].g,
                effect_params.color_set->colors[0].b,
                effect_params.speed,
                effect_params.brightness);
        console_print(status, strlen(status));
    }

    /*** EFFECT COLOR CONTROL ***/
    else if (strcmp(cmd, "color") == 0 && arg != NULL) {
        effect_params.color_set->colors[0].r = atoi(arg);
        arg = strtok(NULL, " ");
        if (arg) effect_params.color_set->colors[0].g = atoi(arg);
        arg = strtok(NULL, " ");
        if (arg) effect_params.color_set->colors[0].b = atoi(arg);
    }

    /*** EFFECT SPEED CONTROL ***/
    else if (strcmp(cmd, "speed") == 0 && arg != NULL) {
        char msg[64];
        float speed = atof(arg);
        if (speed < 0 || speed > 1) {
            snprintf(msg, sizeof(msg), "invalid speed '%.2f'\r\n", speed);
            console_print(msg, strlen(msg));
            return;
        }
        effect_params.speed = speed;
        snprintf(msg, sizeof(msg), "changed speed to %.2f\r\n", effect_params.speed);
        console_print(msg, strlen(msg));
    }

    /*** GLOBAL BRIGHTNESS CONTROL ***/
    else if (strcmp(cmd, "brightness") == 0 && arg != NULL) {
        char msg[64];
        uint8_t brightness = atoi(arg);
        if (brightness > 100) {
            snprintf(msg, sizeof(msg), "invalid brightness '%u'\r\n", brightness);
            console_print(msg, strlen(msg));
            return;
        }
        effect_params.brightness = brightness;
        snprintf(msg, sizeof(msg), "changed brightness to %u%%\r\n", effect_params.brightness);
        console_print(msg, strlen(msg));
    }

    /*** EFFECT DIRECTION CONTROL ***/
    else if (strcmp(cmd, "direction") == 0 && arg != NULL) {
        char msg[64];
        float dir = atof(arg);
        if (dir < 0 || dir > 360) {
            snprintf(msg, sizeof(msg), "invalid direction '%f'\r\n", dir);
            console_print(msg, strlen(msg));
            return;
        }
        effect_params.direction = dir;
        snprintf(msg, sizeof(msg), "changed direction to %f degrees\r\n", effect_params.direction);
        console_print(msg, strlen(msg));
    }

    /*** PALETTE CONTROL ***/
    else if (strcmp(cmd, "palette") == 0 && arg != NULL) {
        if (strcmp(arg, "rainbow") == 0) {
            effect_params.palette = &rainbow_palette;
        }
        else if (strcmp(arg, "fire") == 0) {
            effect_params.palette = &fire_palette;
        }
        else {
            char msg[64];
            snprintf(msg, sizeof(msg), "unknown palette '%s'\r\n", arg);
            console_print(msg, strlen(msg));
            return;
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "changed palette to %s\r\n", effect_params.palette->name);
        console_print(msg, strlen(msg));
    }

    /*** UNKNOWN COMMAND ***/
    else {
        char msg[64];
        snprintf(msg, sizeof(msg), "unknown command '%s'\r\n", cmd);
        console_print(msg, strlen(msg));
    }
}

void console_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_driver_install(CONSOLE_UART_NUM, CONSOLE_BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(CONSOLE_UART_NUM, &uart_config);
}

void console_process(void)
{
    uint8_t byte;
    while (uart_read_bytes(CONSOLE_UART_NUM, &byte, 1, 0) > 0) {
        if (!welcomed) {
            console_print(welcome_message, strlen(welcome_message));
            welcomed = 1;
        }

        if (byte == '\n' || byte == '\r') {
            cmd_buffer[rx_index] = '\0';
            newline_flag = 1;
            console_print("\r\n", 2);
        }
        else if (byte == '\b' || byte == 0x7F) {
            if (rx_index > 0) {
                rx_index--;
                console_print("\b \b", 3);
            }
        }
        else if (rx_index < CONSOLE_BUF_SIZE - 1) {
            cmd_buffer[rx_index++] = byte;
            console_print((char *)&byte, 1);
        }
    }

    if (newline_flag) {
        process_user_command();
        rx_index = 0;
        newline_flag = 0;
        console_print("> ", 2);
    }
}
