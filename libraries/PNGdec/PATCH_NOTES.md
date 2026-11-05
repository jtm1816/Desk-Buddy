# Local PNGdec modifications

Based on PNGdec 1.1.6 by Larry Bank / bitbank2. License: Apache-2.0; retain LICENSE and all source notices.

Modified November 5, 2026 for this DeskBuddy build:

- `src/PNGdec.h`: under `PNG_SPLIT_DICTIONARY`, store an external dictionary pointer instead of the inline 32 KiB window and expose `setDictionary()`.
- `src/png.inl`: use the caller-supplied dictionary when the flag is enabled; report a memory error if it is missing.

The unflagged configuration retains upstream inline-buffer behavior. DeskBuddy sets the flag in `build_opt.h`, calls `setDictionary()` after opening the PNG, and protects shared sprite/input memory during decoding. Keep this library copy with the matching sketch.
