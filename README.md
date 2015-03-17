# spkr

spkr plays interleaved 32-bit float pcm from stdin. It is intended to
provide a *nix file interface to the sound card for languages that
aren't able to have a wait-free audio thread (e.g. Go). Use
https://github.com/rynlbrwn/acat to produce the input and you'll have
a really awesome music player:

```
$ acat foo.wav bar.wav | spkr
```
