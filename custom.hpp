#pragma once
#include <cstdint>

struct _GstElement;
typedef struct _GstElement GstElement;
struct _SoupWebsocketConnection;
typedef struct _SoupWebsocketConnection SoupWebsocketConnection;

struct ReceiverEntry
{
  SoupWebsocketConnection* connection = nullptr;

  GstElement* pipeline = nullptr;
  GstElement* sound_in = nullptr;
  GstElement* webrtcbin = nullptr;
  uint32_t sourceid = 0;
  uint64_t num_samples = 0;
};

bool push_data(ReceiverEntry *data);
void start_feed(GstElement *source, uint32_t size, ReceiverEntry *data) ;
void stop_feed(GstElement *source, ReceiverEntry *data) ;
