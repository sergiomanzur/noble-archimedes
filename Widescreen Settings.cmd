@echo off
rem Experimental Xbox display settings. Edit the values and save this file.
rem Aspect is width divided by height: 1.7777778 = 16:9; 2.3333333 = 21:9.
set "RECOMP_WIDE_ASPECT=1.7777778"
rem FOV is a multiplier of the original tangent of half the vertical FOV.
rem 1.0 is original framing; larger values show more (allowed: 0.5 to 2.0).
set "RECOMP_WIDE_FOV_SCALE=1.0"
rem Initial window height in pixels, 360 through 2160; resize freely afterward.
set "RECOMP_WIDE_WINDOW_HEIGHT=720"
