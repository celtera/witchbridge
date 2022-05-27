#include "custom.hpp"

#include <boost/iostreams/device/mapped_file.hpp>

#include <cmath>
#include <glib.h>
#include <gst/audio/audio.h>
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

#define SAMPLE_RATE 44100
const gchar* video_priority = "low";
const gchar* audio_priority = "high";

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

//// Audio

#define CHUNK_SIZE 1024   /* Amount of bytes we are sending in each buffer */
#define SAMPLE_RATE 44100 /* Samples per second we are sending */
struct Streamer
{
  static bool push_data_audio(ReceiverEntry* data)
  {
    const gint num_samples = CHUNK_SIZE / 4;
    GstBuffer* buffer = gst_buffer_new_and_alloc(CHUNK_SIZE);

    GST_BUFFER_TIMESTAMP(buffer)
        = gst_util_uint64_scale(data->num_samples, GST_SECOND, SAMPLE_RATE);
    GST_BUFFER_DURATION(buffer)
        = gst_util_uint64_scale(num_samples, GST_SECOND, SAMPLE_RATE);

    GstMapInfo map{};
    gst_buffer_map(buffer, &map, GST_MAP_WRITE);

    auto raw = (float*)map.data;
    constexpr float cst = 2.f * 3.14f * 220.f / SAMPLE_RATE;

    for (int i = 0; i < num_samples; i++)
    {
      raw[i] = std::sin(cst * data->num_samples++);
    }

    gst_buffer_unmap(buffer, &map);

    GstFlowReturn ret{};
    g_signal_emit_by_name(data->sound_in, "push-buffer", buffer, &ret);
    gst_buffer_unref(buffer);

    return ret == GST_FLOW_OK;
  }

  static void start_feed_audio(GstElement* source, guint size, ReceiverEntry* data)
  {
    if (data->sourceid == 0)
    {
      g_print("Start feeding\n");
      data->sourceid = g_idle_add((GSourceFunc)push_data_audio, data);
    }
  }

  static void stop_feed_audio(GstElement* source, ReceiverEntry* data)
  {
    if (data->sourceid != 0)
    {
      g_print("Stop feeding\n");
      g_source_remove(data->sourceid);
      data->sourceid = 0;
    }
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
  static ReceiverEntry*
  create_receiver_entry(SoupWebsocketConnection* connection)
  {

    auto receiver_entry = (ReceiverEntry*)g_slice_alloc0(sizeof(ReceiverEntry));
    receiver_entry->connection = connection;

    g_object_ref(G_OBJECT(connection));

    g_signal_connect(
          G_OBJECT(connection),
          "message",
          G_CALLBACK(soup_websocket_message_cb),
          (gpointer)receiver_entry);

    GError* error = nullptr;
    std::string pipeline_web
        = "webrtcbin name=webrtcbin stun-server=stun://" STUN_SERVER;
    std::string pipeline_video
        = " videotestsrc is-live=1 pattern=snow ! videorate ! videoscale ! "
          "video/x-raw,width=64,height=64,framerate=60/1 ! "
          "videoconvert ! queue max-size-buffers=1 ! "
          "x264enc bitrate=600 speed-preset=ultrafast tune=zerolatency "
          "key-int-max=15 ! "
          "video/x-h264,profile=constrained-baseline ! queue "
          "max-size-time=100000000 ! h264parse ! "
          "rtph264pay config-interval=-1 name=payloader "
          "aggregate-mode=zero-latency ! "
          "application/x-rtp,media=video,encoding-name=H264,payload=96 ! "
          "webrtcbin. ";

    std::string pipeline_audio
        = " appsrc name=mysound ! "
          "audioconvert ! audioresample ! opusenc ! rtpopuspay "
          "pt=97 ! webrtcbin. ";

    receiver_entry->pipeline = gst_parse_launch(
                                 (pipeline_web + pipeline_video + pipeline_audio).c_str(), &error);
    if (error != nullptr)
    {
      g_error("Could not create WebRTC pipeline: %s\n", error->message);
      g_error_free(error);
      goto cleanup;
    }

    receiver_entry->sound_in
        = gst_bin_get_by_name(GST_BIN(receiver_entry->pipeline), "mysound");
    g_assert(receiver_entry->sound_in);
    {
      GstAudioInfo info;
      gst_audio_info_set_format(
            &info, GST_AUDIO_FORMAT_F32, SAMPLE_RATE, 1, nullptr);
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
            receiver_entry);
      g_signal_connect(
            receiver_entry->sound_in,
            "enough-data",
            G_CALLBACK(stop_feed_audio),
            receiver_entry);
    }

    {
      receiver_entry->webrtcbin
          = gst_bin_get_by_name(GST_BIN(receiver_entry->pipeline), "webrtcbin");
      g_assert(receiver_entry->webrtcbin != nullptr);

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
            (gpointer)receiver_entry);

      g_signal_connect(
            receiver_entry->webrtcbin,
            "on-ice-candidate",
            G_CALLBACK(on_ice_candidate_cb),
            (gpointer)receiver_entry);

      GstBus* bus;
      bus = gst_pipeline_get_bus(GST_PIPELINE(receiver_entry->pipeline));
      gst_bus_add_watch(bus, bus_watch_cb, nullptr);
      gst_object_unref(bus);
    }

    if (gst_element_set_state(receiver_entry->pipeline, GST_STATE_PLAYING)
        == GST_STATE_CHANGE_FAILURE)
      g_error("Could not start pipeline");

    return receiver_entry;

cleanup:
    destroy_receiver_entry((gpointer)receiver_entry);
    return nullptr;
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

    g_slice_free1(sizeof(ReceiverEntry), receiver_entry);
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

    std::cerr << "on_ice_candidate_cb: JSON OUTPUT: '" << json_string << "'\n";
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
    GHashTable* receiver_entry_table = (GHashTable*)user_data;
    g_hash_table_remove(receiver_entry_table, connection);
    gst_print("Closed websocket connection %p\n", (gpointer)connection);
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
          "/home/jcelerier/projets/perso/gstreamer-webrtc/webrtc.html",
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
    ReceiverEntry* receiver_entry;
    GHashTable* receiver_entry_table = (GHashTable*)user_data;

    gst_print("Processing new websocket connection %p", (gpointer)connection);

    g_signal_connect(
          G_OBJECT(connection),
          "closed",
          G_CALLBACK(soup_websocket_closed_cb),
          (gpointer)receiver_entry_table);

    receiver_entry = create_receiver_entry(connection);
    g_hash_table_replace(receiver_entry_table, connection, receiver_entry);
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

#ifdef G_OS_UNIX
  static gboolean exit_sighandler(gpointer user_data)
  {
    gst_print("Caught signal, stopping mainloop\n");
    GMainLoop* mainloop = (GMainLoop*)user_data;
    g_main_loop_quit(mainloop);
    return TRUE;
  }
#endif

  Streamer()
  {
    GMainLoop* mainloop;
    SoupServer* soup_server;
    GHashTable* receiver_entry_table;
    GError* error = nullptr;

    int argc = 0;
    char* argv[] = {nullptr};
    gst_init(&argc, (char***)&argv);
    setlocale(LC_ALL, "C");
    receiver_entry_table = g_hash_table_new_full(
                             g_direct_hash, g_direct_equal, nullptr, destroy_receiver_entry);

    mainloop = g_main_loop_new(nullptr, FALSE);
    g_assert(mainloop != nullptr);

#ifdef G_OS_UNIX
    g_unix_signal_add(SIGINT, exit_sighandler, mainloop);
    g_unix_signal_add(SIGTERM, exit_sighandler, mainloop);
#endif

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
          (gpointer)receiver_entry_table,
          nullptr);
    soup_server_listen_all(
          soup_server, SOUP_HTTP_PORT, (SoupServerListenOptions)0, nullptr);

    gst_print(
          "WebRTC page link: http://127.0.0.1:%d/\n", (gint)SOUP_HTTP_PORT);

    g_main_loop_run(mainloop);

    g_object_unref(G_OBJECT(soup_server));
    g_hash_table_destroy(receiver_entry_table);
    g_main_loop_unref(mainloop);

    gst_deinit();
  }
};

int main(int argc, char** argv)
{
  Streamer s;
}
