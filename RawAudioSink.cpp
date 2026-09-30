#include "RawAudioSink.h"

extern "C" {
#include "mmf2_module.h"
}

struct RawSinkContext {
    RawAudioCallback callback;
};

static void *rawCreate(void *)
{
    return calloc(1, sizeof(RawSinkContext));
}

static void *rawDestroy(void *context)
{
    free(context);
    return NULL;
}

static int rawControl(void *context, int command, int argument)
{
    RawSinkContext *sink = (RawSinkContext *)context;
    if (command == MM_MODULE_CMD(0)) {
        sink->callback = (RawAudioCallback)argument;
        return 0;
    }
    return -1;
}

static int rawHandle(void *context, void *input, void *)
{
    RawSinkContext *sink = (RawSinkContext *)context;
    mm_queue_item_t *item = (mm_queue_item_t *)input;
    if (sink && sink->callback && item && item->data_addr && item->size >= sizeof(int16_t)) {
        sink->callback((const int16_t *)item->data_addr, item->size / sizeof(int16_t));
    }
    return 0;
}

static mm_module_t rawSinkModule = {
    .create = rawCreate,
    .destroy = rawDestroy,
    .control = rawControl,
    .handle = rawHandle,
    .new_item = NULL,
    .del_item = NULL,
    .rsz_item = NULL,
    .vrelease_item = NULL,
    .output_type = MM_TYPE_NONE,
    .module_type = MM_TYPE_ASINK,
    .name = "RAW_PCM"
};

RawAudioSink::RawAudioSink() {}

RawAudioSink::~RawAudioSink()
{
    end();
}

bool RawAudioSink::begin(RawAudioCallback callback)
{
    if (_p_mmf_context == NULL) _p_mmf_context = mm_module_open(&rawSinkModule);
    if (_p_mmf_context == NULL) return false;
    return mm_module_ctrl(_p_mmf_context, MM_MODULE_CMD(0), (int)callback) == 0;
}

void RawAudioSink::end()
{
    if (_p_mmf_context != NULL) {
        mm_module_close(_p_mmf_context);
        _p_mmf_context = NULL;
    }
}
