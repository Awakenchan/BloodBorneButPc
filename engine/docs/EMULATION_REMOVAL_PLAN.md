# Moving away from emulation: what is done and what comes next (2026-10-06)

## Where the API boundary is

Bloodborne is linked with a static libGnm. The PM4 packets are written not by a driver but by the
game itself, through `sce::Gnm::*CommandBuffer` functions (`0x1068000–0x10a0000`,
`0x11fd000–0x11fe000`). Hooking all of them (`BB_GNM_OBSERVE=1`) showed that 100% of submitted
packets are written by known API calls, 99 methods in all. Gnm is an API that writes registers:
`setDepthStencilControl` is one `SET_CONTEXT_REG`. So parsing the PM4 written by these 99 functions
and translating the calls themselves give the same thing; PM4 here is a format for recording calls,
like a command list. It is already parsed in parallel (DrawPreparation).

Hardware emulation is not packet parsing, but what is done instead of the GPU:

| What | State |
|---|---|
| Memory model 0.2 (copies in VRAM, page protection, readback, early labels) | Fallback. The PC model is the default; NVIDIA uses the "chunk per allocation" mode. Remove the 0.2 code once checked on NVIDIA. |
| EOP/EOS labels | PC model: written after the GPU's work completes (honestly). |
| `WaitRegMem` on labels of the same queue | Translated as a completed wait (queue order); the CPU does not need to wait. |
| `WRITE_DATA`/DMA waited for every host copy | They wait only for overlapping copies (as the command processor, which does not wait for shaders). |
| Constant Engine (CE RAM, dumps, counters) | Runs on the CPU in stream order: our parser reads descriptors from memory. |
| SRT walker (GCN shader code run on the CPU) | Needed for the same reason. |
| Tracking GPU writes for the parser (`PendingWriteOverlaps`, `IsRegionGpuModified`) | ~15% of the GpuComm thread, same reason. |
| Texture cache by address + hash | Same reason, plus PM4 has no "texture" objects. |

Almost all remaining emulation exists because **the translator reads resource descriptors
(T#/V#/S#) from the game's memory on the CPU while parsing**. A PC driver and DXVK do not do that:
the GPU reads the application's memory while executing.

## Next, in order

1. **NVIDIA.** Test build `dist/Bloodborne-bbport-x86_64-test-1006.AppImage` (PC model by default,
   "chunk per allocation" mode). If it works, remove memory model 0.2 entirely (`page_manager`,
   `fault_manager`, readback, BB_UFFD, preloading, early labels).
2. **Parallelize stage B** (draw recording; 99% busy on 4 cores, ~3.3 µs per draw, spread evenly
   over buffer/texture binding, render pass start, vertices). Options: (a) several stage B threads
   over draw ranges inside a render pass (each with its own command segment; shared caches are
   already under locks; barrier order per pass); (b) resolve bindings (buffers/textures → Vulkan)
   in stage A, which is idle 28% of the time, and let B only record. Needs walk tests.
3. **Descriptors on the GPU (the main step towards "like DXVK").** The shader reads T#/V#/S#
   itself (buffers in place and BDA already exist); textures as a bindless array, T# → index
   through a GPU table that the texture cache maintains when it creates images. Then the SRT
   walker, CE on the CPU and pending-write tracking are no longer needed; `WRITE_DATA`/DMA/CE
   dumps become GPU writes in stream order. A big project.
4. **Textures as objects.** Hook `Gnm::Texture::init*` and the TPF loader
   (`CSGXTextureCreateFragment`): an image is created at load time, not guessed from an address;
   hashes and address overlaps go away. Assets can be converted to a linear layout ahead of time
   (detiling once, at install).
5. **Shaders ahead of time.** All GCN shaders from the game's files → SPIR-V at install; no
   compiling during play.

What stays a translation, as in DXVK: GCN shaders → SPIR-V, descriptors and formats → Vulkan.

## Step 1 (2026-10-07): the constant engine no longer synchronizes with draw recording

Measurements (`BB_CE_STATS=1`, Yahar'gul): ~14,000 `DumpConstRam` per second, ~4–5 MB/s, 25 pages
in all (a ~100 KB ring). Each dump is read by exactly one pointer in user data: these are
descriptor tables for the SRT walker. Constants, vertices, indices and large buffers are not read
from these pages; there are no shaders that read memory themselves (`uses_dma`). So CE dumps are
needed only by the translator: they build descriptor tables, not data for the GPU.

Stage A's waits on dumps (`WaitForPendingReads`) were needed only because stage B, for draws without
preparation, re-read the V# tables (vertex buffer descriptors) itself; by then the next dump had
overwritten them (~15% of V#s were someone else's: the DoF bug, issue #44).

Done:
- stage A takes the V#s into the draw packet; stage B takes them from there and does not read the
  tables;
- a DMA source is marked with its exact range, not as "reads something unknown".

Result: waits on dumps ~850/s → ~46/s; stage A waits 1.5% of the time instead of ~30% (16 threads).
On 4 cores / 8 threads the FPS is the same (~170): there the bottleneck is stage B (3.4 µs per
packet; most of it `ObtainBuffer`, 0.57 µs: mapping game memory addresses to GPU buffers).

Command processor writes (`BB_CE_STATS`): `WRITE_DATA` ~8,400/s into 2 pages (read as constants
~240/s), DMA ~1,000/s into 15 pages. They write data for the GPU, and their translation is GPU
commands in stream order (`vkCmdUpdateBuffer`; DMA into in-place memory already works that way). For
`WRITE_DATA` this runs into the rule that moves blocks the GPU writes often into VRAM: do it together
with simplifying the memory model.

## Step 2 (2026-10-07): stage B's synchronizations

Stage B (draw recording) is the bottleneck on 4 cores (99% busy). Its per-frame tasks:
- the flip signal = the presenter preparing the frame; 600k ticks per frame, of which ~40k are work,
  the rest waiting for the command recording threads (`CommandBuffer()`) and a synchronous submit.
  Now ~50k: the presenter is recorded through `Record`, the submit is asynchronous;
- the upscaler's passes write commands directly: they get a segment of their own, without waiting;
- DMA (~900/s, 4 bytes) waited for copies and signals even when the GPU does the write: it waits
  only for CPU writes now.

What is left on stage B (µs per packet, 3.5 in all): resource binding 1.0 (buffers 0.4, textures
0.45), `ObtainBuffer` 0.6, render pass start 0.26, vertices 0.34, tasks 0.2. Next: split stage B,
or bind resources in stage A (see item 2 above).

## Direct path step 4 (2026-10-21): draw resources before stage B

Direct path steps 1–3 (Gnm hooks, call records `BB_NATIVE_CALLS`, measurement) are done: draws
arrive as records, but their cost on stage B barely changed (~3 µs): parsing packets was cheap.
What is expensive is resource binding. The preparation workers (`vk_draw_prep.cpp`) already select
the pipeline, read T#/V#/S# and the vertex inputs; the texture description cache and the remembered
sets (`TexSet`) already remove repeated image lookups. What is left on stage B depends on stream
order:
- `ObtainBuffer` for small read-only buffers copies their data into a streaming buffer at draw time
  (renaming: `WRITE_DATA`/CE dumps further down the stream change them);
- the "written by the GPU" checks (`IsRegionGpuModified`) and image freshness change with every
  packet;
- layout transitions and the render pass start follow the order.

So "at the time of the call" is literally impossible: register state from earlier submissions on
the game's thread is unknown, and data is changed after the call (2,016 packets in the observe
measurement). The practical option is in the preparation workers (after submission, before stage B),
for each draw:
1. buffers: address → arena/offset where the path does not depend on order (in place, large,
   writable); stage B checks the buffer cache's generation and takes the prepared result;
2. textures: the `TexSet` key ahead of time; stage B only checks the registry's generation;
3. a verify mode, as for step 2: stage B also computes it itself, compares, logs differences.

Before code, a measurement is needed: `BB_SECTIONS=1 BB_FRAME_STATS=1` in a busy place (Central
Yharnam, a fight): which share of `BindBuffers`/`ObtainBuffer`/`TexSet`/`BeginRendering` really
occupies stage B. Which item to start with depends on it.

## Direct path step 6 (2026-10-21): the game's data for the GPU in place, on Windows

A `BB_SECTIONS=1 BB_FRAME_STATS=1` measurement (Cathedral Ward, RTX 5060, 103–105 FPS, 1,000–1,100
draws per frame) showed: in steady play, texture tracking costs almost nothing (0.5 page
protections per frame for textures against 55 for buffers, 95% of texture sets from the memo,
image hashes 0.00 µs per packet). The limit is stage B (86–89%, ~4.5 µs per packet), and a notable
share of it is buffers: ~2,800 write faults per second (the game `memcpy`s the same buffers every
frame), uploads and re-protection after each (`VRAM track` 0.5 µs, of which `protect` 0.35). So
"textures as objects" would mostly help loading and streaming, while FPS comes from the PC model
(`BB_GUEST_IN_PLACE`): the GPU reads the game's memory itself, without page protection, write
faults or copies. Until now it was only for Linux (dma-buf) and AMD.

The probe `tools/host_import_probe_win.cpp` (it repeats `src/win32_memory.c`: a pagefile-backed
section, `MapViewOfFile3` views into `VirtualAlloc2` placeholders, a second view of the same pages)
on an RTX 5060, driver 617.42:
- `VK_EXT_external_memory_host`, "host allocation" type: the section's view imports
  (`minImportedHostPointerAlignment` 4096, memory type 2: host visible, coherent); "host mapped
  foreign memory": no;
- the GPU sees the CPU's writes, including later ones through the second view, without an upload;
  the CPU sees the GPU's writes in both views; importing at a 4 KiB offset works;
- GPU reads from this memory run at 10.5 GB/s (PCIe), VRAM is several times faster: what is read
  often must still move to VRAM (the PC model's promotions, `QueuePromotions`);
- the sparse arena: binding imported memory to a block works and the data is right, but the arena's
  `memoryTypeBits` is 0x2 (VRAM only) and the import is type 2: the spec does not allow this, and the
  next driver may break it.

Hence the Windows design, without binding to the sparse arena:
1. `BbGuestMemory` for Windows: chunks = imports of `backing + phys` (the whole pool's view already
   exists), each with its own `vk::Buffer`; `Find(phys)` as now.
2. `ObtainBuffer`: a range lying entirely in one chunk and not promoted to VRAM is given as the
   chunk's buffer with an offset (without `EnsureResident`/`SynchronizeMemory`); the rest as now. A
   verify mode as for step 2: stage B compares the chunk's data with the copy in the arena.
3. The resource loader hooks (`bbport_guest_hooks.cpp`, SIGTRAP now) on a Windows vectored exception
   handler: with page protection off, images and VRAM copies learn about writes only from them.
4. Lift the "AMD only" restriction (`PcModelGpu`) after walk tests on NVIDIA.

Done (0.9.37 beta 2, off by default; `BB_GUEST_IN_PLACE=1` in Extra variables):
- `shim/bbport_guest_memory_win.cpp`: a self-check at start (import of the section's view, the
  arena's memory types; out of spec is logged, not refused); 256 MiB chunks are imported from
  `runtime_memory_backing()` at the first `Find(phys)`. The PC model is then the same as on Linux:
  arena blocks are bound to the chunks (`BindInPlace`), writes are not protected.
- `shim/bbport_guest_hooks_win.cpp`: the same places in the game's code, int3 and a vectored
  exception handler instead of SIGTRAP; `LoaderCopy` is `sysv_abi` (the game's code calls it).
- The `VK_EXT_external_memory_host` extension is enabled only with `BB_GUEST_IN_PLACE=1`.
- Verify mode: `BB_WRITE_VERIFY=1`: pages the GPU watches are protected again, and a write that no
  hook reported shows in the log (write fault sites).
The self-check verifies the import, but not the sparse binding itself (only
`tools/host_import_probe_win.cpp` checks that). If a driver stops accepting it, item 2 above is
needed: chunk buffers without the sparse arena. Since 0.9.37 beta 4 the PC model is the default;
binding imports into the sparse arena is outside the Vulkan spec on every driver tested
(docs/COMPATIBILITY.md, "Known driver behaviour"), so item 2 is also a compatibility fix.

Item 2, part one (`BB_CHUNK_BUFFERS=1`, off by default; after 0.9.37 beta 7):
- `ObtainBuffer`, `ObtainBufferForImage` and `CommandWriteTarget` give a range in place that lies in
  one chunk and one mapping as the chunk's own buffer with an offset (`RouteInPlace`); a texel
  buffer filled from an image is filled there too. Ranges outside one chunk, and ranges only partly
  in place, still go through the arena. The barrier tracker keys accesses by buffer, so the arena and
  the chunk do not see each other's accesses: such an arena access makes the runtime answer "barrier
  needed" until a barrier follows it (`Runtime::NoteAliasedAccess`).
- Counted once a minute ("Chunk buffers, last 60 s: ..."): bindings through their chunk, and the
  arena's by reason (outside one chunk, partly in place, texture reads, command processor writes,
  with how many are written). These counts decide part two.
- `BB_CHUNK_VERIFY=1` (turns it on too): every chunk binding is checked against the arena's binding of
  its blocks (the chunk's memory at the same offset), and 16 ranges a second are copied on the GPU
  through the arena, the chunk and the arena again, and compared ("Chunk buffers check: ...").
- The buffer cache's buffer over a chunk (texture reads, copy-backs) was made with the dma-buf handle
  type on Windows too, where the memory is a host import; it now names the chunk's handle type
  (`BbGuestMemory::ChunkHandleType`) and has the chunk buffer's usage (`ChunkUsage`), on every
  path, the switch aside.
- `tools/chunk_buffer_probe.cpp` makes these calls alone under the validation layer. lavapipe,
  2026-10-22: the chunk and the chunk route 0 errors, the data right both ways; beta 7's buffer over
  the chunk `VUID-vkBindBufferMemory-memory-02985`; the arena's binding of the import
  `VUID-VkSparseMemoryBind-memory-02731` (the arena is not made for external memory), and the GPU
  read other bytes through it than the CPU had written.

The pool committed as it is mapped (`BB_POOL_LAZY=1`, off by default; 0.9.37 beta 9): the pool
section only reserves its size (bb-start's section too) and `win_mem_map` commits what the game
maps, through the pool's own view (`win_mem_commit`; a chunk import commits its 256 MiB first). The
committed pool charges Windows its whole size at the start (5056 or 9152 MiB plus 1 GiB of flexible
memory). First run (RTX 5060, 2026-10-22, about 6 GB of commit free): the game maps its graphics
heap whole at the start (2415 MiB at 0x42c00000), Windows refused it (error 1455) and the game
crashed right after, where the committed pool is refused by bb-start's check before the game starts.
So it only saves what the game never maps (about 1.9 GB of the 6080 MiB pool): the game's heaps,
about 4.2 GB mapped at the start, are its own working set, as on the PS4. Before it can be the
default, bb-start must still check the commit the start needs (the heaps), and a refused commit must
end the start with that message instead of a crash.

Part two, the sparse binding itself gone: blocks in place are no longer bound in the arena; the page
table (shaders that read memory by address) points at the chunks (device addresses on the imports);
ranges outside one chunk or partly in place take another way (a GPU copy for reads; VRAM or a copy
back for writes). It waits for part one's counts from a real session.
