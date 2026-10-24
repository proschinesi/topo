---
name: Report your chip
about: Tell us how topo works on your Mac, to extend the table of tested chips
labels: chip-report

---

topo has been tested only on a few chips. Reports from other Macs (M1, M3, M4, Pro/Max/Ultra variants,
other macOS versions) help to find wrong or missing readings.

**System**

 - Mac model and chip: [e.g. Mac Studio, M1 Ultra 20-core]
 - macOS version and build: `sw_vers`
 - topo version: `topo --version`

**SoC sample**

<!-- Run this command while the Mac does something (a build, a video, a game), so that the GPU is active
for a few seconds, and paste the output. It contains only hardware readings and data sources. -->

```
topo --dump-soc --count 3
```

**Screenshot**

<!-- A screenshot of topo with the cpu, gpu and pwr boxes visible (keys 1, 5 and 6). -->

**What looks right, what looks wrong**

<!-- E.g. "core clusters correct", "GPU temperature missing (—)", "CPU power looks too low compared to
mactop/powermetrics". -->
