/* protocol.c - control-message serialization. */
#include "protocol.h"
#include <string.h>

/* Big-endian cursor helpers. */
static inline gsize put_u8  (guint8 *b, gsize o, guint8  v)  { b[o] = v; return o + 1; }
static inline gsize put_u16 (guint8 *b, gsize o, guint16 v)  { guint16 be = GUINT16_TO_BE (v); memcpy (b + o, &be, 2); return o + 2; }
static inline gsize put_u32 (guint8 *b, gsize o, guint32 v)  { guint32 be = GUINT32_TO_BE (v); memcpy (b + o, &be, 4); return o + 4; }
static inline gsize put_u64 (guint8 *b, gsize o, guint64 v)  { guint64 be = GUINT64_TO_BE (v); memcpy (b + o, &be, 8); return o + 8; }

/* scrcpy encodes pressure/scroll as 16-bit fixed-point fractions of [0,1]. */
static inline guint16 to_u16_fixed (double v)
{
  v = CLAMP (v, 0.0, 1.0);
  return (guint16) (v * 0xFFFF);
}

static inline guint32 norm_to_pixel (double norm, guint16 extent)
{
  double p = CLAMP (norm, 0.0, 1.0) * extent;
  return (guint32) (p + 0.5);
}

gsize
pm_protocol_write_touch (guint8        *buf,
                         PmTouchAction  action,
                         guint64        pointer_id,
                         double         norm_x,
                         double         norm_y,
                         guint16        dev_w,
                         guint16        dev_h,
                         double         pressure,
                         guint32        buttons,
                         guint32        action_button)
{
  gsize o = 0;
  o = put_u8  (buf, o, PM_CTRL_INJECT_TOUCH_EVENT);
  o = put_u8  (buf, o, (guint8) action);
  o = put_u64 (buf, o, pointer_id);
  o = put_u32 (buf, o, norm_to_pixel (norm_x, dev_w));   /* x */
  o = put_u32 (buf, o, norm_to_pixel (norm_y, dev_h));   /* y */
  o = put_u16 (buf, o, dev_w);                           /* screen w */
  o = put_u16 (buf, o, dev_h);                           /* screen h */
  o = put_u16 (buf, o, to_u16_fixed (pressure));         /* pressure */
  o = put_u32 (buf, o, action_button);                   /* button that changed */
  o = put_u32 (buf, o, buttons);                         /* full button state */
  return o;                                              /* 32 bytes */
}

gsize
pm_protocol_write_key (guint8      *buf,
                       PmKeyAction  action,
                       guint32      android_keycode,
                       guint32      meta_state)
{
  gsize o = 0;
  o = put_u8  (buf, o, PM_CTRL_INJECT_KEYCODE);
  o = put_u8  (buf, o, (guint8) action);
  o = put_u32 (buf, o, android_keycode);
  o = put_u32 (buf, o, 0);            /* repeat */
  o = put_u32 (buf, o, meta_state);
  return o;                           /* 14 bytes */
}

gsize
pm_protocol_write_text (guint8 *buf, gsize cap, const char *utf8)
{
  gsize tlen = utf8 ? strlen (utf8) : 0;
  if (tlen == 0 || (1 + 4 + tlen) > cap)
    return 0;

  gsize o = 0;
  o = put_u8  (buf, o, PM_CTRL_INJECT_TEXT);
  o = put_u32 (buf, o, (guint32) tlen);
  memcpy (buf + o, utf8, tlen);
  return o + tlen;
}

gsize
pm_protocol_write_display_power (guint8 *buf, gboolean on)
{
  gsize o = 0;
  o = put_u8 (buf, o, PM_CTRL_SET_DISPLAY_POWER);
  o = put_u8 (buf, o, on ? 1 : 0);
  return o;                           /* 2 bytes */
}

gsize
pm_protocol_write_scroll (guint8  *buf,
                          double   norm_x,
                          double   norm_y,
                          guint16  dev_w,
                          guint16  dev_h,
                          double   h_scroll,
                          double   v_scroll)
{
  gsize o = 0;
  o = put_u8  (buf, o, PM_CTRL_INJECT_SCROLL_EVENT);
  o = put_u32 (buf, o, norm_to_pixel (norm_x, dev_w));
  o = put_u32 (buf, o, norm_to_pixel (norm_y, dev_h));
  o = put_u16 (buf, o, dev_w);
  o = put_u16 (buf, o, dev_h);
  /* Scroll deltas are signed 16-bit fixed-point in scrcpy >= 2.x. */
  o = put_u16 (buf, o, (guint16) (gint16) CLAMP (h_scroll * 0x7FFF, -0x8000, 0x7FFF));
  o = put_u16 (buf, o, (guint16) (gint16) CLAMP (v_scroll * 0x7FFF, -0x8000, 0x7FFF));
  o = put_u32 (buf, o, 0);            /* buttons */
  return o;
}
