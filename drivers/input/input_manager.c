#include "input.h"
#include "apic.h"
#include "log.h"

static input_event_t g_events[INPUT_QUEUE_SIZE];
static volatile uint32_t g_head = 0;
static volatile uint32_t g_tail = 0;
static volatile uint64_t g_event_count = 0;
static volatile uint64_t g_dropped_count = 0;
static uint64_t g_logged_events = 0;

static inline uint64_t input_irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void input_irq_restore(uint64_t flags) {
    if (flags & (1ULL << 9)) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

void input_manager_init(void) {
    g_head = 0;
    g_tail = 0;
    g_event_count = 0;
    g_dropped_count = 0;
    g_logged_events = 0;
    log_printf("[INPUT] Unified input event queue initialized (capacity=%u).\n",
               INPUT_QUEUE_SIZE);
}

bool input_push_event(const input_event_t* event) {
    if (!event) return false;
    uint64_t flags = input_irq_save();
    uint32_t next = (g_head + 1u) % INPUT_QUEUE_SIZE;
    if (next == g_tail) {
        g_dropped_count++;
        input_irq_restore(flags);
        return false;
    }

    g_events[g_head] = *event;
    g_events[g_head].timestamp = apic_get_ticks();
    g_head = next;
    g_event_count++;
    input_irq_restore(flags);
    return true;
}

bool input_poll_event(input_event_t* event) {
    if (!event) return false;
    uint64_t flags = input_irq_save();
    if (g_head == g_tail) {
        input_irq_restore(flags);
        return false;
    }

    *event = g_events[g_tail];
    g_tail = (g_tail + 1u) % INPUT_QUEUE_SIZE;
    input_irq_restore(flags);
    return true;
}

size_t input_get_queue_depth(void) {
    uint32_t head = g_head;
    uint32_t tail = g_tail;
    return (size_t)((head + INPUT_QUEUE_SIZE - tail) % INPUT_QUEUE_SIZE);
}

uint64_t input_get_event_count(void) {
    return g_event_count;
}

uint64_t input_get_dropped_count(void) {
    return g_dropped_count;
}

static void input_log_event(const input_event_t* event) {
    if (g_logged_events >= 64) return;
    g_logged_events++;
    switch (event->type) {
        case INPUT_KEY_DOWN:
            if (event->code >= 0x20 && event->code <= 0x7E) {
                log_printf("[INPUT] KEY_DOWN code=0x%x '%c'\n",
                           event->code, (char)event->code);
            } else {
                log_printf("[INPUT] KEY_DOWN code=0x%x\n", event->code);
            }
            break;
        case INPUT_KEY_UP:
            log_printf("[INPUT] KEY_UP code=0x%x\n", event->code);
            break;
        case INPUT_MOUSE_MOVE:
            log_printf("[INPUT] MOUSE_MOVE dx=%d dy=%d buttons=0x%x\n",
                       event->dx, event->dy, event->buttons);
            break;
        case INPUT_MOUSE_BUTTON:
            log_printf("[INPUT] MOUSE_BUTTON buttons=0x%x\n",
                       event->buttons);
            break;
        case INPUT_MOUSE_SCROLL:
            log_printf("[INPUT] MOUSE_SCROLL wheel=%d\n", event->wheel);
            break;
    }
}

void input_emit_key(uint32_t code, bool pressed) {
    input_event_t event = {
        .type = pressed ? INPUT_KEY_DOWN : INPUT_KEY_UP,
        .code = code
    };
    if (input_push_event(&event)) input_log_event(&event);
}

void input_emit_mouse_move(int32_t dx, int32_t dy,
                           uint32_t buttons, int32_t wheel) {
    input_event_t move = {
        .type = INPUT_MOUSE_MOVE,
        .dx = dx,
        .dy = dy,
        .buttons = buttons
    };
    if (input_push_event(&move)) input_log_event(&move);

    if (wheel != 0) {
        input_event_t scroll = {
            .type = INPUT_MOUSE_SCROLL,
            .buttons = buttons,
            .wheel = wheel
        };
        if (input_push_event(&scroll)) input_log_event(&scroll);
    }
}

void input_emit_mouse_button(uint32_t buttons) {
    input_event_t event = {
        .type = INPUT_MOUSE_BUTTON,
        .buttons = buttons
    };
    if (input_push_event(&event)) input_log_event(&event);
}
