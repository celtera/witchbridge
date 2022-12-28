
#include "custom.hpp"
#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/pool/object_pool.hpp>

#include <cmath>
#include <glib.h>
#include <gst/app/gstappsrc.h>
#include <gst/audio/audio.h>
#include <gst/video/video.h>

#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <locale.h>

#ifdef G_OS_UNIX
#include <glib-unix.h>
#endif

#define GST_USE_UNSTABLE_API
#include <json-glib/json-glib.h>

#include <gst/webrtc/webrtc.h>
#include <libsoup/soup.h>
#include <string.h>

#define RTP_PAYLOAD_TYPE "96"
#define RTP_AUDIO_PAYLOAD_TYPE "97"
#define SOUP_HTTP_PORT 57778
#define STUN_SERVER "stun.l.google.com:19302"

#ifdef G_OS_WIN32
#define VIDEO_SRC "mfvideosrc"
#else
#define VIDEO_SRC "videotestsrc is-live=1 "
#endif

#include <iostream>
#include <thread>

#include <rigtorp/SPSCQueue.h>
#include <boost/circular_buffer.hpp>


static constexpr int max_buffer = 4;

struct audio_buffer
{
  float* audio[2];
  int channels;
  int frames;
};

struct audio_frame
{
  float sample[2];
};

struct video_buffer
{
  unsigned char* bytes;
  int width, height;
};

const gchar* video_priority = "low";
const gchar* audio_priority = "high";

struct _GstElement;
typedef struct _GstElement GstElement;
struct _SoupWebsocketConnection;
typedef struct _SoupWebsocketConnection SoupWebsocketConnection;

#define CHUNK_SIZE 1024*4   /* Amount of bytes we are sending in each buffer */
struct Streamer;
struct ReceiverEntry
{
  Streamer* self = nullptr;
  SoupWebsocketConnection* connection = nullptr;

  GstElement* pipeline = nullptr;
  GstElement* sound_in = nullptr;
  GstElement* video_in = nullptr;
  GstElement* webrtcbin = nullptr;
  uint32_t sourceid = 0;
  uint64_t num_samples = 0;
  uint64_t num_frames = 0;

  int64_t audio_feed{};
  int64_t video_feed{};

  boost::circular_buffer<audio_frame> buf = boost::circular_buffer<audio_frame>(128 * CHUNK_SIZE);
  audio_frame next_frame() noexcept;

  bool push_data_audio(audio_buffer buf);
  bool push_data_video(video_buffer buf);
};

//// Audio


struct Streamer
{
  config conf;

  static bool push_data_audio(ReceiverEntry* data)
  {
    const gint num_samples = CHUNK_SIZE / 4;
    GstBuffer* buffer = gst_buffer_new_and_alloc(CHUNK_SIZE);

    GST_BUFFER_TIMESTAMP(buffer)
        = gst_util_uint64_scale(data->num_samples, GST_SECOND, data->self->conf.rate);
    GST_BUFFER_DURATION(buffer)
        = gst_util_uint64_scale(num_samples, GST_SECOND, data->self->conf.rate);

    GstMapInfo map{};

    gst_buffer_map(buffer, &map, GST_MAP_WRITE);

    auto raw = (float*)map.data;

    for (int i = 0; i < num_samples; i++)
    {
      auto frame = data->next_frame();
      raw[i] = frame.sample[0];
    }
    data->num_samples += num_samples;

    gst_buffer_unmap(buffer, &map);

    GstFlowReturn ret{};
    g_signal_emit_by_name(data->sound_in, "push-buffer", buffer, &ret);
    gst_buffer_unref(buffer);

    return ret == GST_FLOW_OK;
  }


  static void start_feed_audio(GstElement* source, guint size, ReceiverEntry* data)
  {
    data->audio_feed++;
    /*
    if (data->sourceid == 0)
    {
      g_print("Start feeding\n");
      data->sourceid = g_idle_add((GSourceFunc)push_data_audio, data);
    }*/
  }

  static void stop_feed_audio(GstElement* source, ReceiverEntry* data)
  {/*
    if (data->sourceid != 0)
    {
      g_print("Stop feeding\n");
      g_source_remove(data->sourceid);
      data->sourceid = 0;
    }*/
  }

  static void start_feed_video(GstElement* source, guint size, ReceiverEntry* data)
  {
    data->video_feed++;
    /*
    if (data->sourceid == 0)
    {
      g_print("Start feeding\n");
      data->sourceid = g_idle_add((GSourceFunc)push_data_audio, data);
    }*/
  }

  static void stop_feed_video(GstElement* source, ReceiverEntry* data)
  {/*
    if (data->sourceid != 0)
    {
      g_print("Stop feeding\n");
      g_source_remove(data->sourceid);
      data->sourceid = 0;
    }*/
  }

  static gboolean
  bus_watch_cb(GstBus* bus, GstMessage* message, gpointer user_data)
  {
    switch (GST_MESSAGE_TYPE(message))
    {
    case GST_MESSAGE_ERROR:
    {
      GError* error = nullptr;
      gchar* debug = nullptr;

      gst_message_parse_error(message, &error, &debug);
      g_error("Error on bus: %s (debug: %s)", error->message, debug);
      exit(1);
      g_error_free(error);
      g_free(debug);
      break;
    }
    case GST_MESSAGE_WARNING:
    {
      GError* error = nullptr;
      gchar* debug = nullptr;

      gst_message_parse_warning(message, &error, &debug);
      g_warning("Warning on bus: %s (debug: %s)", error->message, debug);
      g_error_free(error);
      g_free(debug);
      break;
    }
    default:
      break;
    }

    return G_SOURCE_CONTINUE;
  }

  static GstWebRTCPriorityType _priority_from_string(const gchar* s)
  {
    return GST_WEBRTC_PRIORITY_TYPE_MEDIUM;

    GEnumClass* klass
        = (GEnumClass*)g_type_class_ref(GST_TYPE_WEBRTC_PRIORITY_TYPE);
    GEnumValue* en;

    g_return_val_if_fail(klass, GstWebRTCPriorityType{});
    if (!(en = g_enum_get_value_by_name(klass, s)))
      en = g_enum_get_value_by_nick(klass, s);
    g_type_class_unref(klass);

    if (en)
      return (GstWebRTCPriorityType)en->value;

    return GstWebRTCPriorityType{};
  }
  static std::shared_ptr<ReceiverEntry>
  create_receiver_entry(SoupWebsocketConnection* connection, Streamer& self)
  {
    auto receiver_entry = std::make_shared<ReceiverEntry>();
    receiver_entry->self = &self;
    receiver_entry->connection = connection;

    g_object_ref(G_OBJECT(connection));

    g_signal_connect(
          G_OBJECT(connection),
          "message",
          G_CALLBACK(soup_websocket_message_cb),
          (gpointer)receiver_entry.get());

    GError* error = nullptr;
    std::string pipeline_web
        = "webrtcbin latency=1 name=webrtcbin stun-server=stun://" STUN_SERVER;
    std::string pipeline_video
        = "   appsrc is-live=1 name=myvid leaky-type=2 min-latency=0  "
          " ! videorate "
          " ! videoscale "
          " ! video/x-raw,width=1280,height=720,framerate=60/1 "
          " ! videoconvert "
          " ! queue max-size-buffers=1 "
          " ! x264enc bitrate=2400 speed-preset=medium tune=zerolatency key-int-max=15 "
          " ! video/x-h264,profile=constrained-baseline "
          " ! queue max-size-time=100 "
          " ! h264parse "
          " ! rtph264pay config-interval=-1 name=payloader aggregate-mode=zero-latency "
          " ! application/x-rtp,media=video,encoding-name=H264,payload=96 "
          " ! webrtcbin. ";

    std::string pipeline_audio
        = " appsrc is-live=1 name=mysound leaky-type=2 min-latency=0 ! "
          "audioconvert ! audioresample ! "
          "opusenc audio-type=restricted-lowdelay bandwidth=fullband bitrate=128000 frame-size=2.5 ! "
          "rtpopuspay pt=97 ! webrtcbin. ";

    receiver_entry->pipeline = gst_parse_launch(
                                 (pipeline_web + pipeline_video + pipeline_audio).c_str(), &error);
    if (error != nullptr)
    {
      g_error("Could not create WebRTC pipeline: %s\n", error->message);
      g_error_free(error);
      return {};
    }

    // Setup the sound source
    {
      receiver_entry->sound_in
          = gst_bin_get_by_name(GST_BIN(receiver_entry->pipeline), "mysound");
      g_assert(receiver_entry->sound_in);

      GstAudioInfo info;
      gst_audio_info_set_format(
            &info, GST_AUDIO_FORMAT_F32, self.conf.rate, 1, nullptr);
      GstCaps* audio_caps;
      audio_caps = gst_audio_info_to_caps(&info);
      g_object_set(
            receiver_entry->sound_in,
            "caps",
            audio_caps,
            "format",
            GST_FORMAT_TIME,
            nullptr);
      g_signal_connect(
            receiver_entry->sound_in,
            "need-data",
            G_CALLBACK(start_feed_audio),
            receiver_entry.get());
      g_signal_connect(
            receiver_entry->sound_in,
            "enough-data",
            G_CALLBACK(stop_feed_audio),
            receiver_entry.get());

      // for(int i = 0; i < 16; i++) {
      //   push_data_audio(receiver_entry);
      // }
    }

    // Setup the video source
    {
      receiver_entry->video_in
          = gst_bin_get_by_name(GST_BIN(receiver_entry->pipeline), "myvid");
      GstVideoInfo info;
      gst_video_info_set_format(&info, GST_VIDEO_FORMAT_RGBA, 1280, 720);
      GstCaps* video_caps = gst_video_info_to_caps(&info);

      g_object_set(
          receiver_entry->video_in,
          "caps",
          video_caps,
          "format",
          GST_FORMAT_TIME,
          nullptr);

      g_signal_connect(
          receiver_entry->video_in,
          "need-data",
          G_CALLBACK(start_feed_video),
          receiver_entry.get());

      g_signal_connect(
          receiver_entry->video_in,
          "enough-data",
          G_CALLBACK(stop_feed_video),
          receiver_entry.get());

    }

    {
      receiver_entry->webrtcbin
          = gst_bin_get_by_name(GST_BIN(receiver_entry->pipeline), "webrtcbin");
      g_assert(receiver_entry->webrtcbin != nullptr);

      // Setup the webrtc internal latency
      {
        auto rtpbin = gst_bin_get_by_name(GST_BIN(receiver_entry->webrtcbin), "rtpbin");
        g_assert_nonnull (rtpbin);
        g_object_set(rtpbin, "latency", 10, nullptr);
        // g_object_set(rtpbin, "sync", false, nullptr);
        // g_object_set(rtpbin, "async", false, nullptr);
        g_object_unref(rtpbin);
      }

      // Setup transceivers
      GArray* transceivers{};
      g_signal_emit_by_name(
            receiver_entry->webrtcbin, "get-transceivers", &transceivers);
      g_assert(transceivers != nullptr && transceivers->len > 1);
      auto trans = g_array_index(transceivers, GstWebRTCRTPTransceiver*, 0);
      g_object_set(
            trans,
            "direction",
            GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY,
            nullptr);
      if (video_priority)
      {
        GstWebRTCPriorityType priority;

        priority = _priority_from_string(video_priority);
        if (priority)
        {
          GstWebRTCRTPSender* sender;

          g_object_get(trans, "sender", &sender, nullptr);
          gst_webrtc_rtp_sender_set_priority(sender, priority);
          g_object_unref(sender);
        }
      }
      trans = g_array_index(transceivers, GstWebRTCRTPTransceiver*, 1);
      g_object_set(
            trans,
            "direction",
            GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY,
            nullptr);
      if (audio_priority)
      {
        GstWebRTCPriorityType priority;

        priority = _priority_from_string(audio_priority);
        if (priority)
        {
          GstWebRTCRTPSender* sender;

          g_object_get(trans, "sender", &sender, nullptr);
          gst_webrtc_rtp_sender_set_priority(sender, priority);
          g_object_unref(sender);
        }
      }
      g_array_unref(transceivers);

      g_signal_connect(
            receiver_entry->webrtcbin,
            "on-negotiation-needed",
            G_CALLBACK(on_negotiation_needed_cb),
            (gpointer)receiver_entry.get());

      g_signal_connect(
            receiver_entry->webrtcbin,
            "on-ice-candidate",
            G_CALLBACK(on_ice_candidate_cb),
            (gpointer)receiver_entry.get());

      GstBus* bus;
      bus = gst_pipeline_get_bus(GST_PIPELINE(receiver_entry->pipeline));
      gst_bus_add_watch(bus, bus_watch_cb, nullptr);
      gst_object_unref(bus);
    }

    if (gst_element_set_state(receiver_entry->pipeline, GST_STATE_PLAYING)
        == GST_STATE_CHANGE_FAILURE)
      g_error("Could not start pipeline");

    return receiver_entry;
  }
  static void destroy_receiver_entry(gpointer receiver_entry_ptr)
  {
    ReceiverEntry* receiver_entry = (ReceiverEntry*)receiver_entry_ptr;

    g_assert(receiver_entry != nullptr);

    if (receiver_entry->pipeline != nullptr)
    {
      gst_element_set_state(
            GST_ELEMENT(receiver_entry->pipeline), GST_STATE_NULL);

      gst_object_unref(GST_OBJECT(receiver_entry->webrtcbin));
      gst_object_unref(GST_OBJECT(receiver_entry->pipeline));
    }

    if (receiver_entry->connection != nullptr)
      g_object_unref(G_OBJECT(receiver_entry->connection));
  }
  static void on_offer_created_cb(GstPromise* promise, gpointer user_data)
  {
    gchar* sdp_string;
    gchar* json_string;
    JsonObject* sdp_json;
    JsonObject* sdp_data_json;
    GstStructure const* reply;
    GstPromise* local_desc_promise;
    GstWebRTCSessionDescription* offer = nullptr;
    ReceiverEntry* receiver_entry = (ReceiverEntry*)user_data;

    reply = gst_promise_get_reply(promise);
    gst_structure_get(
          reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);
    gst_promise_unref(promise);

    local_desc_promise = gst_promise_new();
    g_signal_emit_by_name(
          receiver_entry->webrtcbin,
          "set-local-description",
          offer,
          local_desc_promise);
    gst_promise_interrupt(local_desc_promise);
    gst_promise_unref(local_desc_promise);

    sdp_string = gst_sdp_message_as_text(offer->sdp);
    gst_print("Negotiation offer created:\n%s\n", sdp_string);

    sdp_json = json_object_new();
    json_object_set_string_member(sdp_json, "type", "sdp");

    sdp_data_json = json_object_new();
    json_object_set_string_member(sdp_data_json, "type", "offer");
    json_object_set_string_member(sdp_data_json, "sdp", sdp_string);
    json_object_set_object_member(sdp_json, "data", sdp_data_json);

    json_string = get_string_from_json_object(sdp_json);
    json_object_unref(sdp_json);

    soup_websocket_connection_send_text(
          receiver_entry->connection, json_string);
    g_free(json_string);
    g_free(sdp_string);

    gst_webrtc_session_description_free(offer);
  }
  static void
  on_negotiation_needed_cb(GstElement* webrtcbin, gpointer user_data)
  {
    GstPromise* promise;
    ReceiverEntry* receiver_entry = (ReceiverEntry*)user_data;

    gst_print("Creating negotiation offer\n");

    promise = gst_promise_new_with_change_func(
                on_offer_created_cb, (gpointer)receiver_entry, nullptr);
    g_signal_emit_by_name(
          G_OBJECT(webrtcbin), "create-offer", nullptr, promise);
  }
  static void on_ice_candidate_cb(
      G_GNUC_UNUSED GstElement* webrtcbin,
      guint mline_index,
      gchar* candidate,
      gpointer user_data)
  {
    JsonObject* ice_json;
    JsonObject* ice_data_json;
    gchar* json_string;
    ReceiverEntry* receiver_entry = (ReceiverEntry*)user_data;

    ice_json = json_object_new();
    json_object_set_string_member(ice_json, "type", "ice");

    ice_data_json = json_object_new();
    json_object_set_int_member(ice_data_json, "sdpMLineIndex", mline_index);
    json_object_set_string_member(ice_data_json, "candidate", candidate);
    json_object_set_object_member(ice_json, "data", ice_data_json);

    json_string = get_string_from_json_object(ice_json);

    // std::cerr << "on_ice_candidate_cb: JSON OUTPUT: '" << json_string << "'\n";
    json_object_unref(ice_json);

    soup_websocket_connection_send_text(
          receiver_entry->connection, json_string);
    g_free(json_string);
  }

  static void soup_websocket_message_cb(
      G_GNUC_UNUSED SoupWebsocketConnection* connection,
      SoupWebsocketDataType data_type,
      GBytes* message,
      gpointer user_data)
  {
    gsize size;
    gchar* data;
    gchar* data_string;
    const gchar* type_string;
    JsonNode* root_json;
    JsonObject* root_json_object;
    JsonObject* data_json_object;
    JsonParser* json_parser = nullptr;
    ReceiverEntry* receiver_entry = (ReceiverEntry*)user_data;

    switch (data_type)
    {
    case SOUP_WEBSOCKET_DATA_BINARY:
      g_error("Received unknown binary message, ignoring\n");
      g_bytes_unref(message);
      return;

    case SOUP_WEBSOCKET_DATA_TEXT:
      data = (gchar*)g_bytes_unref_to_data(message, &size);
      /* Convert to nullptr-terminated string */
      data_string = g_strndup(data, size);
      g_free(data);
      break;

    default:
      g_assert_not_reached();
    }

    json_parser = json_parser_new();
    if (!json_parser_load_from_data(json_parser, data_string, -1, nullptr))
      goto unknown_message;

    root_json = json_parser_get_root(json_parser);
    if (!JSON_NODE_HOLDS_OBJECT(root_json))
      goto unknown_message;

    root_json_object = json_node_get_object(root_json);

    if (!json_object_has_member(root_json_object, "type"))
    {
      g_error("Received message without type field\n");
      goto cleanup;
    }
    type_string = json_object_get_string_member(root_json_object, "type");

    if (!json_object_has_member(root_json_object, "data"))
    {
      g_error("Received message without data field\n");
      goto cleanup;
    }
    data_json_object = json_object_get_object_member(root_json_object, "data");

    if (g_strcmp0(type_string, "sdp") == 0)
    {
      const gchar* sdp_type_string;
      const gchar* sdp_string;
      GstPromise* promise;
      GstSDPMessage* sdp;
      GstWebRTCSessionDescription* answer;
      int ret;

      if (!json_object_has_member(data_json_object, "type"))
      {
        g_error("Received SDP message without type field\n");
        goto cleanup;
      }
      sdp_type_string
          = json_object_get_string_member(data_json_object, "type");

      if (g_strcmp0(sdp_type_string, "answer") != 0)
      {
        g_error(
              "Expected SDP message type \"answer\", got \"%s\"\n",
              sdp_type_string);
        goto cleanup;
      }

      if (!json_object_has_member(data_json_object, "sdp"))
      {
        g_error("Received SDP message without SDP string\n");
        goto cleanup;
      }
      sdp_string = json_object_get_string_member(data_json_object, "sdp");

      gst_print("Received SDP:\n%s\n", sdp_string);

      ret = gst_sdp_message_new(&sdp);
      g_assert_cmphex(ret, ==, GST_SDP_OK);

      ret = gst_sdp_message_parse_buffer(
              (guint8*)sdp_string, strlen(sdp_string), sdp);
      if (ret != GST_SDP_OK)
      {
        g_error("Could not parse SDP string\n");
        goto cleanup;
      }

      answer = gst_webrtc_session_description_new(
                 GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
      g_assert_nonnull(answer);

      promise = gst_promise_new();
      g_signal_emit_by_name(
            receiver_entry->webrtcbin,
            "set-remote-description",
            answer,
            promise);
      gst_promise_interrupt(promise);
      gst_promise_unref(promise);
      gst_webrtc_session_description_free(answer);
    }
    else if (g_strcmp0(type_string, "ice") == 0)
    {
      guint mline_index;
      const gchar* candidate_string;

      if (!json_object_has_member(data_json_object, "sdpMLineIndex"))
      {
        g_error("Received ICE message without mline index\n");
        goto cleanup;
      }
      mline_index
          = json_object_get_int_member(data_json_object, "sdpMLineIndex");

      if (!json_object_has_member(data_json_object, "candidate"))
      {
        g_error("Received ICE message without ICE candidate string\n");
        goto cleanup;
      }
      candidate_string
          = json_object_get_string_member(data_json_object, "candidate");

      gst_print(
            "Received ICE candidate with mline index %u; candidate: %s\n",
            mline_index,
            candidate_string);

      g_signal_emit_by_name(
            receiver_entry->webrtcbin,
            "add-ice-candidate",
            mline_index,
            candidate_string);
    }
    else
      goto unknown_message;

cleanup:
    if (json_parser != nullptr)
      g_object_unref(G_OBJECT(json_parser));
    g_free(data_string);
    return;

unknown_message:
    g_error("Unknown message \"%s\", ignoring", data_string);
    goto cleanup;
  }

  static void soup_websocket_closed_cb(
      SoupWebsocketConnection* connection,
      gpointer user_data)
  {
    Streamer& self = *(Streamer*)user_data;

    for(auto it = self.receivers.begin(); it != self.receivers.end(); ) {
      if((*it)->connection == connection) {
        it = self.receivers.erase(it);
        break;
      } else {
        ++it;
      }
    }
    GHashTable* receiver_entry_table = self.receiver_entry_table;
    g_hash_table_remove(receiver_entry_table, connection);

  }
  static void soup_http_handler(
      G_GNUC_UNUSED SoupServer* soup_server,
      SoupMessage* message,
      const char* path,
      G_GNUC_UNUSED GHashTable* query,
      G_GNUC_UNUSED SoupClientContext* client_context,
      G_GNUC_UNUSED gpointer user_data)
  {
    SoupBuffer* soup_buffer;

    if ((g_strcmp0(path, "/") != 0) && (g_strcmp0(path, "/index.html") != 0))
    {
      soup_message_set_status(message, SOUP_STATUS_NOT_FOUND);
      return;
    }

    static boost::iostreams::mapped_file mmap(
          "webrtc.html",
          boost::iostreams::mapped_file::readonly);
    soup_buffer
        = soup_buffer_new(SOUP_MEMORY_STATIC, mmap.const_data(), mmap.size());

    soup_message_headers_set_content_type(
          message->response_headers, "text/html", nullptr);
    soup_message_body_append_buffer(message->response_body, soup_buffer);
    soup_buffer_free(soup_buffer);

    soup_message_set_status(message, SOUP_STATUS_OK);
  }

  static void soup_websocket_handler(
      G_GNUC_UNUSED SoupServer* server,
      SoupWebsocketConnection* connection,
      G_GNUC_UNUSED const char* path,
      G_GNUC_UNUSED SoupClientContext* client_context,
      gpointer user_data)
  {
    Streamer& self = *(Streamer*)user_data;
    GHashTable* receiver_entry_table = self.receiver_entry_table;

    gst_print("Processing new websocket connection %p", (gpointer)connection);

    g_signal_connect(
          G_OBJECT(connection),
          "closed",
          G_CALLBACK(soup_websocket_closed_cb),
          &self);

    auto receiver_entry = create_receiver_entry(connection, self);
    self.receivers.push_back(receiver_entry);
    g_hash_table_replace(receiver_entry_table, connection, receiver_entry.get());
  }

  static gchar* get_string_from_json_object(JsonObject* object)
  {
    JsonNode* root;
    JsonGenerator* generator;
    gchar* text;

    /* Make it the root node */
    root = json_node_init_object(json_node_alloc(), object);
    generator = json_generator_new();
    json_generator_set_root(generator, root);
    text = json_generator_to_data(generator, nullptr);

    /* Release everything */
    g_object_unref(generator);
    json_node_free(root);
    return text;
  }


  GMainLoop* mainloop{};
  SoupServer* soup_server{};
  GHashTable* receiver_entry_table{};

  std::jthread impl;

  void run()
  {
    GError* error = nullptr;

    setlocale(LC_ALL, "C");
    receiver_entry_table = g_hash_table_new_full(
                             g_direct_hash, g_direct_equal, nullptr, destroy_receiver_entry);

    mainloop = g_main_loop_new(nullptr, FALSE);
    g_assert(mainloop != nullptr);

    soup_server = soup_server_new(
                    SOUP_SERVER_SERVER_HEADER, "webrtc-soup-server", nullptr);
    soup_server_add_handler(
          soup_server, "/", soup_http_handler, nullptr, nullptr);
    soup_server_add_websocket_handler(
          soup_server,
          "/ws",
          nullptr,
          nullptr,
          soup_websocket_handler,
          (gpointer)this,
          nullptr);
    soup_server_listen_all(
          soup_server, SOUP_HTTP_PORT, (SoupServerListenOptions)0, nullptr);

    gst_print(
          "WebRTC page link: http://127.0.0.1:%d/\n", (gint)SOUP_HTTP_PORT);

    g_timeout_add(1, (GSourceFunc) +[] (void* data) {
      ((Streamer*)(data))->buffer_read_timeout(); }, this);

    g_main_loop_run(mainloop);

    g_object_unref(G_OBJECT(soup_server));
    g_hash_table_destroy(receiver_entry_table);
    g_main_loop_unref(mainloop);
  }

  bool buffer_read_timeout()
  {
    ready = true;
    int kmax = 20;
    while(audio_buffer* p = audio_to_send.front())
    {
      for(auto& receiver : receivers) {
        // Don't buffer on receivers that haven't even started polling

        receiver->push_data_audio(*p);
/*
        {
          for(int f = 0; f < p->frames; f++) {
            audio_frame frame;
            for(int c = 0; c < p->channels; c++) {
              frame.sample[c] = p->audio[c][f];
            }

            receiver->buf.push_back(frame);
          }
        }*/
      }

      audio_to_free.push(*p);
      audio_to_send.pop();

      if(kmax-- < 0)
        break;
    }

    kmax = 20;
    while(video_buffer* p = video_to_send.front())
    {
      for(auto& receiver : receivers) {
        receiver->push_data_video(*p);
      }

      video_to_free.push(*p);
      video_to_send.pop();

      if(kmax-- < 0)
        break;
    }
    return true;
  }

  Streamer(config c)
    : conf(c)
    , audio_to_send(64)
    , audio_to_free(64)
    , video_to_send(16)
    , video_to_free(16)
    // , storage(4096 * 16)
  {
    static bool init = (gst_init(nullptr, nullptr), true);

    impl = std::jthread{[this, c] { run(); } };
  }

  ~Streamer()
  {
    g_main_loop_quit(mainloop);
    impl.join();


    // gst_deinit();
  }

  std::vector<std::shared_ptr<ReceiverEntry>> receivers;
  // boost::pool<> storage;
  rigtorp::SPSCQueue<audio_buffer> audio_to_send;
  rigtorp::SPSCQueue<audio_buffer> audio_to_free;

  rigtorp::SPSCQueue<video_buffer> video_to_free;
  rigtorp::SPSCQueue<video_buffer> video_to_send;
  std::atomic_bool ready = false;
};

std::shared_ptr<Streamer> make_streamer(config c)
{
  static std::shared_ptr<Streamer> s = std::make_unique<Streamer>(c);
  return s;
}

void push_audio(Streamer& s, audio_buffer_view a)
{
  if(!s.ready)
    return;

  if(s.audio_to_send.size() >= s.audio_to_send.capacity())
    return;

  {
    while(auto p = s.audio_to_free.front()) {
      free(p->audio[0]);

      s.audio_to_free.pop();
    }
  }

  // FIXME use a proper memory pool
  auto buf = (float*)malloc(a.channels * a.frames * sizeof(float));

  audio_buffer bb{.audio = {buf}, .channels = a.channels, .frames = a.frames};
  if(a.channels == 2) {
    bb.audio[1] = bb.audio[0] + a.frames;
  }

  for(int c = 0; c < a.channels; c++) {
    std::copy_n(a.audio[c], a.frames, bb.audio[c]);
  }

  s.audio_to_send.push(bb);
}

void push_video(Streamer& s, video_buffer_view a)
{
  if(!s.ready)
    return;

  if(s.video_to_send.size() >= s.video_to_send.capacity())
    return;

  {
    while(auto p = s.video_to_free.front()) {
      free(p->bytes);

      s.video_to_free.pop();
    }
  }

  // FIXME use a proper memory pool
  auto buf = (unsigned char*)malloc(a.width * a.height * 4);

  video_buffer bb{.bytes = buf, .width = a.width, .height = a.height};

  memcpy(buf, a.bytes, a.width * a.height * 4);

  s.video_to_send.push(bb);
}

audio_frame ReceiverEntry::next_frame() noexcept
{
  if(buf.empty())
    return {{0.f, 0.f}};

  auto res = buf.front();
  buf.pop_front();
  return res;
}

bool ReceiverEntry::push_data_audio(audio_buffer buf)
{
  if(audio_feed == 0)
    return true;

  const gint num_samples = buf.frames;
  GstBuffer* buffer = gst_buffer_new_and_alloc(buf.frames * sizeof(float));

  GST_BUFFER_TIMESTAMP(buffer)
      = gst_util_uint64_scale(this->num_samples, GST_SECOND, self->conf.rate);
  GST_BUFFER_DURATION(buffer)
      = gst_util_uint64_scale(num_samples, GST_SECOND, self->conf.rate);

  GstMapInfo map{};

  gst_buffer_map(buffer, &map, GST_MAP_WRITE);

  auto raw = (float*)map.data;

  for (int i = 0; i < buf.frames; i++)
  {
    raw[i] = buf.audio[0][i];
  }
  this->num_samples += num_samples;

  gst_buffer_unmap(buffer, &map);

  return gst_app_src_push_buffer(GST_APP_SRC(this->sound_in), buffer);
}

bool ReceiverEntry::push_data_video(video_buffer buf)
{
  if(video_feed == 0)
    return true;

  GstBuffer* buffer = gst_buffer_new_and_alloc(buf.width * buf.height * 4);


  GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer) = this->num_frames * 16666666;
  // GST_BUFFER_TIMESTAMP(buffer) = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::microseconds(this->num_frames * 16666)).count();
  // GST_BUFFER_DURATION(buffer) = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::microseconds(16666)).count();
  GstMapInfo map{};

  gst_buffer_map(buffer, &map, GST_MAP_WRITE);

  auto raw = (unsigned char*)map.data;
  memcpy(raw, buf.bytes, buf.width * buf.height * 4);
  this->num_frames++;

  gst_buffer_unmap(buffer, &map);

  return gst_app_src_push_buffer(GST_APP_SRC(this->video_in), buffer);
}
