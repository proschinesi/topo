% topo(1) | User Commands
%
% 2026-10-24

# NAME

topo - terminal system monitor for Apple Silicon Macs, a fork of btop++ with Neural Engine, power and CPU cluster data.

# SYNOPSIS

**topo** [**-c** _file_] [**-d**] [**-f** _filter_] [**-l**] [**-p** _id_] [**-t**] [**-u** _ms_] [**\-\-force-utf**] [**\-\-themes-dir** _dir_]

**topo** **\-\-dump-soc** [**\-\-count** _n_] [**\-\-interval** _ms_]

**topo** [**\-\-default-config** | {**-h** | **\-\-help**} | {**-V** | **\-\-version**}]

# DESCRIPTION

**topo** shows usage and stats for processor, memory, disks, network and processes, like **btop**(1), and adds
the SoC data of Apple Silicon Macs, read without root privileges: CPU cores grouped by cluster with frequency and
temperature, GPU usage, frequency, power and temperature, Neural Engine (ANE) power and estimated usage, CPU, GPU,
ANE, DRAM and system power, DRAM bandwidth and the macOS thermal state.

Values that cannot be read are shown as "—". Estimates are marked with "~" (and "(est.)" for the DRAM bandwidth).

# OPTIONS

The program follows the usual GNU command line syntax, with long options
starting with two dashes ('-'). A summary of options is included below.

**-c**, **\-\-config _file_**
:   Path to a config file.

**-d**, **\-\-debug**
:   Start in debug mode with additional logs and metrics.

**-f**, **\-\-filter _filter_**
:   Set an initial process filter.

**\-\-force-utf**
:   Force start even if no UTF-8 locale was detected.

**-l**, **\-\-low-color**
:   Disable true color, 256 colors only.

**-p**, **\-\-preset _id_**
:   Start with a preset (0-9).

**-t**, **\-\-tty**
:   Force tty mode with ANSI graph symbols and 16 colors only.

**\-\-no-tty**
:   Force disable tty mode.

**\-\-themes-dir _dir_**
:   Path to a custom themes directory. When specified, this directory takes priority over the default theme search paths.

**-u**, **\-\-update _ms_**
:   Set an initial update rate in milliseconds.

**\-\-dump-soc**
:   Print Apple SoC samples as JSON lines, with the data source of every value, and exit.

**\-\-count _n_**
:   Number of samples printed by **\-\-dump-soc** (default 1).

**\-\-interval _ms_**
:   Interval between **\-\-dump-soc** samples in milliseconds (default 1000).

**\-\-default-config**
:   Print default config to standard output.

**-h**, **\-\-help**
:   Show summary of options.

**-V**, **\-\-version**
:   Show version of program.

# KEYS

**1**, **2**, **3**, **4**
:   Toggle the cpu, mem, net and proc boxes, as in btop.

**5**
:   Toggle the gpu box (GPU and ANE).

**6**
:   Toggle the pwr box (CPU, GPU, ANE, DRAM, system and SoC power). While it is shown, the gpu box leaves out
    its own power readings.

All other keys work as in **btop**(1); press **h** or **F1** in the program for the full list.

# CONFIGURATION

The configuration file is _~/.config/topo/topo.conf_ (or _$XDG_CONFIG_HOME/topo/topo.conf_). Options added by topo:

**show_ane** (default true)
:   Show the ANE row and the split GPU/ANE graph in the gpu box.

**pwr_graphs** (default true)
:   Show one-line graphs in the pwr box; false shows numbers only.

**dram_bw_estimate** (default true)
:   Show the PMP estimate of the DRAM bandwidth when the direct counters are not readable.

**proc_gpu** (default false)
:   Show a Gpu% column in the process list.

# FILES

_~/.config/topo/topo.conf_
:   Configuration file.

_~/.config/topo/themes_
:   User themes; themes are also read from the btop theme folders.

_~/.local/state/topo.log_
:   Log file.

# NOTES

topo reads undocumented Apple interfaces (IOReport, SMC, HID sensors, IORegistry) that can change with any macOS
update. On macOS 27 the CPU and ANE power comes from the AppleCLPC driver counters, cross-checked against the GPU
energy counter; see **\-\-dump-soc** (_clpc_check_).

# BUGS

Report bugs and chip reports at https://github.com/proschinesi/topo/issues.

# SEE ALSO

**btop**(1), **top**(1), **htop**(1), **powermetrics**(1)

# AUTHOR

**topo** is maintained by the topo contributors. It is based on **btop**, written by Jakob P. Liljenberg
a.k.a. "Aristocratos" and the btop contributors, and uses code adapted from macmon and mactop (see NOTICE).
