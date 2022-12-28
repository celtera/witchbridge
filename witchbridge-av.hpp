#pragma once
#include "custom.hpp"
#include <halp/meta.hpp>
#include <halp/audio.hpp>
#include <halp/texture.hpp>

#include <cmath>
#include <memory>
#include <iostream>
namespace wb
{
template<std::size_t N>
struct Audio
{
  struct
  {
    halp::fixed_audio_bus<"In", float, N> audio;
  } inputs;

  struct
  {
  } outputs;

  std::shared_ptr<Streamer> streamer;

  void prepare(halp::setup t)
  {
    config c;
    c.rate = t.rate;
    c.frames = t.frames;

    streamer = make_streamer(c);
  }

  void operator()(int frames)
  {
    using namespace std;

    switch(N) {
    case 1:
      push_audio(*streamer, {.audio = {inputs.audio.samples[0]}, .channels = N, .frames = frames});
      break;
    case 2:
      push_audio(*streamer, {.audio = {
                               inputs.audio.samples[0],
                               inputs.audio.samples[1],
                             },
                             .channels = N, .frames = frames});
      break;
    }
  }
};

struct AudioMono : Audio<1>
{
  halp_meta(name, "Witchbridge Audio (mono)")
  halp_meta(c_name, "wb_audio_1ch")
  halp_meta(uuid, "119d7020-6b7b-4dc9-af7d-ecfb23c5994d")
};

struct AudioStereo : Audio<2>
{
  halp_meta(name, "Witchbridge Audio (stereo)")
  halp_meta(c_name, "wb_audio_2ch")
  halp_meta(uuid, "58b9924c-b405-4bad-a1d0-60159dafd019")
};

struct Texture
{
  halp_meta(name, "Witchbridge Video")
  halp_meta(c_name, "wb_video")
  halp_meta(uuid, "942ce751-6791-4209-9625-af0e189afd78")

  struct
  {
    halp::texture_input<"In"> image;
  } inputs;

  struct
  {
    halp::texture_output<"Out"> image;
  } outputs;

  std::shared_ptr<Streamer> streamer;

  Texture()
  {
    config c;
    c.rate = 1;
    c.frames = 1;

    streamer = make_streamer(c);

    outputs.image.create(1, 1);
    outputs.image.upload();
  }

  void operator()()
  {
    using namespace std;

    push_video(*streamer,
               {.bytes= inputs.image.texture.bytes
               , .width = inputs.image.texture.width
               , .height = inputs.image.texture.height
               });
  }
};

}
