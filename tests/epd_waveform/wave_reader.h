static uint8_t bin[262144];
static size_t bin_size;
static uint32_t offset, frames;
static int selected;
static uint32_t word(size_t at)
{
    CHECK(at + 4 <= bin_size);
    return (uint32_t)bin[at] | (uint32_t)bin[at+1] << 8 |
           (uint32_t)bin[at+2] << 16 | (uint32_t)bin[at+3] << 24;
}
int waveform_bin_reader_init(uint32_t max_size)
{ CHECK(bin_size <= max_size && word(0) == 0xe9d7ab1eu); return 0; }
uint32_t waveform_bin_reader_get_frames(int temperature, WAVE_TABLE_MODE_T mode)
{
    selected = mode;
    uint32_t count = word(8 + mode * 8), table = word(12 + mode * 8);
    for (uint32_t i = 0; i < count; ++i) {
        size_t at = table + i * 16;
        if (temperature >= (int32_t)word(at) && temperature < (int32_t)word(at + 4)) {
            frames = word(at + 8); offset = word(at + 12);
            CHECK(offset + frames * 64 <= bin_size);
            return frames;
        }
    }
    CHECK(0); return 0;
}
void waveform_bin_reader_fill_lut(uint32_t *lut, uint32_t frame)
{
    CHECK(frame < frames);
    for (unsigned i = 0; i < 256; ++i)
        lut[i] = ((bin[offset + frame * 64 + i / 4] >> (6u - (i % 4) * 2u)) & 3u) << 3;
}
