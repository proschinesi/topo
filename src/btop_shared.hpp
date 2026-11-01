/* Copyright 2021 Aristocratos (jakob@qvantnet.com)

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

	   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

   Modified for topo (fork of btop for Apple Silicon), 2026: see NOTICE.

indent = tab
tab-size = 4
*/

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <unistd.h>

// From `man 3 getifaddrs`: <net/if.h> must be included before <ifaddrs.h>
// clang-format off
#include <net/if.h>
#include <ifaddrs.h>
// clang-format on

#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
# include <kvm.h>
#endif

using std::array;
using std::atomic;
using std::deque;
using std::string;
using std::tuple;
using std::vector;

using namespace std::literals; // for operator""s

//? topo: nome e versione del fork. CMake li passa da APP_NAME/APP_VERSION;
//? i default servono per il Makefile upstream, che non li definisce.
#ifndef APP_NAME
#define APP_NAME "topo"
#endif
#ifndef APP_VERSION
#define APP_VERSION "0.2.0"
#endif

namespace Tools {
	class atomic_waiting_lock;
}

void term_resize(bool force=false);
void banner_gen();

extern void clean_quit(int sig);

namespace Global {
	extern const vector<array<string, 2>> Banner_src;
	extern const string Version;		// versione btop upstream di partenza
	extern const string Name;			// APP_NAME
	extern const string AppVersion;	// APP_VERSION
	extern const string FullVersion;	// "topo 0.1.0 (btop 1.4.7)"
	extern atomic<bool> quitting;
	extern string exit_error_msg;
	extern atomic<bool> thread_exception;
	extern string banner;
	extern atomic<bool> resized;
	extern string overlay;
	extern string clock;
	extern uid_t real_uid, set_uid;
	extern atomic<bool> init_conf;
}

namespace Runner {
	extern Tools::atomic_waiting_lock active;
	extern atomic<bool> reading;
	extern atomic<bool> stopping;
	extern atomic<bool> redraw;
	extern atomic<bool> coreNum_reset;
	extern bool pause_output;
	extern string debug_bg;

	void run(const string& box = "", bool no_update = false, bool force_redraw = false);
	void stop();
}

namespace Tools {
	//* Platform specific function for system_uptime (seconds since last restart)
	double system_uptime();
}

namespace Shared {
	//* Initialize platform specific needed variables and check for errors
	void init();

	extern long coreCount, page_size, clk_tck;

#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
	struct KvmDeleter {
		void operator()(kvm_t* handle) {
			kvm_close(handle);
		}
	};
	using KvmPtr = std::unique_ptr<kvm_t, KvmDeleter>;
#endif
}

#if defined(GPU_SUPPORT)

namespace Gpu {
	extern vector<string> box;
	extern int width, total_height, min_width, min_height;
	extern vector<int> x_vec, y_vec;
	extern vector<bool> redraw;
	extern int shown;
	extern int count;
	extern vector<int> shown_panels;
	extern vector<string> gpu_names;
	extern vector<int> gpu_b_height_offsets;
	extern vector<bool> gpu_pwr_row_hideable; //? topo: riga PWR del box gpu da nascondere quando c'e' il box pwr
	extern long long gpu_pwr_total_max;

	extern std::unordered_map<string, deque<long long>> shared_gpu_percent; // averages, power/vram total

	extern const array<string, 2> mem_names;

	//* Container for process information // TODO
	/*struct proc_info {
    unsigned int pid;
    unsigned long long mem;
	};*/

	//* Container for supported Gpu::*::collect() functions
	struct gpu_info_supported {
		bool gpu_utilization = true,
		   	 mem_utilization = true,
				 gpu_clock = true,
				 mem_clock = true,
				 pwr_usage = true,
				 pwr_state = true,
				 temp_info = true,
				 mem_total = true,
				 mem_used = true,
				 pcie_txrx = true,
				 encoder_utilization = true,
				 decoder_utilization = true;
	};

	//* Per-device container for GPU info
	struct gpu_info {
		std::unordered_map<string, deque<long long>> gpu_percent = {
			{"gpu-totals", {}},
			{"gpu-vram-totals", {}},
			{"gpu-pwr-totals", {}},
		};
		unsigned int gpu_clock_speed; // MHz

		long long pwr_usage; // mW
		long long pwr_max_usage = 255000;
		long long pwr_state;

		deque<long long> temp = {0};
		long long temp_max = 110;

		long long mem_total = 0;
		long long mem_used = 0;
		deque<long long> mem_utilization_percent = {0}; // TODO: properly handle GPUs that can't report some stats
		long long mem_clock_speed = 0; // MHz

		long long pcie_tx = 0; // KB/s
		long long pcie_rx = 0;

		long long encoder_utilization = 0;
		long long decoder_utilization = 0;

		gpu_info_supported supported_functions;

		//? topo (Apple Silicon): frequenza massima DVFS, temperatura presente in questo campione, ANE, memoria unificata
		unsigned int gpu_clock_max = 0; // MHz, 0 = ignota
		bool temp_available = true;
		bool unified_memory = false;
		bool show_ane = false;
		std::optional<double> ane_power_w;
		bool ane_power_unverified = false; //? valore CLPC non ancora verificato ("~")
		std::optional<double> ane_usage_estimate_pct;
		deque<long long> ane_percent;  //? potenza ANE in % di ane_power_scale_w (storico per i grafici)
		double ane_power_scale_w = 1.0; //? fondo scala: massimo di sessione, almeno 1 W

		// vector<proc_info> graphics_processes = {}; // TODO
		// vector<proc_info> compute_processes = {};
	};

	namespace Nvml {
		extern bool shutdown();
	}
	namespace Rsmi {
		extern bool shutdown();
	}
	namespace Asysfs {
		extern bool shutdown();
	}
	#ifdef __APPLE__
	namespace AppleSilicon {
		extern bool shutdown();
	}
	#endif

	//* Collect gpu stats and temperatures
    auto collect(bool no_update = false) -> vector<gpu_info>&;

	//* Draw contents of gpu box using <gpus> as source
  	string draw(const gpu_info& gpu, unsigned long index, bool force_redraw, bool data_same);
}

#endif // GPU_SUPPORT

namespace Cpu {
	extern string box;
	extern int x, y, width, height, min_width, min_height;
	extern bool shown, redraw, got_sensors, cpu_temp_only, has_battery, supports_watts;
	extern string cpuName, cpuHz;
	extern vector<string> available_fields;
	extern vector<string> available_sensors;
	extern tuple<int, float, long, string> current_bat;
	extern std::optional<std::string> container_engine;
	//? topo: numero di livelli Apple Silicon (0 altrove): il box cpu raggruppa i core per livello
	extern int apple_cluster_count;

	//? topo: un livello di prestazione Apple Silicon (Efficiency/Performance/Super), riempito solo su macOS
	struct cluster_info {
		string name;
		string label;
		vector<int> cores;
		std::optional<double> freq_mhz;
		std::optional<double> freq_max_mhz;
		std::optional<double> active_pct;
		std::optional<double> temp_c;
	};

	struct cpu_info {
		std::unordered_map<string, deque<long long>> cpu_percent = {
			{"total", {}},
			{"user", {}},
			{"nice", {}},
			{"system", {}},
			{"idle", {}},
			{"iowait", {}},
			{"irq", {}},
			{"softirq", {}},
			{"steal", {}},
			{"guest", {}},
			{"guest_nice", {}}
		};
		vector<deque<long long>> core_percent;
		vector<deque<long long>> temp;
		long long temp_max = 0;
		array<double, 3> load_avg;
		float usage_watts = 0;
		std::optional<std::vector<std::int32_t>> active_cpus;
		vector<cluster_info> clusters; //? topo: vuoto fuori da Apple Silicon
		int watts_state = 0; //? topo: 0 valore normale, 1 non verificato ("~"), 2 assente ("—")
	};

	//* Collect cpu stats and temperatures
	auto collect(bool no_update = false) -> cpu_info&;

	//* Draw contents of cpu box using <cpu> as source
    string draw(
		const cpu_info& cpu,
#if defined(GPU_SUPPORT)
		const vector<Gpu::gpu_info>& gpu,
#endif
		bool force_redraw = false,
		bool data_same = false
	);

	//* Parse /proc/cpu info for mapping of core ids
	auto get_core_mapping() -> std::unordered_map<int, int>;
	extern std::unordered_map<int, int> core_mapping;

	auto get_cpuHz() -> string;

	//* Get battery info from /sys
	auto get_battery() -> tuple<int, float, long, string>;

	string trim_name(string);
}

namespace Mem {
	extern string box;
	extern int x, y, width, height, min_width, min_height;
	extern bool has_swap, shown, redraw;
	const array mem_names { "used"s, "available"s, "cached"s, "free"s };
	const array swap_names { "swap_used"s, "swap_free"s };
	extern int disk_ios;

	struct disk_info {
		std::filesystem::path dev;
		string name;
		string fstype{};                // defaults to ""
		std::filesystem::path stat{};   // defaults to ""
		int64_t total{};
		int64_t used{};
		int64_t free{};
		int used_percent{};
		int free_percent{};

		array<int64_t, 3> old_io = {0, 0, 0};
		deque<long long> io_read = {};
		deque<long long> io_write = {};
		deque<long long> io_activity = {};
	};

	struct mem_info {
		std::unordered_map<string, uint64_t> stats =
			{{"used", 0}, {"available", 0}, {"cached", 0}, {"free", 0},
			{"swap_total", 0}, {"swap_used", 0}, {"swap_free", 0}};
		std::unordered_map<string, deque<long long>> percent =
			{{"used", {}}, {"available", {}}, {"cached", {}}, {"free", {}},
			{"swap_total", {}}, {"swap_used", {}}, {"swap_free", {}}};
		std::unordered_map<string, disk_info> disks;
		vector<string> disks_order;
		//? topo (Apple Silicon): banda DRAM da contatori AMC, oppure stima PMP
		std::optional<double> dram_bw_gbs;
		std::optional<double> dram_bw_estimate_gbs;
	};

	//?* Get total system memory
	uint64_t get_totalMem();

	//* Collect mem & disks stats
	auto collect(bool no_update = false) -> mem_info&;

	//* Draw contents of mem box using <mem> as source
	string draw(const mem_info& mem, bool force_redraw = false, bool data_same = false);

}

//? topo: box pwr (potenze del SoC Apple Silicon), solo dove c'e' la raccolta AppleSoc
#if defined(__APPLE__) && defined(GPU_SUPPORT)
#define SOC_PWR_BOX 1
namespace Pwr {
	extern string box;
	extern int x, y, width, height, min_width, min_height;
	extern bool shown, redraw;

	//? serie: cpu, gpu, ane, dram, sistema (SMC), totale (cpu+gpu+ane+dram)
	constexpr size_t series_count = 6;
	extern const array<string, series_count> series_names;

	struct pwr_info {
		array<deque<long long>, series_count> history; //? mW, per i grafici
		array<std::optional<double>, series_count> current; //? W, assente = "—"
		array<double, series_count> session_max {};      //? W
		array<bool, series_count> available {};          //? la serie e' mai arrivata
		array<bool, series_count> unverified {};         //? valore CLPC non ancora verificato ("~")
		std::optional<string> thermal_state;
	};

	//? potenza GPU o ANE mai arrivata nel box pwr: solo allora il box gpu toglie la sua riga PWR e i watt ANE.
	//? gpu_data_reset chiede al runner il ricalcolo delle dimensioni quando il dato cambia (come coreNum_reset).
	extern atomic<bool> has_gpu_data, gpu_data_reset;

	auto collect(bool no_update = false) -> pwr_info&;
	string draw(const pwr_info& pwr, bool force_redraw = false, bool data_same = false);
}
namespace Gpu {
	//? ricalcola show_ane e l'altezza del box gpu dalle opzioni (avvio, e runner quando apple_options_dirty)
	void apply_apple_options();
	//? impostato dal menu opzioni, consumato dal thread runner a inizio ciclo
	extern atomic<bool> apple_options_dirty;
}
#endif

namespace Net {
	extern string box;
	extern int x, y, width, height, min_width, min_height;
	extern bool shown, redraw;
	extern string selected_iface;
	extern vector<string> interfaces;
	extern bool rescale;
	extern std::unordered_map<string, uint64_t> graph_max;

	struct net_stat {
		uint64_t speed{};
		uint64_t top{};
		uint64_t total{};
		uint64_t last{};
		uint64_t offset{};
		uint64_t rollover{};
	};

	struct net_info {
		std::unordered_map<string, deque<long long>> bandwidth = { {"download", {}}, {"upload", {}} };
		std::unordered_map<string, net_stat> stat = { {"download", {}}, {"upload", {}} };
		string ipv4{};      // defaults to ""
		string ipv6{};      // defaults to ""
		bool connected{};
	};

	class IfAddrsPtr {
		struct ifaddrs* ifaddr;
		int status;
	public:
		IfAddrsPtr() { status = getifaddrs(&ifaddr); }
		~IfAddrsPtr() noexcept { freeifaddrs(ifaddr); }
		IfAddrsPtr(const IfAddrsPtr &) = delete;
		IfAddrsPtr& operator=(IfAddrsPtr& other) = delete;
		IfAddrsPtr(IfAddrsPtr &&) = delete;
		IfAddrsPtr& operator=(IfAddrsPtr&& other) = delete;
		[[nodiscard]] constexpr auto operator()() -> struct ifaddrs* { return ifaddr; }
		[[nodiscard]] constexpr auto get() -> struct ifaddrs* { return ifaddr; }
		[[nodiscard]] constexpr auto get_status() const noexcept -> int { return status; };
	};

	extern std::unordered_map<string, net_info> current_net;

	//* Collect net upload/download stats
	auto collect(bool no_update=false) -> net_info&;

	//* Draw contents of net box using <net> as source
	string draw(const net_info& net, bool force_redraw = false, bool data_same = false);
}

namespace Proc {
	extern atomic<int> numpids;

	extern string box;
	extern int x, y, width, height, min_width, min_height;
	extern bool shown, redraw;
	extern int select_max;
	extern atomic<int> detailed_pid;
	extern int selected_pid, start, selected, collapse, expand, filter_found, selected_depth, toggle_children, collapse_all;
	extern int scroll_pos;
	extern string selected_name;
	extern atomic<bool> resized;

	//? Contains the valid sorting options for processes
	extern const vector<string> sort_vector;

	//? topo: true se la raccolta Gpu% per processo e' attiva (opzione proc_gpu o ordinamento "gpu")
	bool gpu_active();

	//? topo: true se la raccolta dei watt per processo e' attiva (opzione proc_power o ordinamento "power");
	//? accende anche la raccolta Gpu% (serve alla quota GPU), non la sua colonna
	bool power_active();

	//? topo: watt per la colonna W e i dettagli: "0.42", "3.1", "12" (al massimo 5 caratteri con "~"), vuoto se < 0
	string format_watts(double w, bool partial = false);
	//? topo: watt CPU+GPU per i dettagli ("2.1+0.42", decimali per ciascuna parte); "+GPU" omesso se gpu_w < 0
	//? (nessun client GPU); senza parte CPU (primo campione o illeggibile) "~0.80 GPU"
	string format_watts_split(double cpu_w, double gpu_w);

	//? topo: quota non attribuita ai processi e somma dei processi (W) dell'ultimo ciclo, -1 = assente
	extern double pwr_system_w, pwr_procs_w;

	//? Translation from process state char to explanative string
	extern const std::unordered_map<char, string> proc_states;

	//* Container for process information
	struct proc_info {
		size_t pid{};
		string name{};          // defaults to ""
		string cmd{};           // defaults to ""
		string short_cmd{};     // defaults to ""
		size_t threads{};
		int name_offset{};
		string user{};          // defaults to ""
		uint64_t mem{};
		double cpu_p{};         // defaults to = 0.0
		double cpu_c{};         // defaults to = 0.0
		char state = '0';
		int64_t p_nice{};
		uint64_t ppid{};
		uint64_t cpu_s{};
		uint64_t cpu_t{};
		uint64_t death_time{};
		string prefix{};        // defaults to ""
		size_t depth{};
		size_t tree_index{};
		bool collapsed{};
		bool filtered{};
		double gpu_p = -1;      //? topo: GPU% (Apple Silicon, opzione proc_gpu); -1 = nessun client GPU
		//? topo: watt (Apple Silicon, opzione proc_power); -1 = nessun valore. pwr_w = somma delle parti note.
		//? pwr_cpu_unknown: parte CPU illeggibile (processo di un altro utente), diverso dal primo campione senza
		//? valore; in albero vale anche per il sottoalbero. pwr_partial: valore mostrato con "~" (manca una parte CPU)
		double pwr_cpu_w = -1;
		double pwr_gpu_w = -1;
		double pwr_w = -1;
		bool pwr_cpu_unknown{};
		bool pwr_partial{};
	};

	//* Container for process info box
	struct detail_container {
		size_t last_pid{};
		bool skip_smaps{};
		proc_info entry;
		string elapsed, parent, status, io_read, io_write, memory;
		string gpu_time; //? topo: tempo GPU accumulato (Apple Silicon), vuoto se raccolta spenta o 0
		string pwr_split; //? topo: watt CPU+GPU (Apple Silicon), vuoto se raccolta spenta o senza valore
		long long first_mem = -1;
		deque<long long> cpu_percent;
		deque<long long> mem_bytes;
	};

	//? Contains all info for proc detailed box
	extern detail_container detailed;

	//* Collect and sort process information from /proc
	auto collect(bool no_update = false) -> vector<proc_info>&;

	//* Update current selection and view, returns -1 if no change otherwise the current selection
	int selection(const std::string_view cmd_key);

	//* Draw contents of proc box using <plist> as data source
	string draw(const vector<proc_info>& plist, bool force_redraw = false, bool data_same = false);

	struct tree_proc {
		std::reference_wrapper<proc_info> entry;
		vector<tree_proc> children;
	};

	//? topo: completa pwr_w e pwr_partial dalle parti pwr_cpu_w e pwr_gpu_w
	void combine_power(proc_info& p);
	//? topo: somma i watt del figlio nel padre (albero chiuso o aggregato): -1 resta solo se nessuno ha valori;
	//? il padre e' parziale se un processo del sottoalbero (lui compreso) ha la parte CPU illeggibile
	void add_power(proc_info& parent, const proc_info& child);

	//* Change priority (nice) of pid, returns true on success otherwise false
	bool set_priority(pid_t pid, int priority);

	//* Sort vector of proc_info's
	void proc_sorter(vector<proc_info>& proc_vec, const string& sorting, bool reverse, bool tree = false);

	//* Recursive sort of process tree
	void tree_sort(vector<tree_proc>& proc_vec, const string& sorting, bool reverse, bool paused,
					int& c_index, const int index_max, bool collapsed = false);

	auto matches_filter(const proc_info& proc, const std::string& filter) -> bool;

	//* Generate process tree list
	void _tree_gen(proc_info& cur_proc, vector<proc_info>& in_procs, vector<tree_proc>& out_procs,
				   int cur_depth, bool collapsed, const string& filter,
				   bool found = false, bool no_update = false, bool should_filter = false);

	//* Build prefixes for tree view
	void _collect_prefixes(tree_proc& t, bool is_last, const string &header = "");

	//* Toggle collapse/expand of all tree entries
	void toggle_tree_collapse(std::vector<proc_info>& current_procs);

	//* Auto-collapse processes with many direct children when entering tree mode
	void _auto_collapse_oversized(std::vector<proc_info>& current_procs, const bool tree_mode_change);
}

/// Detect container engine.
auto detect_container() -> std::optional<std::string>;
