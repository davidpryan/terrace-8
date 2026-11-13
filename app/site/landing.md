# TERRACE-8

TERRACE-8 makes time part of the step. Each of eight stages has a pitch and a number of pulses it lasts, with a gate mode deciding what happens across those pulses (once, every pulse, held through, every other, silent), plus ratchets, slide, accent and skip. Eight values become phrases of up to 64 pulses with the swing and stutter of acid and analogue bass lines, without a 64-step grid. Two lines of different lengths cross against each other, and an accumulator lifts a line through the scale each time round. The screen draws each stage as a terrace: as wide as it lasts and as high as it sounds.

![TERRACE-8 on the FM-1's screen](manual/img/screen-stages.png "TERRACE-8 on the FM-1's screen")

Version {{VERSION}}. **Untested on a real FM-1 so far:** it builds, passes its tests in fm1-base's simulator, and
runs in the browser emulator. Installing is at your own risk; the installer can put M-VAVE's own firmware back.

- [Try it in the browser](emu/): the same code the FM-1 runs, with sound. No FM-1 needed.
- [Install](install/): from Chrome or Edge, with the FM-1 connected by USB.
- [Manual](manual/): what it is for, the controls, the screens.
- [Download {{PKG}}](firmware/{{PKG}}): for the command-line installer, `python3 base/tools/fm1_install.py {{PKG}}`.
- [Source](https://github.com/davidpryan/terrace-8): MPL-2.0. Built on fm1-base and felucca-bsp, from Felucca by Leo
  Kuroshita, Hügelton Instruments. One of six sequencer firmwares built from a survey of sequencer archetypes.

M-VAVE and FM-1 are trademarks of their owners. TERRACE-8 is not affiliated with M-VAVE.
