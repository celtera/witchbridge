#pragma once
#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <string.h>
#include <libsoup/soup.h>

struct ReceiverEntry
{
  SoupWebsocketConnection* connection = nullptr;

  GstElement* pipeline = nullptr;
  GstElement* sound_in = nullptr;
  GstElement* webrtcbin = nullptr;
  guint sourceid = 0;
  guint64 num_samples;   /* Number of samples generated so far (for timestamp generation) */
};
struct CustomData;
gboolean push_data (ReceiverEntry *data);
void start_feed (GstElement *source, guint size, ReceiverEntry *data) ;
void stop_feed (GstElement *source, ReceiverEntry *data) ;
GstFlowReturn new_sample (GstElement *sink, ReceiverEntry *data) ;
