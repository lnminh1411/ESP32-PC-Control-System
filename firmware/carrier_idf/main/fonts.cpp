#include "fonts.h"

extern const uint8_t font_comic12[], font_comic16[], font_comic20b[], font_comic28b[];
extern const uint32_t font_comic12_len, font_comic16_len, font_comic20b_len, font_comic28b_len;

struct LoadedFont {
  lgfx::PointerWrapper data;
  lgfx::VLWfont font;
  bool ok = false, tried = false;
};

static const lgfx::IFont *load(LoadedFont &f, const uint8_t *vlw, uint32_t len) {
  if (!f.tried) {
    f.tried = true;
    f.data.set(vlw, len);
    f.ok = f.font.loadFont(&f.data);
  }
  return f.ok ? (const lgfx::IFont *)&f.font : &fonts::Font2;   // bad array: still readable
}

static LoadedFont small, body, head, big;

const lgfx::IFont *fontSmall() { return load(small, font_comic12, font_comic12_len); }
const lgfx::IFont *fontBody() { return load(body, font_comic16, font_comic16_len); }
const lgfx::IFont *fontHead() { return load(head, font_comic20b, font_comic20b_len); }
const lgfx::IFont *fontBig() { return load(big, font_comic28b, font_comic28b_len); }
