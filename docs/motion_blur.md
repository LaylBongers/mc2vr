# Motion blur and depth of field (removed)

Code: `src/carrier/reimpl/render/bloom_emit_passes.cpp`, the reimplemented `PgLtiBloom_EmitPasses` ([reimpl.md](reimpl.md)). Always off, no config switch: neither is wanted in a headset.

## Where they live

Both are terms of the final post composite (`shader3.bin 0x24e990`), the last record `PgLtiBloom_EmitPasses` emits. That draw also does tone mapping, adaptation, bloom, grain and grading, so it can't be dropped; there is no engine switch for either effect (the `MotionBlur` ini setting is only a per-object mask that misses foliage). RE: [reverse_engineering/post_chain.md](reverse_engineering/post_chain.md).

## The change

The reimplemented emitter builds the composite with:
- **No motion blur:** `MotionBlurConstantX/Y/W` and the velocity scale are uploaded as zeros, so all four taps read the pixel itself (their sum × 0.25 = the pixel). `PostFx_BuildMotionBlurMatrix` is not called.
- **No DOF:** `filterParams.w = 1`. The focus term is a product of two saturates, so the weight is exactly 1: the sharp image.

The constants are still uploaded because the shader reads them. Everything else in the post chain is unchanged.

Live (2026-11-12): verified by eye in gameplay, no smear and the rest of the post chain looking unchanged; no errors.

OPEN:
- The half-size pass (`bloom+0x22c8`, material `+0x2080`) is still emitted. It is inferred to make `blurMap2`, which DOF alone reads. Drop it once a sampler check confirms that (a GPU saving of one half-size pass).
