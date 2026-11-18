# How Litely works

- Choosing a video also sets a still from it as the Windows wallpaper, even when the desktop is covered. Re-selecting the same file reuses its saved image. Windows displays that still while the player starts.
- Manual pause saves the video frame as a BMP, makes it the Windows Fill wallpaper on all monitors, and stops the playback clock. That still remains after exit.
- Litely keeps its stills out of Settings > Personalization > Background > Recent images.
- Playback decodes on the GPU and presents the decoder's own frames, falling back to the Media Engine when a PC or file can't do that. Settings shows which path is active.
- One muted player feeds a shared DirectComposition surface, mirrored and cropped to fill each monitor. An exposed part of any monitor keeps playback active. After two seconds of full coverage, the app overwrites the existing still with the last submitted frame and applies the selected performance mode. In Smooth resume mode, revealing the desktop restarts playback without rebuilding the graphics device or seeking. Locking the session or turning off the displays releases the renderer and its video buffers.
- The app recognizes ordinary opaque windows and their combined coverage. Translucent and irregular windows can leave the animation active. Desktop embedding uses Explorer's desktop host and can vary with Windows builds.
