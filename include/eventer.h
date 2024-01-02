#ifndef __eventer_h__
#define __eventer_h__
#ifdef __cplusplus
extern "C"
{
#endif

typedef void* eventer_t;

void eventer_init();
void eventer_deinit();
eventer_t eventer_add(esp_event_loop_handle_t loop_handle, esp_event_base_t loop_base, int ms, bool periodic, int id, void *data, size_t data_size);
bool eventer_set_period(eventer_t ev, int ms);
void eventer_remove(eventer_t ev);

#ifdef __cplusplus
} // extern "C"
#endif
#endif