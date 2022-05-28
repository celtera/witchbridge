#pragma once
#include "custom.hpp"
#include <halp/meta.hpp>
#include <halp/audio.hpp>

#include <cmath>
#include <memory>
#include <iostream>
namespace wb
{
template<std::size_t N>
struct Audio
{
  halp_meta(name, "Witchbridge Audio")
  halp_meta(c_name, "wb_audio_mono")
  halp_meta(uuid, "119d7020-6b7b-4dc9-af7d-ecfb23c5994d")

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

using AudioMono = Audio<1>;
using AudioStereo = Audio<1>;
}
