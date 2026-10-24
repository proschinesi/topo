---
name: Bug report
about: Something in topo is wrong, missing or crashes
labels: bug

---

**Read the README and search for similar issues before posting a bug report.**

**Describe the bug**

<!-- A clear and concise description of what the bug is. -->

**To reproduce**

<!-- Steps to reproduce the behavior. -->

**Expected behavior**

<!-- What you expected to happen instead. -->

**Screenshots**

<!-- If applicable, a screenshot of the topo window. -->

**System**

 - Mac model and chip: [e.g. MacBook Pro, M3 Pro 11-core]
 - macOS version and build: `sw_vers`
 - topo version: `topo --version`
 - Installed with: [Homebrew / built from source]
 - Terminal and font:

**SoC sample**

<!-- Paste the output of the command below. It contains only hardware readings (chip, clusters,
frequencies, power, temperatures) and the data source of each value, no personal data. -->

```
topo --dump-soc --count 3
```

**Log**

<!-- Contents of `~/.local/state/topo.log` (run `topo --debug` if it is empty). -->

**Crash backtrace**

<!-- If topo crashes: `lldb topo`, then `r`; after the crash `bt all`, and paste the output here. -->
