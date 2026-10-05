# Notice

This is an independent, clean-room implementation based on observable behavior of public webpages and official Waveshare hardware documentation.

It is not affiliated with, endorsed by, or distributed by Creature Company or Waveshare. "LilGuy", "Creature", and product names may be trademarks of their respective owners.

No Creature source code, scripts, sprites, artwork, firmware, or other proprietary assets are included. The shape catalog, palette catalog, motion model, rasterizer, tests, and ESP-IDF integration in this project are original.

The project vendors a minimally modified copy of Waveshare's Apache-2.0 board support component. The only behavioral change keeps CO5300 brightness at zero until the application's first complete frame has been transferred. Its original license is preserved in `components/waveshare__esp32_s3_touch_amoled_1_75c/LICENSE`.
