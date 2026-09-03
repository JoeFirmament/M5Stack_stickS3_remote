#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STICK_S3_EVENT_NONE = 0,
    STICK_S3_EVENT_BUTTON_A,
    STICK_S3_EVENT_BUTTON_B,
    STICK_S3_EVENT_HOLD_A,
    STICK_S3_EVENT_HOLD_B,
    STICK_S3_EVENT_HOME,
    STICK_S3_EVENT_WAKE,
    STICK_S3_EVENT_JOY_UP,
    STICK_S3_EVENT_JOY_DOWN,
    STICK_S3_EVENT_JOY_LEFT,
    STICK_S3_EVENT_JOY_RIGHT,
    STICK_S3_EVENT_JOY_CLICK,
    STICK_S3_EVENT_JOY_HOLD,
} stick_s3_event_t;

typedef enum {
    STICK_S3_VIEW_ACTIVE = 0,
    STICK_S3_VIEW_SELECT,
} stick_s3_view_t;

void stick_s3_ui_init(void);
stick_s3_event_t stick_s3_ui_poll(void);
bool stick_s3_joystick_connected(void);
void stick_s3_ui_draw(bool connected, bool ready, int mode, int view,
                      int reader_mapping, const char *last_action);

#ifdef __cplusplus
}
#endif
