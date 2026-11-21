# MM5 / XEEN

Launch the installed `C:\Games\MM5\XEEN.EXE` using option 5 of `run.bat`.
At the Sound Blaster IRQ prompt, choose **7** for this installation.
The general emulator default remains IRQ5; the CLI equivalent is `--sb-irq 7`.
Keep the game's setup and the emulated card on the same IRQ, port 220h, DMA1.

## Speech Hang

The supplied game's `XEEN.CFG` selects IRQ7. Its driver installs interrupt
vector 0Fh and unmasks IRQ7, leaving IRQ5 masked. With the default card IRQ5,
the first DSP single-cycle PCM command (`40 BC 14 6D 1E`) completes DMA, but
its interrupt is never acknowledged at port 22Eh. The game then waits for a
busy flag that its IRQ7 handler would clear.

Changing only the configured card IRQ to 7 restores speech-buffer completion.
No IRQ remapping hack or game-binary patch is needed. A diagnostic run on
copies of the option-5 disks reached 110 emulated seconds, with 26 DSP IRQ
acknowledgments and continued animation. Original game images were unchanged.

## Distortion

The inspected intro uses melodic FM, not OPL rhythm mode (`BD=00`). Its output
was heavily clipped by an extra fixed x3 gain in the card mixer. That gain
has been removed: maximum mixer volume now applies unity gain before the
explicit programmable output-gain registers.

In the diagnostic 75-90 second interval, rail samples fell from 80,750 to 816
without changing the guest's IRQ or framebuffer trace. Residual clipping and
approximate FM timbre remain; this was a headless PCM check, not a listening
comparison against a physical sound card or an SDL/WASAPI capture.

`tests/test_audio_config.cpp` covers IRQ selection, repeated DMA1 completion
through PIC IRQ7/INT 0Fh, DSP acknowledgment versus PIC EOI, and unity DAC gain.
