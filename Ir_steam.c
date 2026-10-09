// IR Stream Recorder: records many raw IR signals in a row (with the
// pauses between them) and replays the whole sequence.
// OK = start/stop recording, Right = play, Left = clear, Back = exit.

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <infrared_worker.h>
#include <infrared_transmit.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SIGNALS 64
#define IR_FREQ     38000
#define IR_DUTY     0.33f

typedef struct {
    uint32_t* timings;
    size_t count;
    uint32_t gap_ms; // pause before this signal
} Sig;

typedef enum {
    ModeIdle,
    ModeRecording,
    ModePlaying
} Mode;

typedef struct {
    Gui* gui;
    ViewPort* vp;
    FuriMessageQueue* queue;
    InfraredWorker* worker;
    Sig sigs[MAX_SIGNALS];
    size_t n;
    uint32_t last_tick;
    Mode mode;
} App;

static void clear_all(App* app) {
    for(size_t i = 0; i < app->n; i++) {
        free(app->sigs[i].timings);
        app->sigs[i].timings = NULL;
    }
    app->n = 0;
}

static void draw_cb(Canvas* canvas, void* ctx) {
    App* app = ctx;
    char buf[32];
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, "IR Stream Recorder");
    canvas_set_font(canvas, FontSecondary);
    const char* state = app->mode == ModeRecording ? "RECORDING..." :
                        app->mode == ModePlaying   ? "PLAYING..." :
                                                     "Idle";
    canvas_draw_str(canvas, 2, 26, state);
    snprintf(buf, sizeof(buf), "Signals: %u / %d", (unsigned)app->n, MAX_SIGNALS);
    canvas_draw_str(canvas, 2, 38, buf);
    canvas_draw_str(canvas, 2, 50, "OK rec/stop  Right play");
    canvas_draw_str(canvas, 2, 61, "Left clear   Back exit");
}

static void input_cb(InputEvent* event, void* ctx) {
    App* app = ctx;
    furi_message_queue_put(app->queue, event, 0);
}

static void rx_cb(void* ctx, InfraredWorkerSignal* signal) {
    App* app = ctx;
    if(app->mode != ModeRecording || app->n >= MAX_SIGNALS) return;

    const uint32_t* t;
    size_t c;
    infrared_worker_get_raw_signal(signal, &t, &c);
    if(c == 0) return;

    uint32_t* copy = malloc(c * sizeof(uint32_t));
    if(!copy) return;
    memcpy(copy, t, c * sizeof(uint32_t));

    // This callback fires after the line goes quiet, so the time between
    // callbacks = gap + signal length. Subtract the length to get the gap.
    uint32_t now = furi_get_tick();
    uint32_t dur_ms = 0;
    for(size_t i = 0; i < c; i++) dur_ms += t[i];
    dur_ms /= 1000;
    uint32_t delta = (app->n == 0) ? 0 : now - app->last_tick;
    app->last_tick = now;

    Sig* s = &app->sigs[app->n++];
    s->timings = copy;
    s->count = c;
    s->gap_ms = (delta > dur_ms) ? (delta - dur_ms) : 0;

    view_port_update(app->vp);
}

static void start_rec(App* app) {
    clear_all(app);
    app->mode = ModeRecording;
    infrared_worker_rx_enable_signal_decoding(app->worker, false); // raw only
    infrared_worker_rx_set_received_signal_callback(app->worker, rx_cb, app);
    infrared_worker_rx_start(app->worker);
}

static void stop_rec(App* app) {
    infrared_worker_rx_stop(app->worker);
    app->mode = ModeIdle;
}

static void play(App* app) {
    if(app->n == 0) return;
    app->mode = ModePlaying;
    view_port_update(app->vp);
    for(size_t i = 0; i < app->n; i++) {
        Sig* s = &app->sigs[i];
        if(s->gap_ms) furi_delay_ms(s->gap_ms);
        infrared_send_raw_ext(s->timings, s->count, false, IR_FREQ, IR_DUTY);
    }
    app->mode = ModeIdle;
}

int32_t ir_stream_main(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->worker = infrared_worker_alloc();
    app->vp = view_port_alloc();
    view_port_draw_callback_set(app->vp, draw_cb, app);
    view_port_input_callback_set(app->vp, input_cb, app);
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->vp, GuiLayerFullscreen);

    InputEvent ev;
    bool run = true;
    while(run) {
        if(furi_message_queue_get(app->queue, &ev, 100) == FuriStatusOk &&
           ev.type == InputTypeShort) {
            switch(ev.key) {
            case InputKeyOk:
                if(app->mode == ModeRecording) stop_rec(app);
                else start_rec(app);
                break;
            case InputKeyRight:
                if(app->mode == ModeRecording) stop_rec(app);
                play(app);
                break;
            case InputKeyLeft:
                if(app->mode == ModeIdle) clear_all(app);
                break;
            case InputKeyBack:
                run = false;
                break;
            default:
                break;
            }
            view_port_update(app->vp);
        }
    }

    if(app->mode == ModeRecording) stop_rec(app);
    gui_remove_view_port(app->gui, app->vp);
    furi_record_close(RECORD_GUI);
    view_port_free(app->vp);
    infrared_worker_free(app->worker);
    furi_message_queue_free(app->queue);
    clear_all(app);
    free(app);
    return 0;
}
