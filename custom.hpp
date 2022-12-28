#pragma once
#include <cstdint>
#include <memory>


struct Streamer;
struct config
{
  int port;
  std::string path;

  int rate{};
  int frames{};
};

struct audio_buffer_view {
  float* audio[2];
  int channels;
  int frames;
};
struct video_buffer_view {
  unsigned char* bytes;
  int width, height;
};

std::shared_ptr<Streamer> make_streamer(config c);

void push_audio(Streamer&, audio_buffer_view a);
void push_video(Streamer&, video_buffer_view a);

