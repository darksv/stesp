#include <string.h>
#include <espressif/esp_common.h>
#include <esp/uart.h>
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <dhcpserver.h>
#include <httpd/httpd.h>
#include <lwip/api.h>

#define AP_SSID "MotorControl"
#define AP_PSK "12345678"

#define CHANNELS_NUM 1

struct Channel {
    /* Number of ticks left until STEP output toggle */
    uint32_t ticks;

    /* Number of remaining toggles of STEP output */
    uint32_t toggles;

    /* Ticks between STEP output toggles */
    uint16_t interval;

    /* Number of direction pin */
    uint8_t dir_pin;

    /* Number of step pin */
    uint8_t step_pin;
};

volatile struct Channel channels[CHANNELS_NUM] = {
    {
        .ticks = 0,
        .toggles = 0,
        .interval = 0,
        .dir_pin = 4,
        .step_pin = 5
    },
    // {
    //     .counter = 0,
    //     .steps = 0,
    //     .frequency = 1,
    //     .dir_pin = 4,
    //     .step_pin = 5
    // },
};

char* index_cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[])
{
    return "/index.html";
}

void websocket_task(void *pvParameter)
{
    struct tcp_pcb *pcb = (struct tcp_pcb *) pvParameter;

    for (;;) {
        if (pcb == NULL || pcb->state != ESTABLISHED) {
            printf("Connection closed, deleting task\n");
            break;
        }

        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }

    vTaskDelete(NULL);
}

/**
 * This function is called when websocket frame is received.
 *
 * Note: this function is executed on TCP thread and should return as soon
 * as possible.
 */
void websocket_cb(struct tcp_pcb *pcb, uint8_t *data, u16_t data_len, uint8_t mode)
{
    printf("[websocket_callback]:\n%.*s\n", (int) data_len, (char*) data);

    uint8_t response;

    int channel, steps, duration;
    if (sscanf(data, "%d,%d,%d", &channel, &steps, &duration) == 3) {
        if (channel >= 0 && channel < CHANNELS_NUM) {
            printf("Requested %d steps on channel #%d: duration=%dms direction=%s\n", abs(steps), channel, duration, (steps < 0) ? "left" : "right");

            const uint32_t toggles = abs(steps) * 2;
            const uint32_t interval = duration * 200 / toggles;
            const bool dir = (steps > 0);

            channels[channel].toggles = toggles;
            channels[channel].ticks = interval;
            channels[channel].interval = interval;

            /* always start from LOW */
            gpio_write(channels[channel].step_pin, 0);

            /* rotation direction */
            gpio_write(channels[channel].dir_pin, dir);

            response = 0;
        } else {
            printf("channel #%d does not exist!\n", channel);

            response = 1;
        }
    } else {
        printf("Received invalid command\n");
        response = 1;
    }

    websocket_write(pcb, &response, 1, WS_BIN_MODE);
}

/**
 * This function is called when new websocket is open and
 * creates a new websocket_task if requested URI equals '/stream'.
 */
void websocket_open_cb(struct tcp_pcb *pcb, const char *uri)
{
    printf("WS URI: %s\n", uri);
    if (strcmp(uri, "/stream") == 0) {
        printf("request for streaming\n");
        xTaskCreate(&websocket_task, "websocket_task", 256, (void *) pcb, 2, NULL);
    }
}

void httpd_task(void *pvParameters)
{
    tCGI pCGIs[] = {
        {"/", (tCGIHandler) index_cgi_handler},
    };

    /* register handlers and start the server */
    http_set_cgi_handlers(pCGIs, sizeof (pCGIs) / sizeof (pCGIs[0]));
    websocket_register_callbacks((tWsOpenHandler) websocket_open_cb,
            (tWsHandler) websocket_cb);
    httpd_init();

    for (;;);
}

void frc1_interrupt_handler(void)
{
    for (uint8_t i = 0; i < CHANNELS_NUM; ++i) {
        /* check whether channel is ready to have toggled output */
        if (channels[i].ticks == 0 && channels[i].toggles > 0) {
          channels[i].ticks = channels[i].interval;
          channels[i].toggles--;

          gpio_toggle(channels[i].step_pin);
        }

        channels[i].ticks--;
    }
}

void setup_timer(uint32_t freq)
{
  /* stop both timers and mask their interrupts as a precaution */
  timer_set_interrupts(FRC1, false);
  timer_set_run(FRC1, false);

  /* set up ISRs */
  _xt_isr_attach(INUM_TIMER_FRC1, frc1_interrupt_handler);

  /* configure timer frequencies */
  timer_set_frequency(FRC1, freq);

  /* unmask interrupts and start timers */
  timer_set_interrupts(FRC1, true);
  timer_set_run(FRC1, true);
}

void user_init(void)
{
    uart_set_baud(0, 115200);
    printf("SDK version:%s\n", sdk_system_get_sdk_version());

    sdk_wifi_set_opmode(SOFTAP_MODE);
    struct ip_info ap_ip;
    IP4_ADDR(&ap_ip.ip, 10, 10, 10, 10);
    IP4_ADDR(&ap_ip.gw, 0, 0, 0, 0);
    IP4_ADDR(&ap_ip.netmask, 255, 0, 0, 0);
    sdk_wifi_set_ip_info(1, &ap_ip);

    struct sdk_softap_config ap_config = {
        .ssid = AP_SSID,
        .ssid_hidden = 0,
        .channel = 3,
        .ssid_len = strlen(AP_SSID),
        .authmode = AUTH_WPA_WPA2_PSK,
        .password = AP_PSK,
        .max_connection = 3,
        .beacon_interval = 100,
    };
    sdk_wifi_softap_set_config(&ap_config);

    ip_addr_t first_client_ip;
    IP4_ADDR(&first_client_ip, 10, 0, 0, 1);
    dhcpserver_start(&first_client_ip, 4);

    xTaskCreate(&httpd_task, "HTTP Daemon", 128, NULL, 2, NULL);

    setup_timer(200000);

    /* initialize outputs for channels */
    for (uint8_t i = 0; i < CHANNELS_NUM; ++i) {
        gpio_enable(channels[i].dir_pin, GPIO_OUTPUT);
        gpio_write(channels[i].dir_pin, 0);

        gpio_enable(channels[i].step_pin, GPIO_OUTPUT);
        gpio_write(channels[i].step_pin, 0);
    }
}
