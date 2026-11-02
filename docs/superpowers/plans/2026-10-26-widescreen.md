# Widescreen implementation plan

Goal: support correct wide gameplay projection, configurable field of view and HUD placement while deliberately preserving original menu and cutscene framing.

1. Establish and test a safe aspect-preserving presentation fallback for the HLE output, including invalid or minimized windows.
2. Trace the title's projection matrix, viewport, HUD draws, and scene transitions on both renderer paths; verify ownership using controlled matching-frame captures before changing them.
3. Implement opt-in horizontal expansion and FOV on verified gameplay projection only; preserve the 4:3 baseline and apply explicit aspect-safe policy to menus and cutscenes.
4. Add independently configurable HUD placement only after identifying HUD draws, then verify reference captures, aspect ratios, resizing and transitions. Document backend and hardware limitations.

The renderer's inherited Halo widescreen code is not evidence that this title's camera or HUD uses the same contract. Existing controls, rendering, and DSP timing must remain intact.
