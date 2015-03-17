// spkr plays interleaved 32-bit float pcm from stdin.
//
// $ acat foo.wav bar.wav | spkr
//
// You can specify the sample rate (default is 44100) and the number
// of channels (default is 2) of the input audio.
//
// $ acat mono-48000.wav | spkr -c 1 -r 48000
//
// The primary goal is smooth playback. This means that the audio
// thread must not do anything unbounded (no new/delete, no syscalls,
// no blocking). The audio thread only does three things. First, it
// attempts to receive a chunk from the main thead via
// Channel. Second, it copies the chunk, or zeros if no chunk was
// available, to shared sound card memory via memcpy. Third, it
// attempts to send the chunk back to the main thead via Channel. If
// it was not able to send because the Channel was full, it has no
// choice but to just forget about it (memory leak) since space cannot
// be allocated/deallocated by the audio thread.
//
// The main thread then has two things it must do to help the audio
// thread. First, it must make sure there is always a chunk ready,
// that way the audio thread doesn't have to output zeros. It will
// accomplish this via buffered reading from stdin. Second, it must
// make sure there is always space for the chunk coming back. It will
// accomplish this my making sure that the Channel has enough space
// for all of the allocated chunks in the system.

#include <iostream>
#include <string>
#include <unistd.h>
#include <portaudio.h>

#include "github.com/rynlbrwn/channel/channel.h"

using std::cout;
using std::endl;
using std::stoi;
using std::string;

struct Chunk {
  int len;
  float* buf;
  // The audio callback will fill-in the time at which the buffer
  // should start playing out of the speaker relative to
  // Pa_GetStreamTime(). This is used to wait for all audio to finish
  // playing before terminating.
  PaTime out_time;

  Chunk(int frames_per_chunk, int nchannels)
    : len(frames_per_chunk * nchannels), buf(new float[len]), out_time(0) {}
  ~Chunk() { delete[] buf; }
};

struct Config {
  // Audio is output one frame at a time. Each frame includes one
  // sample per output channel. A mono frame would include only one
  // sample, a stereo frame two samples.
  int nchannels;
  int frames_per_chunk;
  int sample_rate;
  Channel<Chunk*> buffer;
  Channel<Chunk*> free_list;

  Config(int nchannels, int sample_rate, int nchunks)
    : buffer(nchunks), free_list(nchunks) {
    this->nchannels = nchannels;
    this->sample_rate = sample_rate;
    frames_per_chunk = getpagesize() / sizeof(float) / nchannels;
  }
};

int callback(const void* input, void* output, unsigned long nframes,
             const PaStreamCallbackTimeInfo* timeInfo,
             PaStreamCallbackFlags flags, void *data) {
  Config* c = reinterpret_cast<Config*>(data);
  float* out = reinterpret_cast<float*>(output);
  Chunk* chunk;
  if (c->buffer.Receive(&chunk)) {
    assert(chunk->len == nframes * c->nchannels);
    memcpy(out, chunk->buf, chunk->len * sizeof(float));
    chunk->out_time = timeInfo->outputBufferDacTime;
    c->free_list.Send(chunk);
  } else {
    memset(out, 0, nframes * c->nchannels * sizeof(float));
  }
  return paContinue;
}

void usage(string name) {
  cout << "usage: " << name
       << " [-c <nchannels>] [-r <sample_rate>]" << endl;
  exit(1);
}

int parseInt(int i, int argc, char **argv) {
  if (i >= argc) {
    usage(argv[0]);
  }
  try {
    return stoi(argv[i]);
  } catch (const std::invalid_argument& e) {
    cout << "expected integer argument" << endl;
    exit(1);
  }
}

void must(PaError err) {
  if (err != paNoError) {
    cout << Pa_GetErrorText(err) << endl;
    exit(1);
  }
}

bool fill(float* buf, int len) {
  int floats_read = fread(buf, sizeof(float), len, stdin);
  if (floats_read == 0) {
    return false;
  }
  memset(buf+floats_read, 0, (len-floats_read)*sizeof(float));
  return true;
}

int main(int argc, char** argv) {
  freopen(NULL, "rb", stdin);

  int nchannels = 2, sample_rate = 44100;
  for (int i = 1; i < argc; i++) {
    string s(argv[i]);
    if (s == "-c") {
      nchannels = parseInt(++i, argc, argv);
    } else if (s == "-r") {
      sample_rate = parseInt(++i, argc, argv);
    } else {
      usage(argv[0]);
    }
  }
  int nchunks = 16;
  Config c(nchannels, sample_rate, nchunks);

  must(Pa_Initialize());
  PaStream *stream;
  must(Pa_OpenDefaultStream(&stream, 0, c.nchannels, paFloat32, c.sample_rate,
                            c.frames_per_chunk, callback, &c));

  for (int i = 0; i < nchunks; i++) {
    Chunk* chunk = new Chunk(c.frames_per_chunk, c.nchannels);
    if (!fill(chunk->buf, chunk->len)) {
      delete chunk;
      nchunks = i;
      break; // no more data
    }
    bool ok = c.buffer.Send(chunk);
    assert(ok);
  }
  if (nchunks == 0) {
    return 0;
  }

  must(Pa_StartStream(stream));

  PaTime last_out_time = 0;
  while (1) {
    Chunk *chunk;
    while (!c.free_list.Receive(&chunk)) {
      Pa_Sleep(10);
    }
    if (!fill(chunk->buf, chunk->len)) {
      last_out_time = chunk->out_time;
      delete chunk;
      nchunks--;
      break; // no more data
    }
    bool ok = c.buffer.Send(chunk);
    assert(ok);
  }
  
  while (nchunks > 0) {
    Chunk *chunk;
    while (!c.free_list.Receive(&chunk)) {
      Pa_Sleep(10);
    }
    last_out_time = chunk->out_time;
    delete chunk;
    nchunks--;
  }

  float chunk_duration = float(c.frames_per_chunk) / c.sample_rate;
  float end_time = last_out_time + chunk_duration;
  while (Pa_GetStreamTime(stream) < end_time) {
    Pa_Sleep(10);
  }

  Pa_StopStream(stream);
  Pa_CloseStream(stream);
  Pa_Terminate();
  return 0;
}
