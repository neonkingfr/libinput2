/*
 * Copyright © 2013 Jonas Ådahl
 * Copyright © 2013-2015 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "libinput.h"
#include "libinput-util.h"
#include "libinput-private.h"

#define require_event_type(li_, type_, retval_, ...)	\
	if (type_ == LIBINPUT_EVENT_NONE) abort(); \
	if (!check_event_type(li_, __func__, type_, __VA_ARGS__, -1)) \
		return retval_; \

static inline bool
check_event_type(struct libinput *libinput,
		 const char *function_name,
		 enum libinput_event_type type_in,
		 ...)
{
	bool rc = false;
	va_list args;
	unsigned int type_permitted;

	va_start(args, type_in);
	type_permitted = va_arg(args, unsigned int);

	while (type_permitted != (unsigned int)-1) {
		if (type_permitted == type_in) {
			rc = true;
			break;
		}
		type_permitted = va_arg(args, unsigned int);
	}

	va_end(args);

	if (!rc)
		log_bug_client(libinput,
			       "Invalid event type %d passed to %s()\n",
			       type_in, function_name);

	return rc;
}

struct libinput_source {
	libinput_source_dispatch_t dispatch;
	void *user_data;
	int fd;
	struct list link;
};

struct libinput_event_device_notify {
	struct libinput_event base;
};

struct libinput_event_keyboard {
	struct libinput_event base;
	uint64_t time;
	uint32_t key;
	uint32_t seat_key_count;
	enum libinput_key_state state;
};

struct libinput_event_pointer {
	struct libinput_event base;
	uint64_t time;
	struct normalized_coords delta;
	struct device_float_coords delta_raw;
	struct device_coords absolute;
	struct discrete_coords discrete;
	uint32_t button;
	uint32_t seat_button_count;
	enum libinput_button_state state;
	enum libinput_pointer_axis_source source;
	uint32_t axes;
};

struct libinput_event_touch {
	struct libinput_event base;
	uint64_t time;
	int32_t slot;
	int32_t seat_slot;
	struct device_coords point;
};

struct libinput_event_gesture {
	struct libinput_event base;
	uint64_t time;
	int finger_count;
	int cancelled;
	struct normalized_coords delta;
	struct normalized_coords delta_unaccel;
	double scale;
	double angle;
};

static void
libinput_default_log_func(struct libinput *libinput,
			  enum libinput_log_priority priority,
			  const char *format, va_list args)
{
	const char *prefix;

	switch(priority) {
	case LIBINPUT_LOG_PRIORITY_DEBUG: prefix = "debug"; break;
	case LIBINPUT_LOG_PRIORITY_INFO: prefix = "info"; break;
	case LIBINPUT_LOG_PRIORITY_ERROR: prefix = "error"; break;
	default: prefix="<invalid priority>"; break;
	}

	fprintf(stderr, "libinput %s: ", prefix);
	vfprintf(stderr, format, args);
}

void
log_msg_va(struct libinput *libinput,
	   enum libinput_log_priority priority,
	   const char *format,
	   va_list args)
{
	if (libinput->log_handler &&
	    libinput->log_priority <= priority)
		libinput->log_handler(libinput, priority, format, args);
}

void
log_msg(struct libinput *libinput,
	enum libinput_log_priority priority,
	const char *format, ...)
{
	va_list args;

	va_start(args, format);
	log_msg_va(libinput, priority, format, args);
	va_end(args);
}

void
log_msg_ratelimit(struct libinput *libinput,
		  struct ratelimit *ratelimit,
		  enum libinput_log_priority priority,
		  const char *format, ...)
{
	va_list args;
	enum ratelimit_state state;

	state = ratelimit_test(ratelimit);
	if (state == RATELIMIT_EXCEEDED)
		return;

	va_start(args, format);
	log_msg_va(libinput, priority, format, args);
	va_end(args);

	if (state == RATELIMIT_THRESHOLD)
		log_msg(libinput,
			priority,
			"WARNING: log rate limit exceeded (%d msgs per %dms). Discarding future messages.\n",
			ratelimit->burst,
			us2ms(ratelimit->interval));
}

LIBINPUT_EXPORT void
libinput_log_set_priority(struct libinput *libinput,
			  enum libinput_log_priority priority)
{
	libinput->log_priority = priority;
}

LIBINPUT_EXPORT enum libinput_log_priority
libinput_log_get_priority(const struct libinput *libinput)
{
	return libinput->log_priority;
}

LIBINPUT_EXPORT void
libinput_log_set_handler(struct libinput *libinput,
			 libinput_log_handler log_handler)
{
	libinput->log_handler = log_handler;
}

static void
libinput_post_event(struct libinput *libinput,
		    struct libinput_event *event);

LIBINPUT_EXPORT enum libinput_event_type
libinput_event_get_type(struct libinput_event *event)
{
	return event->type;
}

LIBINPUT_EXPORT struct libinput *
libinput_event_get_context(struct libinput_event *event)
{
	return event->device->seat->libinput;
}

LIBINPUT_EXPORT struct libinput_device *
libinput_event_get_device(struct libinput_event *event)
{
	return event->device;
}

LIBINPUT_EXPORT struct libinput_event_pointer *
libinput_event_get_pointer_event(struct libinput_event *event)
{
	require_event_type(libinput_event_get_context(event),
			   event->type,
			   NULL,
			   LIBINPUT_EVENT_POINTER_MOTION,
			   LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE,
			   LIBINPUT_EVENT_POINTER_BUTTON,
			   LIBINPUT_EVENT_POINTER_AXIS);

	return (struct libinput_event_pointer *) event;
}

LIBINPUT_EXPORT struct libinput_event_keyboard *
libinput_event_get_keyboard_event(struct libinput_event *event)
{
	require_event_type(libinput_event_get_context(event),
			   event->type,
			   NULL,
			   LIBINPUT_EVENT_KEYBOARD_KEY);

	return (struct libinput_event_keyboard *) event;
}

LIBINPUT_EXPORT struct libinput_event_touch *
libinput_event_get_touch_event(struct libinput_event *event)
{
	require_event_type(libinput_event_get_context(event),
			   event->type,
			   NULL,
			   LIBINPUT_EVENT_TOUCH_DOWN,
			   LIBINPUT_EVENT_TOUCH_UP,
			   LIBINPUT_EVENT_TOUCH_MOTION,
			   LIBINPUT_EVENT_TOUCH_CANCEL,
			   LIBINPUT_EVENT_TOUCH_FRAME);

	return (struct libinput_event_touch *) event;
}

LIBINPUT_EXPORT struct libinput_event_gesture *
libinput_event_get_gesture_event(struct libinput_event *event)
{
	require_event_type(libinput_event_get_context(event),
			   event->type,
			   NULL,
			   LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN,
			   LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE,
			   LIBINPUT_EVENT_GESTURE_SWIPE_END,
			   LIBINPUT_EVENT_GESTURE_PINCH_BEGIN,
			   LIBINPUT_EVENT_GESTURE_PINCH_UPDATE,
			   LIBINPUT_EVENT_GESTURE_PINCH_END);

	return (struct libinput_event_gesture *) event;
}

LIBINPUT_EXPORT struct libinput_event_device_notify *
libinput_event_get_device_notify_event(struct libinput_event *event)
{
	require_event_type(libinput_event_get_context(event),
			   event->type,
			   NULL,
			   LIBINPUT_EVENT_DEVICE_ADDED,
			   LIBINPUT_EVENT_DEVICE_REMOVED);

	return (struct libinput_event_device_notify *) event;
}

LIBINPUT_EXPORT uint32_t
libinput_event_keyboard_get_time(struct libinput_event_keyboard *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_KEYBOARD_KEY);

	return us2ms(event->time);
}

LIBINPUT_EXPORT uint64_t
libinput_event_keyboard_get_time_usec(struct libinput_event_keyboard *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_KEYBOARD_KEY);

	return event->time;
}

LIBINPUT_EXPORT uint32_t
libinput_event_keyboard_get_key(struct libinput_event_keyboard *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_KEYBOARD_KEY);

	return event->key;
}

LIBINPUT_EXPORT enum libinput_key_state
libinput_event_keyboard_get_key_state(struct libinput_event_keyboard *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_KEYBOARD_KEY);

	return event->state;
}

LIBINPUT_EXPORT uint32_t
libinput_event_keyboard_get_seat_key_count(
	struct libinput_event_keyboard *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_KEYBOARD_KEY);

	fprintf(stderr, "%s: partial stub\n", __func__);
	return event->seat_key_count;
}

LIBINPUT_EXPORT uint32_t
libinput_event_pointer_get_time(struct libinput_event_pointer *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_POINTER_MOTION,
			   LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE,
			   LIBINPUT_EVENT_POINTER_BUTTON,
			   LIBINPUT_EVENT_POINTER_AXIS);

	return us2ms(event->time);
}

LIBINPUT_EXPORT uint64_t
libinput_event_pointer_get_time_usec(struct libinput_event_pointer *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_POINTER_MOTION,
			   LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE,
			   LIBINPUT_EVENT_POINTER_BUTTON,
			   LIBINPUT_EVENT_POINTER_AXIS);

	return event->time;
}

LIBINPUT_EXPORT double
libinput_event_pointer_get_dx(struct libinput_event_pointer *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_POINTER_MOTION);

	return event->delta.x;
}

LIBINPUT_EXPORT double
libinput_event_pointer_get_dy(struct libinput_event_pointer *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_POINTER_MOTION);

	return event->delta.y;
}

LIBINPUT_EXPORT double
libinput_event_pointer_get_dx_unaccelerated(
	struct libinput_event_pointer *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_POINTER_MOTION);

	fprintf(stderr, "%s: partial stub\n", __func__);
	return event->delta_raw.x;
}

LIBINPUT_EXPORT double
libinput_event_pointer_get_dy_unaccelerated(
	struct libinput_event_pointer *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_POINTER_MOTION);

	fprintf(stderr, "%s: partial stub\n", __func__);
	return event->delta_raw.y;
}

LIBINPUT_EXPORT double
libinput_event_pointer_get_absolute_x(struct libinput_event_pointer *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE);

	fprintf(stderr, "%s: stub\n", __func__);
	return (double)(-1);
}

LIBINPUT_EXPORT double
libinput_event_pointer_get_absolute_y(struct libinput_event_pointer *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE);

	fprintf(stderr, "%s: stub\n", __func__);
	return (double)(-1);
}

LIBINPUT_EXPORT double
libinput_event_pointer_get_absolute_x_transformed(
	struct libinput_event_pointer *event,
	uint32_t width)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE);

	fprintf(stderr, "%s: stub\n", __func__);
	return (double)(-1);
}

LIBINPUT_EXPORT double
libinput_event_pointer_get_absolute_y_transformed(
	struct libinput_event_pointer *event,
	uint32_t height)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE);

	fprintf(stderr, "%s: stub\n", __func__);
	return (double)(-1);
}

LIBINPUT_EXPORT uint32_t
libinput_event_pointer_get_button(struct libinput_event_pointer *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_POINTER_BUTTON);

	return event->button;
}

LIBINPUT_EXPORT enum libinput_button_state
libinput_event_pointer_get_button_state(struct libinput_event_pointer *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_POINTER_BUTTON);

	return event->state;
}

LIBINPUT_EXPORT uint32_t
libinput_event_pointer_get_seat_button_count(
	struct libinput_event_pointer *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_POINTER_BUTTON);

	fprintf(stderr, "%s: partial stub\n", __func__);
	return event->seat_button_count;
}

LIBINPUT_EXPORT int
libinput_event_pointer_has_axis(struct libinput_event_pointer *event,
				enum libinput_pointer_axis axis)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_POINTER_AXIS);

	/* Event type LIBINPUT_EVENT_POINTER_AXIS is unsupported. */
	return 0;
}

LIBINPUT_EXPORT double
libinput_event_pointer_get_axis_value(struct libinput_event_pointer *event,
      enum libinput_pointer_axis axis)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0.0,
			   LIBINPUT_EVENT_POINTER_AXIS);

	return 0;
}

LIBINPUT_EXPORT double
libinput_event_pointer_get_axis_value_discrete(struct libinput_event_pointer *event,
					       enum libinput_pointer_axis axis)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0.0,
			   LIBINPUT_EVENT_POINTER_AXIS);

	return 0;
}

LIBINPUT_EXPORT enum libinput_pointer_axis_source
libinput_event_pointer_get_axis_source(struct libinput_event_pointer *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_POINTER_AXIS);

	/* XXX impossible to return an error, so pick a source type. */
	return LIBINPUT_POINTER_AXIS_SOURCE_WHEEL;
}

LIBINPUT_EXPORT uint32_t
libinput_event_touch_get_time(struct libinput_event_touch *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_TOUCH_DOWN,
			   LIBINPUT_EVENT_TOUCH_UP,
			   LIBINPUT_EVENT_TOUCH_MOTION,
			   LIBINPUT_EVENT_TOUCH_CANCEL,
			   LIBINPUT_EVENT_TOUCH_FRAME);

	return us2ms(event->time);
}

LIBINPUT_EXPORT uint64_t
libinput_event_touch_get_time_usec(struct libinput_event_touch *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_TOUCH_DOWN,
			   LIBINPUT_EVENT_TOUCH_UP,
			   LIBINPUT_EVENT_TOUCH_MOTION,
			   LIBINPUT_EVENT_TOUCH_CANCEL,
			   LIBINPUT_EVENT_TOUCH_FRAME);

	return event->time;
}

LIBINPUT_EXPORT int32_t
libinput_event_touch_get_slot(struct libinput_event_touch *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_TOUCH_DOWN,
			   LIBINPUT_EVENT_TOUCH_UP,
			   LIBINPUT_EVENT_TOUCH_MOTION,
			   LIBINPUT_EVENT_TOUCH_CANCEL);
	fprintf(stderr, "%s: partial stub\n", __func__);
	return event->slot;
}

LIBINPUT_EXPORT int32_t
libinput_event_touch_get_seat_slot(struct libinput_event_touch *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_TOUCH_DOWN,
			   LIBINPUT_EVENT_TOUCH_UP,
			   LIBINPUT_EVENT_TOUCH_MOTION,
			   LIBINPUT_EVENT_TOUCH_CANCEL);

	fprintf(stderr, "%s: partial stub\n", __func__);
	return event->seat_slot;
}

LIBINPUT_EXPORT double
libinput_event_touch_get_x(struct libinput_event_touch *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_TOUCH_DOWN,
			   LIBINPUT_EVENT_TOUCH_MOTION);

	fprintf(stderr, "%s: stub\n", __func__);
	return (double)(-1);
}

LIBINPUT_EXPORT double
libinput_event_touch_get_x_transformed(struct libinput_event_touch *event,
				       uint32_t width)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_TOUCH_DOWN,
			   LIBINPUT_EVENT_TOUCH_MOTION);

	fprintf(stderr, "%s: stub\n", __func__);
	return (double)(-1);
}

LIBINPUT_EXPORT double
libinput_event_touch_get_y_transformed(struct libinput_event_touch *event,
				       uint32_t height)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_TOUCH_DOWN,
			   LIBINPUT_EVENT_TOUCH_MOTION);

	fprintf(stderr, "%s: stub\n", __func__);
	return (double)(-1);
}

LIBINPUT_EXPORT double
libinput_event_touch_get_y(struct libinput_event_touch *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_TOUCH_DOWN,
			   LIBINPUT_EVENT_TOUCH_MOTION);

	fprintf(stderr, "%s: stub\n", __func__);
	return (double)(-1);
}

LIBINPUT_EXPORT uint32_t
libinput_event_gesture_get_time(struct libinput_event_gesture *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_GESTURE_PINCH_BEGIN,
			   LIBINPUT_EVENT_GESTURE_PINCH_UPDATE,
			   LIBINPUT_EVENT_GESTURE_PINCH_END,
			   LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN,
			   LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE,
			   LIBINPUT_EVENT_GESTURE_SWIPE_END);

	return us2ms(event->time);
}

LIBINPUT_EXPORT uint64_t
libinput_event_gesture_get_time_usec(struct libinput_event_gesture *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_GESTURE_PINCH_BEGIN,
			   LIBINPUT_EVENT_GESTURE_PINCH_UPDATE,
			   LIBINPUT_EVENT_GESTURE_PINCH_END,
			   LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN,
			   LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE,
			   LIBINPUT_EVENT_GESTURE_SWIPE_END);

	return event->time;
}

LIBINPUT_EXPORT int
libinput_event_gesture_get_finger_count(struct libinput_event_gesture *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_GESTURE_PINCH_BEGIN,
			   LIBINPUT_EVENT_GESTURE_PINCH_UPDATE,
			   LIBINPUT_EVENT_GESTURE_PINCH_END,
			   LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN,
			   LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE,
			   LIBINPUT_EVENT_GESTURE_SWIPE_END);

	fprintf(stderr, "%s: partial stub\n", __func__);
	return event->finger_count;
}

LIBINPUT_EXPORT int
libinput_event_gesture_get_cancelled(struct libinput_event_gesture *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0,
			   LIBINPUT_EVENT_GESTURE_PINCH_END,
			   LIBINPUT_EVENT_GESTURE_SWIPE_END);

	fprintf(stderr, "%s: partial stub\n", __func__);
	return event->cancelled;
}

LIBINPUT_EXPORT double
libinput_event_gesture_get_dx(struct libinput_event_gesture *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0.0,
			   LIBINPUT_EVENT_GESTURE_PINCH_BEGIN,
			   LIBINPUT_EVENT_GESTURE_PINCH_UPDATE,
			   LIBINPUT_EVENT_GESTURE_PINCH_END,
			   LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN,
			   LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE,
			   LIBINPUT_EVENT_GESTURE_SWIPE_END);

	fprintf(stderr, "%s: partial stub\n", __func__);
	return event->delta.x;
}

LIBINPUT_EXPORT double
libinput_event_gesture_get_dy(struct libinput_event_gesture *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0.0,
			   LIBINPUT_EVENT_GESTURE_PINCH_BEGIN,
			   LIBINPUT_EVENT_GESTURE_PINCH_UPDATE,
			   LIBINPUT_EVENT_GESTURE_PINCH_END,
			   LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN,
			   LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE,
			   LIBINPUT_EVENT_GESTURE_SWIPE_END);

	fprintf(stderr, "%s: partial stub\n", __func__);
	return event->delta.y;
}

LIBINPUT_EXPORT double
libinput_event_gesture_get_dx_unaccelerated(
	struct libinput_event_gesture *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0.0,
			   LIBINPUT_EVENT_GESTURE_PINCH_BEGIN,
			   LIBINPUT_EVENT_GESTURE_PINCH_UPDATE,
			   LIBINPUT_EVENT_GESTURE_PINCH_END,
			   LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN,
			   LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE,
			   LIBINPUT_EVENT_GESTURE_SWIPE_END);

	fprintf(stderr, "%s: partial stub\n", __func__);
	return event->delta_unaccel.x;
}

LIBINPUT_EXPORT double
libinput_event_gesture_get_dy_unaccelerated(
	struct libinput_event_gesture *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0.0,
			   LIBINPUT_EVENT_GESTURE_PINCH_BEGIN,
			   LIBINPUT_EVENT_GESTURE_PINCH_UPDATE,
			   LIBINPUT_EVENT_GESTURE_PINCH_END,
			   LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN,
			   LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE,
			   LIBINPUT_EVENT_GESTURE_SWIPE_END);

	fprintf(stderr, "%s: partial stub\n", __func__);
	return event->delta_unaccel.y;
}

LIBINPUT_EXPORT double
libinput_event_gesture_get_scale(struct libinput_event_gesture *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0.0,
			   LIBINPUT_EVENT_GESTURE_PINCH_BEGIN,
			   LIBINPUT_EVENT_GESTURE_PINCH_UPDATE,
			   LIBINPUT_EVENT_GESTURE_PINCH_END);

	fprintf(stderr, "%s: partial stub\n", __func__);
	return event->scale;
}

LIBINPUT_EXPORT double
libinput_event_gesture_get_angle_delta(struct libinput_event_gesture *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   0.0,
			   LIBINPUT_EVENT_GESTURE_PINCH_BEGIN,
			   LIBINPUT_EVENT_GESTURE_PINCH_UPDATE,
			   LIBINPUT_EVENT_GESTURE_PINCH_END);

	fprintf(stderr, "%s: partial stub\n", __func__);
	return event->angle;
}

struct libinput_source *
libinput_add_fd(struct libinput *libinput,
		int fd,
		libinput_source_dispatch_t dispatch,
		void *user_data)
{
	struct libinput_source *source;
	struct kevent kev;

	source = calloc(1, sizeof *source);
	if (!source)
		return NULL;

	source->dispatch = dispatch;
	source->user_data = user_data;
	source->fd = fd;

	EV_SET(&kev, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, source);
	if (kevent(libinput->kq, &kev, 1, NULL, 0, NULL)) {
		free(source);
		return NULL;
	}

	return source;
}

void
libinput_remove_source(struct libinput *libinput,
		       struct libinput_source *source)
{
	struct kevent kev;

	EV_SET(&kev, source->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
	kevent(libinput->kq, &kev, 1, NULL, 0, NULL);
	source->fd = -1;
	list_insert(&libinput->source_destroy_list, &source->link);
}

int
libinput_init(struct libinput *libinput,
	      const struct libinput_interface *interface,
	      void *user_data)
{
	if ((libinput->kq = kqueue()) == -1)
		return -1;

	libinput->events_len = 4;
	libinput->events = calloc(libinput->events_len,
	    sizeof(*libinput->events));
	if (!libinput->events) {
		close(libinput->kq);
		return -1;
	}

	libinput->log_handler = libinput_default_log_func;
	libinput->log_priority = LIBINPUT_LOG_PRIORITY_ERROR;
	libinput->interface = interface;
	libinput->user_data = user_data;
	libinput->refcount = 1;
	list_init(&libinput->source_destroy_list);
	list_init(&libinput->seat_list);

	return 0;
}

static void
libinput_device_destroy(struct libinput_device *device);

static void
libinput_seat_destroy(struct libinput_seat *seat);

static void
libinput_drop_destroyed_sources(struct libinput *libinput)
{
	struct libinput_source *source, *next;

	list_for_each_safe(source, next, &libinput->source_destroy_list, link)
		free(source);
	list_init(&libinput->source_destroy_list);
}

LIBINPUT_EXPORT struct libinput *
libinput_ref(struct libinput *libinput)
{
	libinput->refcount++;
	return libinput;
}

LIBINPUT_EXPORT struct libinput *
libinput_unref(struct libinput *libinput)
{
	struct libinput_event *event;
	struct libinput_device *device, *next_device;
	struct libinput_seat *seat, *next_seat;

	if (libinput == NULL)
		return NULL;

	assert(libinput->refcount > 0);
	libinput->refcount--;
	if (libinput->refcount > 0)
		return libinput;

	while ((event = libinput_get_event(libinput)))
	       libinput_event_destroy(event);

	free(libinput->events);

	list_for_each_safe(seat, next_seat, &libinput->seat_list, link) {
		list_for_each_safe(device, next_device,
				   &seat->devices_list,
				   link)
			libinput_device_destroy(device);

		libinput_seat_destroy(seat);
	}

	libinput_drop_destroyed_sources(libinput);
	close(libinput->kq);
	free(libinput);

	return NULL;
}

LIBINPUT_EXPORT void
libinput_event_destroy(struct libinput_event *event)
{
	if (event == NULL)
		return;

	if (event->device)
		libinput_device_unref(event->device);

	free(event);
}

int
open_restricted(struct libinput *libinput,
		const char *path, int flags)
{
	return libinput->interface->open_restricted(path,
						    flags,
						    libinput->user_data);
}

void
close_restricted(struct libinput *libinput, int fd)
{
	return libinput->interface->close_restricted(fd, libinput->user_data);
}

void
libinput_seat_init(struct libinput_seat *seat,
		   struct libinput *libinput,
		   const char *physical_name,
		   const char *logical_name)
{
	seat->refcount = 1;
	seat->libinput = libinput;
	seat->physical_name = strdup(physical_name);
	seat->logical_name = strdup(logical_name);
	list_init(&seat->devices_list);
	list_insert(&libinput->seat_list, &seat->link);
}

LIBINPUT_EXPORT struct libinput_seat *
libinput_seat_ref(struct libinput_seat *seat)
{
	seat->refcount++;
	return seat;
}

static void
libinput_seat_destroy(struct libinput_seat *seat)
{
	list_remove(&seat->link);
	free(seat->logical_name);
	free(seat->physical_name);
	free(seat);
}

LIBINPUT_EXPORT struct libinput_seat *
libinput_seat_unref(struct libinput_seat *seat)
{
	assert(seat->refcount > 0);
	seat->refcount--;
	if (seat->refcount == 0) {
		libinput_seat_destroy(seat);
		return NULL;
	} else {
		return seat;
	}
}

LIBINPUT_EXPORT void
libinput_seat_set_user_data(struct libinput_seat *seat, void *user_data)
{
	seat->user_data = user_data;
}

LIBINPUT_EXPORT void *
libinput_seat_get_user_data(struct libinput_seat *seat)
{
	return seat->user_data;
}

LIBINPUT_EXPORT struct libinput *
libinput_seat_get_context(struct libinput_seat *seat)
{
	return seat->libinput;
}

LIBINPUT_EXPORT const char *
libinput_seat_get_physical_name(struct libinput_seat *seat)
{
	return seat->physical_name;
}

LIBINPUT_EXPORT const char *
libinput_seat_get_logical_name(struct libinput_seat *seat)
{
	return seat->logical_name;
}


void
libinput_device_init(struct libinput_device *device,
		     struct libinput_seat *seat)
{
	device->seat = seat;
	device->refcount = 1;
}

LIBINPUT_EXPORT struct libinput_device *
libinput_device_ref(struct libinput_device *device)
{
	device->refcount++;
	return device;
}

static void
libinput_device_destroy(struct libinput_device *device)
{
	list_remove(&device->link);
	libinput_seat_unref(device->seat);
	free(device);
}

LIBINPUT_EXPORT struct libinput_device *
libinput_device_unref(struct libinput_device *device)
{
	assert(device->refcount > 0);
	device->refcount--;
	if (device->refcount == 0) {
		libinput_device_destroy(device);
		return NULL;
	} else {
		return device;
	}
}

LIBINPUT_EXPORT int
libinput_get_fd(struct libinput *libinput)
{
	return libinput->kq;
}

LIBINPUT_EXPORT  int
libinput_dispatch(struct libinput *libinput)
{
	struct libinput_source *source;
	struct kevent kev[1];
	struct timespec ts = { 0, 0 };
	int i, count;

	count = kevent(libinput->kq, NULL, 0, kev, ARRAY_LENGTH(kev), &ts);
	if (count == -1)
		return -errno;

	for (i = 0; i < count; i++) {
		if (kev[i].filter != EVFILT_READ)
			continue;

		source = kev[i].udata;
		if (source->fd == -1)
			continue;

		source->dispatch(source->user_data);
	}

	libinput_drop_destroyed_sources(libinput);

	return 0;
}

static uint32_t
update_seat_key_count(struct libinput_seat *seat,
		      int32_t key,
		      enum libinput_key_state state)
{
	assert(key >= 0 && key <= KEY_MAX);

	switch (state) {
	case LIBINPUT_KEY_STATE_PRESSED:
		return ++seat->button_count[key];
	case LIBINPUT_KEY_STATE_RELEASED:
		/* We might not have received the first PRESSED event. */
		if (seat->button_count[key] == 0)
			return 0;

		return --seat->button_count[key];
	}

	return 0;
}

static uint32_t
update_seat_button_count(struct libinput_seat *seat,
			 int32_t button,
			 enum libinput_button_state state)
{
	assert(button >= 0 && button <= KEY_MAX);

	switch (state) {
	case LIBINPUT_BUTTON_STATE_PRESSED:
		return ++seat->button_count[button];
	case LIBINPUT_BUTTON_STATE_RELEASED:
		/* We might not have received the first PRESSED event. */
		if (seat->button_count[button] == 0)
			return 0;

		return --seat->button_count[button];
	}

	return 0;
}

static void
init_event_base(struct libinput_event *event,
		struct libinput_device *device,
		enum libinput_event_type type)
{
	event->type = type;
	event->device = device;
}

void
post_device_event(struct libinput_device *device,
		  uint64_t time,
		  enum libinput_event_type type,
		  struct libinput_event *event)
{
	init_event_base(event, device, type);
	fprintf(stderr, "   %s: %s %p %p\n", __func__, device->devname, event, event->device);
	libinput_post_event(device->seat->libinput, event);
}

static inline bool
device_has_cap(struct libinput_device *device,
	       enum libinput_device_capability cap)
{
	const char *capability;

	if (libinput_device_has_capability(device, cap))
		return true;

	switch (cap) {
	case LIBINPUT_DEVICE_CAP_POINTER:
		capability = "CAP_POINTER";
		break;
	case LIBINPUT_DEVICE_CAP_KEYBOARD:
		capability = "CAP_KEYBOARD";
		break;
	case LIBINPUT_DEVICE_CAP_TOUCH:
		capability = "CAP_TOUCH";
		break;
	case LIBINPUT_DEVICE_CAP_GESTURE:
		capability = "CAP_GESTURE";
		break;
	case LIBINPUT_DEVICE_CAP_TABLET_TOOL:
		capability = "CAP_TABLET_TOOL";
		break;
	case LIBINPUT_DEVICE_CAP_TABLET_PAD:
		capability = "CAP_TABLET_PAD";
		break;
	case LIBINPUT_DEVICE_CAP_SWITCH:
		capability = "CAP_SWITCH";
		break;
	}

	log_bug_libinput(device->seat->libinput,
			 "Event for missing capability %s on device \"%s\"\n",
			 capability,
			 libinput_device_get_name(device));

	return false;
}

void
keyboard_notify_key(struct libinput_device *device,
		    uint64_t time,
		    uint32_t key,
		    enum libinput_key_state state)
{
	struct libinput_event_keyboard *key_event;
	uint32_t seat_key_count;

	if (!device_has_cap(device, LIBINPUT_DEVICE_CAP_KEYBOARD))
		return;

	key_event = zalloc(sizeof *key_event);
	if (!key_event)
		return;

	seat_key_count = update_seat_key_count(device->seat, key, state);

	*key_event = (struct libinput_event_keyboard) {
		.time = time,
		.key = key,
		.state = state,
		.seat_key_count = seat_key_count,
	};

	post_device_event(device, time,
			  LIBINPUT_EVENT_KEYBOARD_KEY,
			  &key_event->base);
}

void
pointer_notify_motion(struct libinput_device *device,
		      uint64_t time,
		      const struct normalized_coords *delta,
		      const struct device_float_coords *raw)
{
	struct libinput_event_pointer *motion_event;

	if (!device_has_cap(device, LIBINPUT_DEVICE_CAP_POINTER))
		return;

	motion_event = zalloc(sizeof *motion_event);
	if (!motion_event)
		return;

	*motion_event = (struct libinput_event_pointer) {
		.time = time,
		.delta = *delta,
		.delta_raw = *raw,
	};

	post_device_event(device, time,
			  LIBINPUT_EVENT_POINTER_MOTION,
			  &motion_event->base);
}

void
pointer_notify_button(struct libinput_device *device,
		      uint64_t time,
		      int32_t button,
		      enum libinput_button_state state)
{
	struct libinput_event_pointer *button_event;
	int32_t seat_button_count;

	if (!device_has_cap(device, LIBINPUT_DEVICE_CAP_POINTER))
		return;

	button_event = zalloc(sizeof *button_event);
	if (!button_event)
		return;

	seat_button_count = update_seat_button_count(device->seat,
						     button,
						     state);

	*button_event = (struct libinput_event_pointer) {
		.time = time,
		.button = button,
		.state = state,
		.seat_button_count = seat_button_count,
	};

	post_device_event(device, time,
			  LIBINPUT_EVENT_POINTER_BUTTON,
			  &button_event->base);
}
static void
libinput_post_event(struct libinput *libinput,
		    struct libinput_event *event)
{
	struct libinput_event **events = libinput->events;
	size_t events_len = libinput->events_len;
	size_t events_count = libinput->events_count;
	size_t move_len;
	size_t new_out;

	events_count++;
	if (events_count > events_len) {
		events_len *= 2;
		events = realloc(events, events_len * sizeof *events);
		if (!events) {
			fprintf(stderr, "Failed to reallocate event ring "
				"buffer");
			return;
		}

		if (libinput->events_count > 0 && libinput->events_in == 0) {
			libinput->events_in = libinput->events_len;
		} else if (libinput->events_count > 0 &&
			   libinput->events_out >= libinput->events_in) {
			move_len = libinput->events_len - libinput->events_out;
			new_out = events_len - move_len;
			memmove(events + new_out,
				events + libinput->events_out,
				move_len * sizeof *events);
			libinput->events_out = new_out;
		}

		libinput->events = events;
		libinput->events_len = events_len;
	}

	if (event->device)
		libinput_device_ref(event->device);

	libinput->events_count = events_count;
	events[libinput->events_in] = event;
	libinput->events_in = (libinput->events_in + 1) % libinput->events_len;
}

LIBINPUT_EXPORT struct libinput_event *
libinput_get_event(struct libinput *libinput)
{
	struct libinput_event *event;

	if (libinput->events_count == 0)
		return NULL;

	event = libinput->events[libinput->events_out];
	libinput->events_out =
		(libinput->events_out + 1) % libinput->events_len;
	libinput->events_count--;

	fprintf(stderr, "%s: %p %p\n", __func__, event, event->device);
	return event;
}

LIBINPUT_EXPORT enum libinput_event_type
libinput_next_event_type(struct libinput *libinput)
{
	struct libinput_event *event;

	if (libinput->events_count == 0)
		return LIBINPUT_EVENT_NONE;

	event = libinput->events[libinput->events_out];
	return event->type;
}

LIBINPUT_EXPORT void
libinput_set_user_data(struct libinput *libinput,
		       void *user_data)
{
	libinput->user_data = user_data;
}

LIBINPUT_EXPORT void *
libinput_get_user_data(struct libinput *libinput)
{
	return libinput->user_data;
}

LIBINPUT_EXPORT int
libinput_resume(struct libinput *libinput)
{
	return 0; // XXX
}

LIBINPUT_EXPORT void
libinput_suspend(struct libinput *libinput)
{
}

LIBINPUT_EXPORT void
libinput_device_set_user_data(struct libinput_device *device, void *user_data)
{
	device->user_data = user_data;
}

LIBINPUT_EXPORT void *
libinput_device_get_user_data(struct libinput_device *device)
{
	return device->user_data;
}

LIBINPUT_EXPORT struct libinput *
libinput_device_get_context(struct libinput_device *device)
{
	return libinput_seat_get_context(device->seat);
}

LIBINPUT_EXPORT struct libinput_device_group *
libinput_device_get_device_group(struct libinput_device *device)
{
	/* We do not support device groups. */
	return NULL;
}

LIBINPUT_EXPORT const char *
libinput_device_get_sysname(struct libinput_device *device)
{
	return "unsupported";
}

LIBINPUT_EXPORT const char *
libinput_device_get_name(struct libinput_device *device)
{
	return "unsupported";
}

LIBINPUT_EXPORT unsigned int
libinput_device_get_id_product(struct libinput_device *device)
{
	/* Exposing IDs will result in people crafting hacks. */
	return 0xdeadbeef;
}

LIBINPUT_EXPORT unsigned int
libinput_device_get_id_vendor(struct libinput_device *device)
{
	/* Exposing IDs will result in people crafting hacks. */
	return 0xdeadbeef;
}

LIBINPUT_EXPORT const char *
libinput_device_get_output_name(struct libinput_device *device)
{
	return NULL;
}

LIBINPUT_EXPORT struct libinput_seat *
libinput_device_get_seat(struct libinput_device *device)
{
	return device->seat;
}

LIBINPUT_EXPORT int
libinput_device_set_seat_logical_name(struct libinput_device *device,
	const char *name)
{
	return -1;
}

LIBINPUT_EXPORT struct udev_device *
libinput_device_get_udev_device(struct libinput_device *device)
{
	return NULL;
}

LIBINPUT_EXPORT void
libinput_device_led_update(struct libinput_device *device,
	enum libinput_led leds)
{
}

LIBINPUT_EXPORT int
libinput_device_has_capability(struct libinput_device *device,
       enum libinput_device_capability capability)
{
	int rc = 0;

	switch (capability) {
	case LIBINPUT_DEVICE_CAP_POINTER:
		if (strncmp(device->devname, "/dev/wsmouse", 12) == 0)
			rc = 1;
		break;
	case LIBINPUT_DEVICE_CAP_KEYBOARD:
		if (strncmp(device->devname, "/dev/wskbd", 10) == 0)
			rc = 1;
		break;
	case LIBINPUT_DEVICE_CAP_TOUCH:
	case LIBINPUT_DEVICE_CAP_GESTURE:
	default:
		break;
	}

	return rc;
}

LIBINPUT_EXPORT int
libinput_device_get_size(struct libinput_device *device,
	 double *width, double *height)
{
	fprintf(stderr, "%s: stub\n", __func__);
	return (-1);
}

LIBINPUT_EXPORT int
libinput_device_pointer_has_button(struct libinput_device *device, uint32_t code)
{
	fprintf(stderr, "%s: stub\n", __func__);
	return (-1);
}

LIBINPUT_EXPORT int
libinput_device_keyboard_has_key(struct libinput_device *device, uint32_t code)
{
	fprintf(stderr, "%s: stub\n", __func__);
	return (-1);
}

LIBINPUT_EXPORT struct libinput_event *
libinput_event_device_notify_get_base_event(struct libinput_event_device_notify *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   NULL,
			   LIBINPUT_EVENT_DEVICE_ADDED,
			   LIBINPUT_EVENT_DEVICE_REMOVED);

	return &event->base;
}

LIBINPUT_EXPORT struct libinput_event *
libinput_event_keyboard_get_base_event(struct libinput_event_keyboard *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   NULL,
			   LIBINPUT_EVENT_KEYBOARD_KEY);

	return &event->base;
}

LIBINPUT_EXPORT struct libinput_event *
libinput_event_pointer_get_base_event(struct libinput_event_pointer *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   NULL,
			   LIBINPUT_EVENT_POINTER_MOTION,
			   LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE,
			   LIBINPUT_EVENT_POINTER_BUTTON,
			   LIBINPUT_EVENT_POINTER_AXIS);

	return &event->base;
}

LIBINPUT_EXPORT struct libinput_event *
libinput_event_touch_get_base_event(struct libinput_event_touch *event)
{
	require_event_type(libinput_event_get_context(&event->base),
			   event->base.type,
			   NULL,
			   LIBINPUT_EVENT_TOUCH_DOWN,
			   LIBINPUT_EVENT_TOUCH_UP,
			   LIBINPUT_EVENT_TOUCH_MOTION,
			   LIBINPUT_EVENT_TOUCH_CANCEL,
			   LIBINPUT_EVENT_TOUCH_FRAME);

	return &event->base;
}

LIBINPUT_EXPORT struct libinput_event *
libinput_event_gesture_get_base_event(struct libinput_event_gesture *event)
{
	return &event->base;
}

LIBINPUT_EXPORT struct libinput_device_group *
libinput_device_group_ref(struct libinput_device_group *group)
{
	assert(group == NULL);
	return NULL; // XXX
}

LIBINPUT_EXPORT struct libinput_device_group *
libinput_device_group_unref(struct libinput_device_group *group)
{
	assert(group == NULL);
	return NULL; // XXX
}

LIBINPUT_EXPORT void
libinput_device_group_set_user_data(struct libinput_device_group *group,
				    void *user_data)
{
	assert(group == NULL);
}

LIBINPUT_EXPORT void *
libinput_device_group_get_user_data(struct libinput_device_group *group)
{
	assert(group == NULL);
	return NULL; // XXX
}

LIBINPUT_EXPORT const char *
libinput_config_status_to_str(enum libinput_config_status status)
{
	const char *str = NULL;

	switch (status) {
	case LIBINPUT_CONFIG_STATUS_SUCCESS:
		str = "Success";
		break;
	case LIBINPUT_CONFIG_STATUS_UNSUPPORTED:
		str = "Unsupported configuration option";
		break;
	case LIBINPUT_CONFIG_STATUS_INVALID:
		str = "Invalid argument range";
		break;
	}

	return str;
}

LIBINPUT_EXPORT int
libinput_device_config_tap_get_finger_count(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_config_status
libinput_device_config_tap_set_enabled(struct libinput_device *device,
	enum libinput_config_tap_state enable)
{
	return LIBINPUT_CONFIG_STATUS_INVALID;
}

LIBINPUT_EXPORT enum libinput_config_tap_state
libinput_device_config_tap_get_enabled(struct libinput_device *device)
{
	return LIBINPUT_CONFIG_TAP_DISABLED;
}

LIBINPUT_EXPORT enum libinput_config_tap_state
libinput_device_config_tap_get_default_enabled(struct libinput_device *device)
{
	return LIBINPUT_CONFIG_TAP_DISABLED;
}

LIBINPUT_EXPORT enum libinput_config_status
libinput_device_config_tap_set_drag_lock_enabled(struct libinput_device *device,
	enum libinput_config_drag_lock_state enable)
{
	return LIBINPUT_CONFIG_STATUS_INVALID;
}

LIBINPUT_EXPORT enum libinput_config_drag_lock_state
libinput_device_config_tap_get_drag_lock_enabled(struct libinput_device *device)
{
	return LIBINPUT_CONFIG_DRAG_LOCK_DISABLED;
}

LIBINPUT_EXPORT enum libinput_config_drag_lock_state
libinput_device_config_tap_get_default_drag_lock_enabled(struct libinput_device *device)
{
	return LIBINPUT_CONFIG_DRAG_LOCK_DISABLED;
}

LIBINPUT_EXPORT int
libinput_device_config_calibration_has_matrix(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_config_status
libinput_device_config_calibration_set_matrix(struct libinput_device *device,
	const float matrix[6])
{
	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}

LIBINPUT_EXPORT int
libinput_device_config_calibration_get_matrix(struct libinput_device *device,
	float matrix[6])
{
	return 0;
}

LIBINPUT_EXPORT int
libinput_device_config_calibration_get_default_matrix(struct libinput_device *device,
	float matrix[6])
{
	return 0;
}

LIBINPUT_EXPORT uint32_t
libinput_device_config_send_events_get_modes(struct libinput_device *device)
{
	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}

LIBINPUT_EXPORT enum libinput_config_status
libinput_device_config_send_events_set_mode(struct libinput_device *device,
	uint32_t mode)
{
	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

LIBINPUT_EXPORT uint32_t
libinput_device_config_send_events_get_mode(struct libinput_device *device)
{
	return LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;
}

LIBINPUT_EXPORT uint32_t
libinput_device_config_send_events_get_default_mode(struct libinput_device *device)
{
	return LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;
}

LIBINPUT_EXPORT int
libinput_device_config_accel_is_available(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_config_status
libinput_device_config_accel_set_speed(struct libinput_device *device,
				       double speed)
{
	/* Need the negation in case speed is NaN */
	if (!(speed >= -1.0 && speed <= 1.0))
		return LIBINPUT_CONFIG_STATUS_INVALID;

	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}

LIBINPUT_EXPORT double
libinput_device_config_accel_get_speed(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT double
libinput_device_config_accel_get_default_speed(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT int
libinput_device_config_scroll_has_natural_scroll(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_config_status
libinput_device_config_scroll_set_natural_scroll_enabled(struct libinput_device *device,
	int enable)
{
	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}

LIBINPUT_EXPORT int
libinput_device_config_scroll_get_natural_scroll_enabled(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT int
libinput_device_config_scroll_get_default_natural_scroll_enabled(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT int
libinput_device_config_left_handed_is_available(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_config_status
libinput_device_config_left_handed_set(struct libinput_device *device,
	int left_handed)
{
	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}

LIBINPUT_EXPORT int
libinput_device_config_left_handed_get(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT int
libinput_device_config_left_handed_get_default(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT uint32_t
libinput_device_config_click_get_methods(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_config_status
libinput_device_config_click_set_method(struct libinput_device *device,
	enum libinput_config_click_method method)
{
	/* Check method is a single valid method */
	switch (method) {
	case LIBINPUT_CONFIG_CLICK_METHOD_NONE:
	case LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS:
	case LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER:
		break;
	default:
		return LIBINPUT_CONFIG_STATUS_INVALID;
	}

	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}

LIBINPUT_EXPORT enum libinput_config_click_method
libinput_device_config_click_get_method(struct libinput_device *device)
{
	return LIBINPUT_CONFIG_CLICK_METHOD_NONE;
}

LIBINPUT_EXPORT enum libinput_config_click_method
libinput_device_config_click_get_default_method(struct libinput_device *device)
{
	return LIBINPUT_CONFIG_CLICK_METHOD_NONE;
}

LIBINPUT_EXPORT int
libinput_device_config_middle_emulation_is_available(
	struct libinput_device *device)
{
	return LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED;
}

LIBINPUT_EXPORT enum libinput_config_status
libinput_device_config_middle_emulation_set_enabled(
	struct libinput_device *device,
	enum libinput_config_middle_emulation_state enable)
{
	switch (enable) {
	case LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED:
		return LIBINPUT_CONFIG_STATUS_SUCCESS;
	case LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED:
		return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
	default:
		return LIBINPUT_CONFIG_STATUS_INVALID;
	}
}


LIBINPUT_EXPORT enum libinput_config_middle_emulation_state
libinput_device_config_middle_emulation_get_enabled(
	struct libinput_device *device)
{
	return LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED;
}

LIBINPUT_EXPORT enum libinput_config_middle_emulation_state
libinput_device_config_middle_emulation_get_default_enabled(
	struct libinput_device *device)
{
	return LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED;
}

LIBINPUT_EXPORT uint32_t
libinput_device_config_scroll_get_methods(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_config_status
libinput_device_config_scroll_set_method(struct libinput_device *device,
	enum libinput_config_scroll_method method)
{
	/* Check method is a single valid method */
	switch (method) {
	case LIBINPUT_CONFIG_SCROLL_NO_SCROLL:
	case LIBINPUT_CONFIG_SCROLL_2FG:
	case LIBINPUT_CONFIG_SCROLL_EDGE:
	case LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN:
		break;
	default:
		return LIBINPUT_CONFIG_STATUS_INVALID;
	}

	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}

LIBINPUT_EXPORT enum libinput_config_scroll_method
libinput_device_config_scroll_get_method(struct libinput_device *device)
{
	return LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
}

LIBINPUT_EXPORT enum libinput_config_scroll_method
libinput_device_config_scroll_get_default_method(struct libinput_device *device)
{
	return LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
}

LIBINPUT_EXPORT enum libinput_config_status
libinput_device_config_scroll_set_button(struct libinput_device *device,
	uint32_t button)
{
	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}

LIBINPUT_EXPORT uint32_t
libinput_device_config_scroll_get_button(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT uint32_t
libinput_device_config_scroll_get_default_button(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT int
libinput_device_config_dwt_is_available(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_config_status
libinput_device_config_dwt_set_enabled(struct libinput_device *device,
	enum libinput_config_dwt_state enable)
{
	if (enable != LIBINPUT_CONFIG_DWT_ENABLED &&
	    enable != LIBINPUT_CONFIG_DWT_DISABLED)
		return LIBINPUT_CONFIG_STATUS_INVALID;

	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}

LIBINPUT_EXPORT enum libinput_config_dwt_state
libinput_device_config_dwt_get_enabled(struct libinput_device *device)
{
	return LIBINPUT_CONFIG_DWT_DISABLED;
}

LIBINPUT_EXPORT enum libinput_config_dwt_state
libinput_device_config_dwt_get_default_enabled(struct libinput_device *device)
{
	return LIBINPUT_CONFIG_DWT_DISABLED;
}

LIBINPUT_EXPORT int
libinput_device_tablet_pad_get_num_buttons(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT int
libinput_device_tablet_pad_get_num_rings(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT int
libinput_device_tablet_pad_get_num_strips(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT int
libinput_device_tablet_pad_get_num_mode_groups(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT struct libinput_tablet_pad_mode_group*
libinput_device_tablet_pad_get_mode_group(struct libinput_device *device,
					  unsigned int index)
{
	return NULL;
}

LIBINPUT_EXPORT unsigned int
libinput_tablet_pad_mode_group_get_mode(struct libinput_tablet_pad_mode_group *group)
{
	return 0;
}


LIBINPUT_EXPORT uint64_t
libinput_tablet_tool_get_serial(struct libinput_tablet_tool *tool)
{
	return 0;
}
LIBINPUT_EXPORT enum libinput_tablet_tool_type
libinput_tablet_tool_get_type(struct libinput_tablet_tool *tool)
{
	return 0;
}

LIBINPUT_EXPORT uint64_t
libinput_tablet_tool_get_tool_id(struct libinput_tablet_tool *tool)
{
	return 0;
}

LIBINPUT_EXPORT int
libinput_tablet_tool_has_pressure(struct libinput_tablet_tool *tool)
{
	return 0;
}

LIBINPUT_EXPORT int
libinput_tablet_tool_has_distance(struct libinput_tablet_tool *tool)
{
	return 0;
}

LIBINPUT_EXPORT int
libinput_tablet_tool_has_tilt(struct libinput_tablet_tool *tool)
{
	return 0;
}

LIBINPUT_EXPORT int
libinput_tablet_tool_has_rotation(struct libinput_tablet_tool *tool)
{
	return 0;
}

LIBINPUT_EXPORT int
libinput_tablet_tool_has_slider(struct libinput_tablet_tool *tool)
{
	return 0;
}

LIBINPUT_EXPORT int
libinput_tablet_tool_has_wheel(struct libinput_tablet_tool *tool)
{
	return 0;
}

LIBINPUT_EXPORT double
libinput_event_tablet_pad_get_ring_position(struct libinput_event_tablet_pad *event)
{
	return 0;
}

LIBINPUT_EXPORT unsigned int
libinput_event_tablet_pad_get_ring_number(struct libinput_event_tablet_pad *event)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_tablet_pad_ring_axis_source
libinput_event_tablet_pad_get_ring_source(struct libinput_event_tablet_pad *event)
{
	return LIBINPUT_TABLET_PAD_RING_SOURCE_UNKNOWN;
}

LIBINPUT_EXPORT double
libinput_event_tablet_tool_get_pressure(struct libinput_event_tablet_tool *event)
{
	return 0;
}

LIBINPUT_EXPORT double
libinput_event_tablet_tool_get_distance(struct libinput_event_tablet_tool *event)
{
	return 0;
}

LIBINPUT_EXPORT double
libinput_event_tablet_tool_get_tilt_x(struct libinput_event_tablet_tool *event)
{
	return 0;
}

LIBINPUT_EXPORT double
libinput_event_tablet_tool_get_tilt_y(struct libinput_event_tablet_tool *event)
{
	return 0;
}

LIBINPUT_EXPORT double
libinput_event_tablet_tool_get_rotation(struct libinput_event_tablet_tool *event)
{
	return 0;
}

LIBINPUT_EXPORT double
libinput_event_tablet_tool_get_slider_position(struct libinput_event_tablet_tool *event)
{
	return 0;
}

LIBINPUT_EXPORT double
libinput_event_tablet_tool_get_size_major(struct libinput_event_tablet_tool *event)
{
	return 0;
}

LIBINPUT_EXPORT double
libinput_event_tablet_tool_get_size_minor(struct libinput_event_tablet_tool *event)
{
	return 0;
}

LIBINPUT_EXPORT double
libinput_event_tablet_tool_get_wheel_delta(struct libinput_event_tablet_tool *event)
{
	return 0;
}

LIBINPUT_EXPORT int
libinput_event_tablet_tool_get_wheel_delta_discrete(
				      struct libinput_event_tablet_tool *event)
{
	return 0;
}

LIBINPUT_EXPORT double
libinput_event_tablet_tool_get_x_transformed(struct libinput_event_tablet_tool *event,
					uint32_t width)
{
	return 0;
}

LIBINPUT_EXPORT double
libinput_event_tablet_tool_get_y_transformed(struct libinput_event_tablet_tool *event,
					uint32_t height)
{
	return 0;
}

LIBINPUT_EXPORT struct libinput_tablet_tool *
libinput_event_tablet_tool_get_tool(struct libinput_event_tablet_tool *event)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_tablet_tool_tip_state
libinput_event_tablet_tool_get_tip_state(struct libinput_event_tablet_tool *event)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_tablet_tool_proximity_state
libinput_event_tablet_tool_get_proximity_state(struct libinput_event_tablet_tool *event)
{
	return 0;
}
LIBINPUT_EXPORT uint32_t
libinput_event_tablet_tool_get_time(struct libinput_event_tablet_tool *event)
{
	return 0;
}
LIBINPUT_EXPORT uint32_t
libinput_event_tablet_tool_get_button(struct libinput_event_tablet_tool *event)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_button_state
libinput_event_tablet_tool_get_button_state(struct libinput_event_tablet_tool *event)
{
	return 0;
}
LIBINPUT_EXPORT uint32_t
libinput_event_tablet_pad_get_button_number(struct libinput_event_tablet_pad *event)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_button_state
libinput_event_tablet_pad_get_button_state(struct libinput_event_tablet_pad *event)
{
	return 0;
}

LIBINPUT_EXPORT uint32_t
libinput_event_tablet_pad_get_key(struct libinput_event_tablet_pad *event)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_key_state
libinput_event_tablet_pad_get_key_state(struct libinput_event_tablet_pad *event)
{
	return 0;
}

LIBINPUT_EXPORT unsigned int
libinput_event_tablet_pad_get_mode(struct libinput_event_tablet_pad *event)
{
	return 0;
}

LIBINPUT_EXPORT struct libinput_tablet_pad_mode_group *
libinput_event_tablet_pad_get_mode_group(struct libinput_event_tablet_pad *event)
{
	return 0;
}

LIBINPUT_EXPORT uint32_t
libinput_event_tablet_pad_get_time(struct libinput_event_tablet_pad *event)
{
	return 0;
}

LIBINPUT_EXPORT uint64_t
libinput_event_tablet_pad_get_time_usec(struct libinput_event_tablet_pad *event)
{
	return 0;
}

LIBINPUT_EXPORT struct libinput_event *
libinput_event_tablet_pad_get_base_event(struct libinput_event_tablet_pad *event)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_config_status
libinput_device_config_accel_set_profile(struct libinput_device *device,
					 enum libinput_config_accel_profile profile)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_config_status
libinput_device_config_tap_set_drag_enabled(struct libinput_device *device,
					    enum libinput_config_drag_state enable)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_config_status
libinput_device_config_tap_set_button_map(struct libinput_device *device,
					    enum libinput_config_tap_button_map map)
{
	return 0;
}

LIBINPUT_EXPORT int
libinput_device_switch_has_switch(struct libinput_device *device,
				  enum libinput_switch sw)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_config_tap_button_map
libinput_device_config_tap_get_default_button_map(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_config_tap_button_map
libinput_device_config_tap_get_button_map(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_config_drag_state
libinput_device_config_tap_get_drag_enabled(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_config_accel_profile
libinput_device_config_accel_get_profile(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_config_scroll_button_lock_state
libinput_device_config_scroll_get_button_lock(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT int
libinput_device_config_dwtp_is_available(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_config_dwtp_state
libinput_device_config_dwtp_get_enabled(struct libinput_device *device)
{
	return LIBINPUT_CONFIG_DWTP_DISABLED;
}

LIBINPUT_EXPORT int
libinput_device_config_rotation_is_available(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT unsigned int
libinput_device_config_rotation_get_angle(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_config_status
libinput_device_config_rotation_set_angle(struct libinput_device *device,
    unsigned int  degrees)
{
	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}
LIBINPUT_EXPORT enum libinput_config_status
libinput_device_config_scroll_set_button_lock(struct libinput_device *device,
    enum libinput_config_scroll_button_lock_state state)
{
	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}

LIBINPUT_EXPORT enum libinput_config_status
libinput_device_config_dwtp_set_enabled(struct libinput_device *device,
    enum libinput_config_dwtp_state enable)
{
	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}

LIBINPUT_EXPORT enum libinput_config_drag_state
libinput_device_config_tap_get_default_drag_enabled(struct libinput_device *device)
{
	return LIBINPUT_CONFIG_DRAG_DISABLED;
}

LIBINPUT_EXPORT unsigned int
libinput_device_config_rotation_get_default_angle(struct libinput_device *device)
{
	return 0;
}

LIBINPUT_EXPORT enum libinput_config_accel_profile
libinput_device_config_accel_get_default_profile(struct libinput_device *device)
{
	return LIBINPUT_CONFIG_ACCEL_PROFILE_NONE;
}

LIBINPUT_EXPORT enum libinput_config_dwtp_state
libinput_device_config_dwtp_get_default_enabled(struct libinput_device *device)
{
	return LIBINPUT_CONFIG_DWTP_DISABLED;
}


