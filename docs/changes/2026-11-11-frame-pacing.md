# Frame pacing: frames on the display's refreshes, the poll after the GPU

**Spec:** `docs/specs/2026-10-27-feel-design.md`, "Present mode and late input sampling" (this replaces its wait);
budgets `tests/bench/feel_budgets.json`. **Status:** built 2026-11-11, on `step3-engine-game-boundary`, not pushed.

## 1. Summary

A window's frames were uneven wherever the GPU was faster than the display: with the render scale at 0.5 a play
window made two frames every three refreshes while the player lived. Two rules replace the old wait:

- a frame at the display's rate starts on the display's refreshes, 0.45 ms before one;
- a play window polls its input once the GPU has finished the frame before.

At scale 0.5 the window now makes one frame a refresh (240 a second, p99 over median 1.04, was 2.12). At scale 1 the
look latency fell from 20.8 ms to 19.6 to 20.3 ms and the frame's p99 from 17 ms to 12 ms.

## 2. What was built

- **`LiveHost::onRefreshGrid`** (`ohao/runtime/live_host.cpp`): the frame cap's sleep ends on a refresh. The
  refreshes are placed by the present stamps (`VK_KHR_present_wait`), each stamp moving the place a tenth of what
  it is off by. It applies when the cap is the display's rate (the default) and presents are confirmed.
- **`LiveHost::pollsLate`**: with a look-led player in control the loop waits for the newest frame's fence
  (`VulkanRenderer::waitForSubmitted`) before it polls.
- **Fifo is capped like the other modes**, at the display's rate. It had no cap.
- **Deleted:** `LiveConfig::framesAhead`, `presentToAwait`, `VulkanRenderer::waitForPresentOf`,
  `PresentWaiter::waitResolved`, and `feel_bench --frames-ahead`.

## 3. Bugs found and fixed

- **Two frames every three refreshes.** The old rule polled once the frame before the newest was presented. Here a
  present comes about three refreshes after its submit (12 ms at 240 Hz, of which the GPU is 3 ms), so the loop ran
  at 160 frames a second, 4.2 and 8.4 ms alternating, while the player lived, and at 237 while dead.
- **The cap drifted across the refreshes.** A 4.167 ms timer measured 4.22 ms a frame (237 a second against 240), so
  a frame's start moved through the refresh three times a second, and its latency with it: 14.1 ms, p95 16.0, where
  a start held at the right place gives 13.0 and 13.6.

## 4. Spec corrections

The feel design's late input sampling ("at most N frames unpresented") is replaced by the two rules above. The spec
is a record and is not rewritten; `docs/notes/simulation.md` describes the rules as built.

## 5. Results

`feel_bench`, immediate, the play scene at 1920x1080, the display on, 10 to 20 s a run. Before and after ran from
one build, a switch choosing the rule, in turn.

| Scale | | Latency, median / p95 (ms) | Frame, median / p99 (ms) | p99 / median |
|---|---|---|---|---|
| 1 | before, 3 runs | 20.82, 20.82, 20.82 / 25.1 to 25.3 | 9.1 / 17.2 to 17.5 | 1.87 to 1.92 |
| 1 | after, 4 runs | 20.26, 20.09, 19.61, 19.82 / 22.1 to 23.1 | 9.1 / 11.5 to 12.4 | 1.27 to 1.36 |
| 0.667 | before, 2 runs | 16.68, 16.61 / 17.5 to 17.8 | 4.9 / 13.4 to 13.5 | 2.71 to 2.78 |
| 0.667 | after, 3 runs | 15.62, 15.75, 15.62 / 17.7 to 18.3 | 4.5 / 6.4 to 7.3 | 1.44 to 1.61 |
| 0.5 | before, 1 run | 12.50 / 13.5 | 4.23 / 8.97 | 2.12 |
| 0.5 | after, 2 runs | 12.99, 12.96 / 13.6, 14.1 | 4.17 / 4.27, 4.33 | 1.02, 1.04 |

- **Frames made** in 15 s at scale 0.667: 3,162 to 3,188, against 2,419 to 2,440.
- **The start's place in the refresh** (scale 0.5, one frame a refresh, 10 s each; the start against a present
  stamp, and the median latency): +1.0 ms 15.6, +2.0 ms 14.6, +3.0 ms 13.6, -0.45 ms 13.0, 0 12.5 (p95 14.0 to
  14.8), +0.25 ms 12.2 (p95 16.3), +0.5 ms 12.2 (p95 16.3), +0.75 ms 15.8. The compositor takes a frame between
  +0.5 and +0.75 ms; -0.45 ms is the latest start whose p95 stays within a millisecond of its median.
- **Other waits tried at scale 0.5:** two frames unpresented 12.7 ms, p95 16.5, p99 over median 1.59; three 14.1 ms,
  p95 16.0 (it never waits: the drifting cap).
- **Mailbox** (the default) at scale 0.5: 12.9 ms, p95 14.4, p99 over median 1.04.
- **Gates:** 6 of 6, no GPU fault. The GPU stayed under 55 °C.

## 6. Known gaps

- **Fifo is a refresh slower at scale 0.5:** 17.1 ms (16.5 to 19.2 at four other starts) against the old rule's
  12.5 ms. Fifo shows every frame for a refresh, so at one frame a refresh each waits for the one before; the old
  rule avoided it by making 160 uneven frames a second (p99 over median 3.06). At scale 1 fifo is as immediate
  (20.2 ms against 20.8 before).
- **`kRefreshLead` is measured under KWin 6.8 on this laptop** (the panel on the Intel GPU). Another compositor may
  take its frame elsewhere in the refresh; a wrong place costs at most one refresh.
- **Between the regimes** (scale 0.667, the GPU near one refresh a frame) frames are 4.2 to 7 ms: p99 over median
  1.44 to 1.61 against the budget's 1.5.
- **The 15 ms budget** is met only at scale 0.5 on the 4060. About 9 ms from the GPU's end to the display is the
  compositor's. A fullscreen window was not measured.
- **Without present wait** the refreshes are not known and the cap is a timer, as before.
