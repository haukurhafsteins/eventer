#include <stdio.h>
#include <list>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "eventer.h"

#define POST_WAIT_MS 0

static char TAG[] = "EVENTER";
class EvLoopEvent
{
public:
    esp_event_loop_handle_t loop_handle;
    esp_event_base_t loop_base;
    int64_t period_us; // TODO - make this double to allow for sub-millisecond periods
    int64_t next_timeout_us;
    int64_t start_time_us;
    int period_counter;
    bool periodic;
    int id;
    void *data;
    size_t data_size;
};

typedef enum
{
    CMD_EXIT,
    CMD_REMOVE,
    CMD_ADD,
    CMD_NEW_PERIOD
} queue_cmd_t;

typedef struct
{
    queue_cmd_t cmd;
    EvLoopEvent *evp;
    union
    {
        int new_period_ms;
    };
} queue_msg_t;

static std::list<EvLoopEvent *> eventList = {};
static QueueHandle_t xEventQueue = NULL;

static bool compare(const EvLoopEvent *first, const EvLoopEvent *second)
{
    if (first->next_timeout_us <= second->next_timeout_us)
        return true;
    return false;
}
static void calculate_next_timeout(EvLoopEvent *ev)
{
    ev->period_counter++;
    ev->next_timeout_us = ev->start_time_us + ev->period_us * ev->period_counter;
}

static void setup_period(EvLoopEvent *evp, int ms)
{
    evp->period_us = ms * 1000;
    evp->start_time_us = esp_timer_get_time();
    evp->period_counter = 0;
}

bool evloop_post(esp_event_loop_handle_t loop_handle, esp_event_base_t loop_base, int32_t id, void *data, size_t data_size)
{
    if (loop_handle == NULL)
    {
        esp_err_t err = esp_event_post(loop_base, id, data, data_size, pdMS_TO_TICKS(POST_WAIT_MS));
        if (ESP_OK != err)
            return false;
    }
    else
    {
        esp_err_t err = esp_event_post_to(loop_handle, loop_base, id, data, data_size, pdMS_TO_TICKS(POST_WAIT_MS));
        if (ESP_OK != err)
            return false;
    }
    return true;
}
static void event_task(void *)
{
    queue_msg_t msg;
    TickType_t ticks;
    while (1)
    {
        if (eventList.empty())
        {
            ticks = 1000 / portTICK_PERIOD_MS;
            // ESP_LOGI(TAG, "sleep_time %d ms - no events", 1000);
        }
        else
        {
            auto ev = eventList.front();
            int64_t sleep_time_us = ev->next_timeout_us - esp_timer_get_time();
            ticks = (sleep_time_us / 1000) / portTICK_PERIOD_MS;
            // ESP_LOGI(TAG, "sleep_time %lld us", sleep_time_us);
        }

        if (xQueueReceive(xEventQueue, &msg, ticks) == pdTRUE)
        {
            switch (msg.cmd)
            {
            case CMD_REMOVE:
                // ESP_LOGW(TAG, "Removing %s", msg.evp->loop_base);
                eventList.remove(msg.evp);
                break;
            case CMD_ADD:
                // ESP_LOGI(TAG, "%s: Adding %lld us (periodic=%d) timer for %s", __func__, msg.evp->period_us, msg.evp->periodic, msg.evp->loop_base);
                eventList.push_back(msg.evp);
                break;
            case CMD_NEW_PERIOD:
                ESP_LOGI(TAG, "%s: New period %d us (%f Hz) for %s", __func__, msg.new_period_ms, (float)(1000.0/msg.new_period_ms), msg.evp->loop_base);
                setup_period(msg.evp, msg.new_period_ms);
                calculate_next_timeout(msg.evp);
                break;
            case CMD_EXIT:
                vQueueDelete(xEventQueue);
                xEventQueue = NULL;
                vTaskDelete(NULL);
                return;
            }
        }
        else
        {
            // Timeout, raise event
            if (eventList.empty())
                continue;

            auto ev = eventList.front();
            // ESP_LOGW(TAG, "Posting %lld us timer to %s", ev->period_us, ev->loop_base);
            if (!evloop_post(ev->loop_handle, ev->loop_base, ev->id, ev->data, ev->data_size))
                ESP_LOGE(TAG, "eventer: Failed posting event %d to %s", ev->id, ev->loop_base);
            if (ev->periodic)
                calculate_next_timeout(ev);
            else
            {
                eventList.remove(ev);
                delete ev;
            }
        }
        eventList.sort(compare);
    }
}

eventer_t eventer_add(esp_event_loop_handle_t loop_handle, esp_event_base_t loop_base, int ms, bool periodic, int id, void *data, size_t data_size)
{
    if (ms < 1)
    {
        ESP_LOGE(TAG, "%s: Invalid period, must be 1 or bigger", __func__);
        return NULL;
    }
    auto evp = new EvLoopEvent();
    evp->loop_handle = loop_handle;
    evp->loop_base = loop_base;
    evp->id = id;
    evp->periodic = periodic;
    evp->data = data;
    evp->data_size = data_size;

    setup_period(evp, ms);
    calculate_next_timeout(evp);

    queue_msg_t msg = {CMD_ADD, evp, 0};
    xQueueSend(xEventQueue, &msg, 0);
    return (eventer_t)evp;
}

void eventer_remove(eventer_t ev)
{
    if (ev == NULL || xEventQueue == NULL)
        return;

    auto evp = (EvLoopEvent *)ev;
    queue_msg_t msg = {.cmd = CMD_REMOVE, .evp = evp, .new_period_ms = 0};
    xQueueSend(xEventQueue, &msg, 0);
}

bool eventer_set_period(eventer_t ev, int ms)
{
    if (ev == NULL || xEventQueue == NULL)
        return false;

    auto evp = (EvLoopEvent *)ev;
    if (ms < 1)
    {
        ESP_LOGE(TAG, "%s: Invalid period, must be 1 or bigger", __func__);
        return false;
    }
    queue_msg_t msg = {.cmd = CMD_NEW_PERIOD, .evp = evp, .new_period_ms = ms};
    return pdTRUE == xQueueSend(xEventQueue, &msg, 0);
}

void eventer_init()
{
    xEventQueue = xQueueCreate(5, sizeof(queue_msg_t));
    if (xTaskCreate(&event_task, "eventer", 1024 * 4, NULL, 15, NULL) != errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY)
        ESP_LOGI(TAG, "%s: Eventer started", __func__);
}

void eventer_deinit()
{
    queue_msg_t msg = {.cmd = CMD_EXIT, .evp = NULL, .new_period_ms = 0};
    xQueueSend(xEventQueue, &msg, 0);
}