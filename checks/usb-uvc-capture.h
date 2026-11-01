/* Numeric streaming probe facts from Wireshark issue 8414; no image bytes. */
static const struct {
  unsigned format, frame, interval, quality, maximum, payload;
} captured_probes[] = {
    {1, 1, 333333, 61, 614400, 3060}, {1, 2, 333333, 61, 38400, 3060},
    {1, 2, 333333, 61, 38400, 3060},  {1, 2, 333333, 61, 38400, 3060},
    {1, 2, 2000000, 61, 38400, 3060}, {2, 2, 333333, 61, 38400, 3060},
    {2, 2, 333333, 61, 38400, 3060},  {2, 2, 333333, 61, 38400, 3060},
    {2, 2, 2000000, 61, 38400, 3060},
};
