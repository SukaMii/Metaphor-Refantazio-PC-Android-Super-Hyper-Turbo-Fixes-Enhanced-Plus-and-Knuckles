# The game's shaders for the tests

`fx_test` and the stabilise frame test patch and run some of the game's own pixel shaders. They are the game's content, so they are not part of this repository. Without them `build.ps1 -RunTests` skips those two tests.

The tests are tied to these exact files, named by the hash of their bytecode:

* `ps_822dcd902415fe5d.dxbc` and `ps_5b1af0481406dae1.dxbc` (SMAA edge detection)
* `ps_9f3d5b38f7eff19e.dxbc` and `ps_da00acd64a9e2b1f.dxbc` (SMAA blending weights)
* `ps_6a27f376d7d79b8f.dxbc` (SMAA neighbourhood blending)
* `ps_b3ebc8024b526721.dxbc` (the painterly pass)
* `ps_c76d6b4ed1ee9785.dxbc` (a toon material shader)
* `ps_447820495b495377.dxbc` (an outline shader)

To collect them on a PC:

1. In `MetaphorCPU.ini`, set `[GpuProbe] Capture=1`.
2. In the game, stand in the Sunlumeo street and press F10. The mod writes a `MetaphorCPU_capture_<time>` folder (in its `OutputFolder`) with the frame's pixel shaders as `ps_<hash>.dxbc`.
3. Copy the files above from it into this folder. If one of them is not in your capture, the test that needs it stays skipped.
4. Set `Capture=0` again.
