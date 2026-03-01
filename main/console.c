#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "console.h"
#include "effects.h"
#include "led_math.h"
#include "tcp_cmd_server.h"

/* WiFi credential storage — defined in main.c */
extern bool wifi_creds_save(const char *ssid, const char *password);
extern bool wifi_is_ap_mode;

#define CONSOLE_UART_NUM    UART_NUM_0
#define CONSOLE_BUF_SIZE    256

static const char *TAG = "console";

/* Global handles to control which effect, effect parameters, and mapping are currently active */
extern EffectParams_t effect_params;
extern const Effect_t *current_effect;
extern const Mapping_t *current_mapping;

static char cmd_buffer[CONSOLE_BUF_SIZE];
static uint16_t rx_index;

static const char welcome_message[] =
        "Welcome to the PulseBox LED control CLI.\r\n"
        "Type 'help' for a menu.\r\n> ";

static const char help_text[] =
        "Command Menu:\r\n"
        "  effect <rainbow|bass|twinkle|solid|splash|fire|breathe|wipe|spectrum|image|gif|tunnel>\r\n"
        "  color <r> <g> <b>           - set primary color (0-255)\r\n"
        "  color <index> <r> <g> <b>   - set color by index (0-2)\r\n"
        "  palette <rainbow|fire> - set palette\r\n"
        "  speed <float>          - set speed (decimal 0-1)\r\n"
        "  sensitivity <0-100>    - set audio sensitivity\r\n"
        "  brightness <%>         - global brightness 0-100%\r\n"
        "  direction <degrees>    - set direction (0-360)\r\n"
        "  wifi <ssid> <password>  - set WiFi credentials and reboot\r\n"
        "  status                 - show current settings\r\n"
        "  help                   - show this menu\r\n";

static void console_print(const char *msg, uint16_t len)
{
    uart_write_bytes(CONSOLE_UART_NUM, msg, len);
}

bool process_user_command(const char *cmd_line, char *resp, size_t resp_size)
{
    /* Work on a mutable copy since strtok modifies the string */
    char buf[CONSOLE_BUF_SIZE];
    strncpy(buf, cmd_line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    resp[0] = '\0';
    bool state_changed = false;

    char *cmd = strtok(buf, " ");
    if (cmd == NULL) return true;
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
        else if (strcmp(arg, "image") == 0) {
            current_effect = &image_effect;
        }
        else if (strcmp(arg, "gif") == 0) {
            current_effect = &gif_effect;
        }
        else if (strcmp(arg, "tunnel") == 0) {
            current_effect = &tunnel_effect;
        }
        else {
            snprintf(resp, resp_size, "unknown effect '%s'\r\n", arg);
            return false;
        }
        snprintf(resp, resp_size, "changed effect to %s\r\n", current_effect->name);
        state_changed = true;
    }

    /*** HELP MENU ***/
    else if (strcmp(cmd, "help") == 0) {
        snprintf(resp, resp_size, "%s", help_text);
    }

    /*** CURRENT STATUS PRINTOUT ***/
    else if (strcmp(cmd, "status") == 0) {
        snprintf(resp, resp_size,
                "Current Status:\r\n"
                "  wifi: %s\r\n"
                "  effect: %s\r\n"
                "  color[0]: r: %d, g: %d, b: %d\r\n"
                "  color[1]: r: %d, g: %d, b: %d\r\n"
                "  color[2]: r: %d, g: %d, b: %d\r\n"
                "  speed: %.2f/1.00\r\n"
                "  sensitivity: %u\r\n"
                "  brightness: %u%%\r\n",
                wifi_is_ap_mode ? "AP (setup mode)" : "STA (connected)",
                current_effect->name,
                effect_params.color_set->colors[0].r,
                effect_params.color_set->colors[0].g,
                effect_params.color_set->colors[0].b,
                effect_params.color_set->colors[1].r,
                effect_params.color_set->colors[1].g,
                effect_params.color_set->colors[1].b,
                effect_params.color_set->colors[2].r,
                effect_params.color_set->colors[2].g,
                effect_params.color_set->colors[2].b,
                effect_params.speed,
                effect_params.sensitivity,
                effect_params.brightness);
    }

    /*** EFFECT COLOR CONTROL ***/
    else if (strcmp(cmd, "color") == 0 && arg != NULL) {
        char *arg2 = strtok(NULL, " ");
        char *arg3 = strtok(NULL, " ");
        char *arg4 = strtok(NULL, " ");

        if (arg4 != NULL) {
            /* 4-arg form: color <index> <r> <g> <b> */
            int idx = atoi(arg);
            if (idx < 0 || idx > 2) {
                snprintf(resp, resp_size, "invalid color index %d (0-2)\r\n", idx);
                return false;
            }
            WRGB_t rgb = { .r = atoi(arg2), .g = atoi(arg3), .b = atoi(arg4) };
            effect_params.color_set->colors[idx] = rgb;
            snprintf(resp, resp_size, "changed color[%d] to r: %d, g: %d, b: %d\r\n",
                    idx, rgb.r, rgb.g, rgb.b);
        } else {
            /* 3-arg form: color <r> <g> <b> (backward compatible, sets colors[0]) */
            WRGB_t rgb = { .r = atoi(arg) };
            if (arg2) rgb.g = atoi(arg2);
            if (arg3) rgb.b = atoi(arg3);
            effect_params.color_set->colors[0] = rgb;
            snprintf(resp, resp_size, "changed color to r: %d, g: %d, b: %d\r\n",
                    rgb.r, rgb.g, rgb.b);
        }
        state_changed = true;
    }

    /*** EFFECT SPEED CONTROL ***/
    else if (strcmp(cmd, "speed") == 0 && arg != NULL) {
        float speed = atof(arg);
        if (speed < 0 || speed > 1) {
            snprintf(resp, resp_size, "invalid speed '%.2f'\r\n", speed);
            return false;
        }
        effect_params.speed = speed;
        snprintf(resp, resp_size, "changed speed to %.2f\r\n", effect_params.speed);
        state_changed = true;
    }

    /*** AUDIO SENSITIVITY CONTROL ***/
    else if (strcmp(cmd, "sensitivity") == 0 && arg != NULL) {
        uint8_t sensitivity = atoi(arg);
        if (sensitivity > 100) {
            snprintf(resp, resp_size, "invalid sensitivity '%u'\r\n", sensitivity);
            return false;
        }
        effect_params.sensitivity = sensitivity;
        snprintf(resp, resp_size, "changed sensitivity to %u\r\n", effect_params.sensitivity);
        state_changed = true;
    }

    /*** GLOBAL BRIGHTNESS CONTROL ***/
    else if (strcmp(cmd, "brightness") == 0 && arg != NULL) {
        uint8_t brightness = atoi(arg);
        if (brightness > 100) {
            snprintf(resp, resp_size, "invalid brightness '%u'\r\n", brightness);
            return false;
        }
        effect_params.brightness = brightness;
        snprintf(resp, resp_size, "changed brightness to %u%%\r\n", effect_params.brightness);
        state_changed = true;
    }

    /*** EFFECT DIRECTION CONTROL ***/
    else if (strcmp(cmd, "direction") == 0 && arg != NULL) {
        float dir = atof(arg);
        if (dir < 0 || dir > 360) {
            snprintf(resp, resp_size, "invalid direction '%f'\r\n", dir);
            return false;
        }
        effect_params.direction = dir;
        snprintf(resp, resp_size, "changed direction to %f degrees\r\n", effect_params.direction);
        state_changed = true;
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
            snprintf(resp, resp_size, "unknown palette '%s'\r\n", arg);
            return false;
        }
        snprintf(resp, resp_size, "changed palette to %s\r\n", effect_params.palette->name);
        state_changed = true;
    }

    /*** WIFI CREDENTIAL CONFIGURATION ***/
    else if (strcmp(cmd, "wifi") == 0 && arg != NULL) {
        char *pass = strtok(NULL, "");  /* rest of line is password (may contain spaces) */
        if (pass == NULL) {
            snprintf(resp, resp_size, "usage: wifi <ssid> <password>\r\n");
            return false;
        }
        /* skip leading whitespace in password */
        while (*pass == ' ') pass++;
        if (*pass == '\0') {
            snprintf(resp, resp_size, "usage: wifi <ssid> <password>\r\n");
            return false;
        }
        if (wifi_creds_save(arg, pass)) {
            snprintf(resp, resp_size, "WiFi credentials saved for '%s', rebooting...\r\n", arg);
            /* Give time for the response to be sent before rebooting */
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        } else {
            snprintf(resp, resp_size, "failed to save WiFi credentials\r\n");
        }
    }

    /*** UNKNOWN COMMAND ***/
    else {
        snprintf(resp, resp_size, "unknown command '%s'\r\n", cmd);
        if (state_changed) {
            tcp_push_status();
        }
        return false;
    }

    if (state_changed) {
        tcp_push_status();
    }
    return true;
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

void console_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Console task started on core %d", xPortGetCoreID());
    console_print(welcome_message, strlen(welcome_message));

    while (1) {
        uint8_t byte;
        int len = uart_read_bytes(CONSOLE_UART_NUM, &byte, 1, pdMS_TO_TICKS(10));
        if (len <= 0) {
            continue;
        }

        if (byte == '\n' || byte == '\r') {
            cmd_buffer[rx_index] = '\0';
            console_print("\r\n", 2);
            if (rx_index > 0) {
                char resp[1024];
                process_user_command(cmd_buffer, resp, sizeof(resp));
                if (resp[0] != '\0') {
                    console_print(resp, strlen(resp));
                }
            }
            rx_index = 0;
            console_print("> ", 2);
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
}
