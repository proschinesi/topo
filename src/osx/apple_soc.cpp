/* Copyright 2026 the sitop contributors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

	   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

   Modified for sitop (fork of btop for Apple Silicon), 2026: see NOTICE.
   Sottoscrizione IOReport unica, canali CLPC costruiti a mano e tabelle DVFS come in
   macmon (MIT, Copyright (c) 2024 vladkens); potenza di sistema/DRAM da SMC, banda PMP
   e floor ANE come in mactop (MIT, Copyright (c) 2024-2026 Carsen Klock).

indent = tab
tab-size = 4
*/

//? Parte IOKit di AppleSoc: raccoglie i dati grezzi e li passa alle funzioni pure (apple_soc_pure.cpp).

#include "apple_soc.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/mach_time.h>
#include <mach/processor_info.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

#include <fmt/format.h>

#include "sensors.hpp"
#include "smc.hpp"

#if __MAC_OS_X_VERSION_MIN_REQUIRED < 120000
#define kIOMainPortDefault kIOMasterPortDefault
#endif

extern "C" {
	typedef struct IOReportSubscription* IOReportSubscriptionRef;
	CFDictionaryRef IOReportCopyAllChannels(uint64_t a, uint64_t b);
	IOReportSubscriptionRef IOReportCreateSubscription(void* a, CFMutableDictionaryRef b,
		CFMutableDictionaryRef* c, uint64_t d, CFTypeRef cfnull);
	CFDictionaryRef IOReportCreateSamples(IOReportSubscriptionRef sub, CFMutableDictionaryRef chan, CFTypeRef cfnull);
	CFDictionaryRef IOReportCreateSamplesDelta(CFDictionaryRef a, CFDictionaryRef b, CFTypeRef cfnull);
	CFStringRef IOReportChannelGetGroup(CFDictionaryRef item);
	CFStringRef IOReportChannelGetSubGroup(CFDictionaryRef item);
	CFStringRef IOReportChannelGetChannelName(CFDictionaryRef item);
	CFStringRef IOReportChannelGetUnitLabel(CFDictionaryRef item);
	int IOReportChannelGetFormat(CFDictionaryRef item);
	int64_t IOReportSimpleGetIntegerValue(CFDictionaryRef item, int32_t idx);
	int32_t IOReportStateGetCount(CFDictionaryRef item);
	CFStringRef IOReportStateGetNameForIndex(CFDictionaryRef item, int32_t idx);
	int64_t IOReportStateGetResidency(CFDictionaryRef item, int32_t idx);

	int sitop_thermal_state(void);
}

namespace AppleSoc {
	using namespace Pure;

	namespace {
		constexpr int kFormatSimple = 1;
		constexpr int kFormatState = 2;

		std::string cfstr(CFStringRef s) {
			if (not s) return {};
			char buf[256];
			if (CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return buf;
			return {};
		}

		std::optional<std::string> sysctl_str(const char* name) {
			size_t size = 0;
			if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 or size == 0) return std::nullopt;
			std::string buf(size, '\0');
			if (sysctlbyname(name, buf.data(), &size, nullptr, 0) != 0) return std::nullopt;
			while (not buf.empty() and buf.back() == '\0') buf.pop_back();
			return buf;
		}

		std::optional<int> sysctl_int(const char* name) {
			int32_t v = 0;
			size_t size = sizeof v;
			if (sysctlbyname(name, &v, &size, nullptr, 0) != 0) return std::nullopt;
			return v;
		}

		uint64_t now_ns() {
			static mach_timebase_info_data_t tb = {0, 0};
			if (tb.denom == 0) mach_timebase_info(&tb);
			return mach_absolute_time() * tb.numer / tb.denom;
		}

		uint64_t unix_ms() {
			struct timeval tv;
			gettimeofday(&tv, nullptr);
			return static_cast<uint64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
		}

		States read_states(CFDictionaryRef item) {
			States states;
			const int32_t n = IOReportStateGetCount(item);
			states.reserve(std::max(0, n));
			for (int32_t i = 0; i < n; i++)
				states.emplace_back(cfstr(IOReportStateGetNameForIndex(item, i)), IOReportStateGetResidency(item, i));
			return states;
		}

		//? tutte le proprieta' CFData di un nodo IORegistry
		std::map<std::string, std::vector<uint8_t>> data_props(io_object_t entry) {
			std::map<std::string, std::vector<uint8_t>> out;
			CFMutableDictionaryRef props = nullptr;
			if (IORegistryEntryCreateCFProperties(entry, &props, kCFAllocatorDefault, 0) != kIOReturnSuccess or not props) return out;
			const CFIndex n = CFDictionaryGetCount(props);
			std::vector<const void*> keys(n), values(n);
			CFDictionaryGetKeysAndValues(props, keys.data(), values.data());
			for (CFIndex i = 0; i < n; i++) {
				if (CFGetTypeID((CFTypeRef)keys[i]) != CFStringGetTypeID() or CFGetTypeID((CFTypeRef)values[i]) != CFDataGetTypeID()) continue;
				auto data = (CFDataRef)values[i];
				const auto* p = CFDataGetBytePtr(data);
				out[cfstr((CFStringRef)keys[i])] = std::vector<uint8_t>(p, p + CFDataGetLength(data));
			}
			CFRelease(props);
			return out;
		}

		struct State {
			bool tried = false;
			bool ok = false;
			std::string chip, os_version;
			std::optional<int> os;

			std::vector<Tier> tiers;
			std::vector<std::vector<uint32_t>> tier_freqs;
			std::vector<uint32_t> gpu_freqs;
			bool has_super = false;

			std::string clpc_bundle;
			bool clpc_channels = false;

			IOReportSubscriptionRef sub = nullptr;
			CFMutableDictionaryRef chan = nullptr;       //? richiesta
			CFMutableDictionaryRef subscribed = nullptr; //? canali effettivi della sottoscrizione
			CFDictionaryRef prev = nullptr;
			uint64_t prev_ns = 0;

			std::unique_ptr<Cpu::SMCConnection> smc;
			std::map<char, std::vector<std::string>> smc_temp_keys;
			std::vector<std::string> smc_dram_keys;

			std::unique_ptr<Cpu::HidThermal> hid;

			std::vector<TempFilter> tier_filters;
			TempFilter gpu_filter;

			std::vector<std::array<uint64_t, 2>> prev_ticks; //? (attivi, totali) per cpu
			Sample last;
			uint64_t last_ns = 0;
		};

		State& st() {
			static State s;
			return s;
		}

		//* ---- inizializzazione ----

		void init_topology(State& S) {
			const int n = sysctl_int("hw.nperflevels").value_or(0);
			std::vector<PerfLevel> levels;
			for (int i = 0; i < n; i++) {
				auto name = sysctl_str(fmt::format("hw.perflevel{}.name", i).c_str()).value_or("");
				auto cpus = sysctl_int(fmt::format("hw.perflevel{}.logicalcpu", i).c_str()).value_or(0);
				levels.push_back(PerfLevel{.name = name, .logical_cpus = cpus});
			}

			//? cluster-type dei nodi cpuN (E, M, P) per sapere a quale livello appartiene ogni core di btop
			std::vector<char> types;
			io_iterator_t it;
			if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOPlatformDevice"), &it) == kIOReturnSuccess) {
				std::map<int, char> by_index;
				io_object_t entry;
				while ((entry = IOIteratorNext(it)) != 0) {
					io_name_t name;
					if (IORegistryEntryGetName(entry, name) == kIOReturnSuccess and strncmp(name, "cpu", 3) == 0 and std::isdigit(static_cast<unsigned char>(name[3]))) {
						const int idx = std::atoi(name + 3);
						auto props = data_props(entry);
						char t = 0;
						if (auto p = props.find("cluster-type"); p != props.end() and not p->second.empty()) t = static_cast<char>(p->second[0]);
						by_index[idx] = t;
					}
					IOObjectRelease(entry);
				}
				IOObjectRelease(it);
				int expected = 0;
				for (auto& [idx, t] : by_index) {
					if (idx != expected++) { by_index.clear(); break; }
					types.push_back(t);
				}
			}
			S.tiers = build_tiers(levels, types);
			S.has_super = std::any_of(S.tiers.begin(), S.tiers.end(), [](const Tier& t) { return t.label == "S"; });
		}

		void init_dvfs(State& S) {
			std::map<std::string, std::vector<uint8_t>> pmgr;
			io_iterator_t it;
			if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("AppleARMIODevice"), &it) == kIOReturnSuccess) {
				io_object_t entry;
				while ((entry = IOIteratorNext(it)) != 0) {
					io_name_t name;
					//? M1-M5: tabelle su "pmgr"; M6+: su "pmgr-child". Vince la prima tabella trovata.
					if (IORegistryEntryGetName(entry, name) == kIOReturnSuccess and (strcmp(name, "pmgr") == 0 or strcmp(name, "pmgr-child") == 0)) {
						for (auto& [k, v] : data_props(entry))
							if (not pmgr.contains(k)) pmgr[k] = std::move(v);
					}
					IOObjectRelease(entry);
				}
				IOObjectRelease(it);
			}
			std::vector<std::pair<int, int>> acc;
			if (auto p = pmgr.find("acc-clusters"); p != pmgr.end()) acc = parse_acc_clusters(p->second);
			const auto keys = dvfs_keys_for_tiers(S.tiers.size(), acc);
			S.tier_freqs.assign(S.tiers.size(), {});
			for (size_t i = 0; i < keys.size(); i++) {
				if (auto p = pmgr.find(keys[i]); p != pmgr.end())
					S.tier_freqs[i] = dvfs_to_mhz(parse_dvfs(p->second));
			}
			if (auto p = pmgr.find("voltage-states9"); p != pmgr.end())
				S.gpu_freqs = dvfs_to_mhz(parse_dvfs(p->second));
		}

		CFMutableDictionaryRef clpc_channel(uint64_t driver, uint64_t id, std::string_view name) {
			//? IOReportTypes.h: intero semplice, categoria potenza, un elemento; energia in nJ (come macmon)
			const int64_t type = 1 | (2LL << 16) | (1LL << 32);
			const int64_t unit = (3LL << 56) | (118LL << 32);
			const int64_t idv = static_cast<int64_t>(id), drv = static_cast<int64_t>(driver);
			CFNumberRef n_id = CFNumberCreate(nullptr, kCFNumberSInt64Type, &idv);
			CFNumberRef n_type = CFNumberCreate(nullptr, kCFNumberSInt64Type, &type);
			CFNumberRef n_unit = CFNumberCreate(nullptr, kCFNumberSInt64Type, &unit);
			CFNumberRef n_drv = CFNumberCreate(nullptr, kCFNumberSInt64Type, &drv);
			CFStringRef s_name = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(name.data()), static_cast<CFIndex>(name.size()), kCFStringEncodingUTF8, false);
			const void* legend_v[] = {n_id, n_type, s_name};
			CFArrayRef legend = CFArrayCreate(nullptr, legend_v, 3, &kCFTypeArrayCallBacks);
			const void* info_k[] = {CFSTR("IOReportChannelUnit")};
			const void* info_v[] = {n_unit};
			CFDictionaryRef info = CFDictionaryCreate(nullptr, info_k, info_v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			CFMutableDictionaryRef ch = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			CFDictionarySetValue(ch, CFSTR("DriverID"), n_drv);
			CFDictionarySetValue(ch, CFSTR("DriverName"), CFSTR("AppleCLPC"));
			CFDictionarySetValue(ch, CFSTR("IOReportGroupName"), CFSTR("CLPC"));
			CFDictionarySetValue(ch, CFSTR("IOReportSubGroupName"), CFSTR("Energy Counters"));
			CFDictionarySetValue(ch, CFSTR("IOReportChannelInfo"), info);
			CFDictionarySetValue(ch, CFSTR("LegendChannel"), legend);
			for (CFTypeRef r : {(CFTypeRef)n_id, (CFTypeRef)n_type, (CFTypeRef)n_unit, (CFTypeRef)n_drv, (CFTypeRef)s_name, (CFTypeRef)legend, (CFTypeRef)info})
				CFRelease(r);
			return ch;
		}

		void add_clpc_channels(State& S, CFMutableArrayRef selected) {
			io_iterator_t it;
			if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("AppleCLPC"), &it) != kIOReturnSuccess) return;
			io_object_t entry;
			while ((entry = IOIteratorNext(it)) != 0) {
				uint64_t driver = 0;
				std::string bundle;
				if (IORegistryEntryGetRegistryEntryID(entry, &driver) == kIOReturnSuccess) {
					CFTypeRef b = IORegistryEntryCreateCFProperty(entry, CFSTR("CFBundleIdentifier"), kCFAllocatorDefault, 0);
					if (b) {
						if (CFGetTypeID(b) == CFStringGetTypeID()) bundle = cfstr((CFStringRef)b);
						CFRelease(b);
					}
				}
				IOObjectRelease(entry);
				if (bundle.empty()) continue;
				if (S.clpc_bundle.empty()) S.clpc_bundle = bundle;
				auto channels = clpc_energy_channels(bundle, S.os_version);
				if (not channels) continue;
				for (const auto& c : *channels) {
					auto ch = clpc_channel(driver, c.id, c.name);
					CFArrayAppendValue(selected, ch);
					CFRelease(ch);
				}
				S.clpc_channels = true;
			}
			IOObjectRelease(it);
		}

		bool want_channel(const std::string& g, const std::string& sg, const std::string& c) {
			if (g == "CPU Stats") return sg == "CPU Core Performance States";
			if (g == "GPU Stats") return sg == "GPU Performance States";
			if (g == "Energy Model") return c == "GPU Energy" or c.ends_with("CPU Energy") or c.starts_with("ANE") or c.starts_with("DRAM");
			if (g == "AMC Stats") return c == "DCS RD" or c == "DCS WR";
			if (g.starts_with("PMP")) {
				if (sg == "DCS BW" and c == "AMCC RD+WR") return true;
				if (sg.find("Floor") != std::string::npos and c.find("ANE") != std::string::npos) return true;
			}
			return false;
		}

		bool init_ioreport(State& S) {
			CFDictionaryRef all = IOReportCopyAllChannels(0, 0);
			if (not all) return false;
			auto arr = (CFArrayRef)CFDictionaryGetValue(all, CFSTR("IOReportChannels"));
			if (not arr) { CFRelease(all); return false; }
			CFMutableArrayRef selected = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
			for (CFIndex i = 0; i < CFArrayGetCount(arr); i++) {
				auto item = (CFDictionaryRef)CFArrayGetValueAtIndex(arr, i);
				if (want_channel(cfstr(IOReportChannelGetGroup(item)), cfstr(IOReportChannelGetSubGroup(item)), cfstr(IOReportChannelGetChannelName(item))))
					CFArrayAppendValue(selected, item);
			}
			add_clpc_channels(S, selected);
			S.chan = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, all);
			CFDictionarySetValue(S.chan, CFSTR("IOReportChannels"), selected);
			CFRelease(selected);
			CFRelease(all);
			S.sub = IOReportCreateSubscription(nullptr, S.chan, &S.subscribed, 0, nullptr);
			if (not S.sub) {
				CFRelease(S.chan);
				S.chan = nullptr;
				return false;
			}
			return true;
		}

		void init_smc(State& S) {
			try {
				S.smc = std::make_unique<Cpu::SMCConnection>();
			} catch (const std::runtime_error&) {
				S.smc.reset();
				return;
			}
			//? chiavi di temperatura per livello/GPU (solo "flt "), al massimo 64 per classe per contenere il costo
			for (const auto& key : S.smc->listKeys()) {
				const char cls = classify_smc_temp_key(key, S.has_super);
				if (cls != 0) {
					auto& v = S.smc_temp_keys[cls];
					if (v.size() < 64 and S.smc->keyType(key) == "flt ") v.push_back(key);
				}
			}
			//? Tf* vale per i core P solo dove mancano i Tp* (M3)
			if (auto f = S.smc_temp_keys.find('F'); f != S.smc_temp_keys.end()) {
				if (S.smc_temp_keys['P'].empty()) S.smc_temp_keys['P'] = f->second;
				S.smc_temp_keys.erase('F');
			}
			if (S.smc_temp_keys.contains('P') and S.smc_temp_keys['P'].empty()) S.smc_temp_keys.erase('P');
			//? DRAM da SMC: PZD1, PZD2... contigui da 1
			for (int i = 1; i <= 8; i++) {
				auto key = fmt::format("PZD{}", i);
				if (not S.smc->keyType(key)) break;
				S.smc_dram_keys.push_back(key);
			}
		}

		//* ---- campionamento ----

		struct Raw {
			std::vector<Residency> tiers;
			std::optional<Residency> gpu;
			std::optional<double> em_cpu, em_gpu, em_ane, em_dram;
			std::optional<double> clpc_cpu, clpc_gpu, clpc_ane;
			std::optional<int64_t> amc_bytes;
			std::optional<double> pmp_bw_gbs;
			std::optional<double> ane_floor_pct;
		};

		void add_opt(std::optional<double>& into, std::optional<double> v) {
			if (v) into = into.value_or(0) + *v;
		}

		void parse_delta(State& S, CFDictionaryRef delta, double dt, Raw& R) {
			auto items = (CFArrayRef)CFDictionaryGetValue(delta, CFSTR("IOReportChannels"));
			if (not items) return;
			R.tiers.assign(S.tiers.size(), Residency{});
			for (CFIndex i = 0; i < CFArrayGetCount(items); i++) {
				auto item = (CFDictionaryRef)CFArrayGetValueAtIndex(items, i);
				const std::string g = cfstr(IOReportChannelGetGroup(item));
				const std::string sg = cfstr(IOReportChannelGetSubGroup(item));
				const std::string c = cfstr(IOReportChannelGetChannelName(item));
				const int fmt = IOReportChannelGetFormat(item);

				if (g == "CPU Stats" and fmt == kFormatState) {
					auto t = tier_for_channel(S.tiers, c);
					if (not t) continue;
					auto states = read_states(item);
					const auto& freqs = S.tier_freqs[*t];
					accumulate(R.tiers[*t], residency(states, freqs_fit(states, freqs) ? std::span<const uint32_t>(freqs) : std::span<const uint32_t>{}));
				} else if (g == "GPU Stats" and c == "GPUPH" and fmt == kFormatState) {
					R.gpu = residency(read_states(item), S.gpu_freqs);
				} else if (g == "Energy Model" and fmt == kFormatSimple) {
					auto w = energy_to_watts(IOReportSimpleGetIntegerValue(item, 0), cfstr(IOReportChannelGetUnitLabel(item)), dt);
					if (c == "GPU Energy") add_opt(R.em_gpu, w);
					else if (c.ends_with("CPU Energy")) add_opt(R.em_cpu, w);
					else if (c.starts_with("ANE")) add_opt(R.em_ane, w);
					else if (c.starts_with("DRAM")) add_opt(R.em_dram, w);
				} else if (g == "CLPC") {
					auto unit = cfstr(IOReportChannelGetUnitLabel(item));
					auto w = energy_to_watts(IOReportSimpleGetIntegerValue(item, 0), unit.empty() ? "nJ" : unit, dt);
					if (c == "CPU Energy") add_opt(R.clpc_cpu, w);
					else if (c == "GPU Energy") add_opt(R.clpc_gpu, w);
					else if (c == "ANE") add_opt(R.clpc_ane, w);
				} else if (g == "AMC Stats" and fmt == kFormatSimple) {
					const int64_t v = IOReportSimpleGetIntegerValue(item, 0);
					if (v >= 0) R.amc_bytes = R.amc_bytes.value_or(0) + v;
				} else if (g.starts_with("PMP") and fmt == kFormatState) {
					auto states = read_states(item);
					if (sg == "DCS BW") {
						if (auto v = histogram_avg_gbs(states)) R.pmp_bw_gbs = std::max(R.pmp_bw_gbs.value_or(0), *v);
					} else if (auto v = floor_active_pct(states)) {
						R.ane_floor_pct = std::max(R.ane_floor_pct.value_or(0), *v);
					}
				}
			}
		}

		//? su macOS 27 i contatori Energy Model di CPU/ANE/DRAM sono bloccati e restano a 0: uno 0 li', e' un dato assente
		std::optional<double> em_usable(const State& S, std::optional<double> v) {
			if (not v) return std::nullopt;
			if (*v > 0 or (S.os and *S.os < 27)) return v;
			return std::nullopt;
		}

		void fill_power(State& S, const Raw& R, Sample& s) {
			if (R.clpc_cpu) { s.cpu_power_w = R.clpc_cpu; s.sources["cpu_power"] = "clpc"; }
			else if (auto v = em_usable(S, R.em_cpu)) { s.cpu_power_w = v; s.sources["cpu_power"] = "energy_model"; }

			if (R.em_gpu and (*R.em_gpu > 0 or not R.clpc_gpu)) { s.gpu_power_w = R.em_gpu; s.sources["gpu_power"] = "energy_model"; }
			else if (R.clpc_gpu) { s.gpu_power_w = R.clpc_gpu; s.sources["gpu_power"] = "clpc"; }

			if (R.clpc_ane) { s.ane_power_w = R.clpc_ane; s.sources["ane_power"] = "clpc"; }
			else if (auto v = em_usable(S, R.em_ane)) { s.ane_power_w = v; s.sources["ane_power"] = "energy_model"; }

			if (auto v = em_usable(S, R.em_dram)) { s.dram_power_w = v; s.sources["dram_power"] = "energy_model"; }
		}

		void fill_smc(State& S, Sample& s) {
			if (not S.smc) return;
			//? potenza di sistema: PSTR (totale), PDTR (DC-in), PD0R (binario DC-in, Mac Studio); vince il primo plausibile
			for (const char* key : {"PSTR", "PDTR", "PD0R"}) {
				if (auto v = S.smc->readValue(key); v and plausible_power(*v)) {
					s.system_power_w = v;
					s.sources["system_power"] = std::string("smc ") + key;
					break;
				}
			}
			if (not s.dram_power_w and not S.smc_dram_keys.empty()) {
				std::optional<double> sum;
				for (const auto& key : S.smc_dram_keys)
					if (auto v = S.smc->readValue(key); v and plausible_power(*v)) sum = sum.value_or(0) + *v;
				if (sum) { s.dram_power_w = sum; s.sources["dram_power"] = "smc PZD"; }
			}
		}

		std::optional<double> smc_avg(State& S, char cls) {
			if (not S.smc) return std::nullopt;
			auto it = S.smc_temp_keys.find(cls);
			if (it == S.smc_temp_keys.end()) return std::nullopt;
			std::vector<double> values;
			for (const auto& key : it->second)
				if (auto v = S.smc->readValue(key)) values.push_back(*v);
			return average_valid(values);
		}

		void fill_temps(State& S, Sample& s, double dt) {
			std::map<char, std::vector<double>> hid;
			if (S.hid)
				for (const auto& [name, value] : S.hid->read()) hid[classify_hid_sensor(name)].push_back(value);

			for (size_t i = 0; i < s.clusters.size(); i++) {
				const char cls = s.clusters[i].label.empty() ? 0 : s.clusters[i].label[0];
				std::optional<double> v;
				std::string src;
				if (auto h = hid.find(cls); h != hid.end()) { v = average_valid(h->second); src = "hid"; }
				if (not v) { v = smc_avg(S, cls); src = "smc"; }
				s.clusters[i].temp_c = S.tier_filters[i].feed(v, dt);
				if (s.clusters[i].temp_c) s.sources["temp_" + s.clusters[i].label] = src;
			}
			std::optional<double> g;
			std::string src;
			if (auto h = hid.find('G'); h != hid.end()) { g = average_valid(h->second); src = "hid"; }
			if (not g) { g = smc_avg(S, 'G'); src = "smc"; }
			s.gpu_temp_c = S.gpu_filter.feed(g, dt);
			if (s.gpu_temp_c) s.sources["gpu_temp"] = src;
		}

		void fill_ticks(State& S, Sample& s) {
			natural_t count = 0;
			processor_info_array_t info = nullptr;
			mach_msg_type_number_t info_count = 0;
			if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &count, &info, &info_count) != KERN_SUCCESS) return;
			auto* load = reinterpret_cast<processor_cpu_load_info_data_t*>(info);
			std::vector<std::array<uint64_t, 2>> cur(count);
			for (natural_t i = 0; i < count; i++) {
				const auto& t = load[i].cpu_ticks;
				const uint64_t active = static_cast<uint64_t>(t[CPU_STATE_USER]) + t[CPU_STATE_SYSTEM] + t[CPU_STATE_NICE];
				cur[i] = {active, active + t[CPU_STATE_IDLE]};
			}
			vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(info), static_cast<vm_size_t>(info_count) * sizeof(integer_t));
			if (S.prev_ticks.size() == cur.size()) {
				for (auto& c : s.clusters) {
					uint64_t active = 0, total = 0;
					for (int core : c.cores) {
						if (core < 0 or static_cast<size_t>(core) >= cur.size()) continue;
						active += cur[core][0] - S.prev_ticks[core][0];
						total += cur[core][1] - S.prev_ticks[core][1];
					}
					if (total > 0) c.ticks_usage_pct = 100.0 * static_cast<double>(active) / static_cast<double>(total);
				}
			}
			S.prev_ticks = std::move(cur);
		}

		Sample take(State& S) {
			Sample s;
			const uint64_t t = now_ns();
			s.timestamp_ms = unix_ms();
			for (size_t i = 0; i < S.tiers.size(); i++) {
				ClusterSample c;
				c.name = S.tiers[i].name;
				c.label = S.tiers[i].label;
				c.cores = S.tiers[i].cores;
				if (not S.tier_freqs[i].empty()) c.freq_max_mhz = S.tier_freqs[i].back();
				s.clusters.push_back(std::move(c));
			}
			if (not S.gpu_freqs.empty()) s.gpu_freq_max_mhz = S.gpu_freqs.back();

			double dt = 0;
			if (S.sub) {
				CFDictionaryRef cur = IOReportCreateSamples(S.sub, S.subscribed ? S.subscribed : S.chan, nullptr);
				if (cur) {
					if (S.prev) {
						dt = static_cast<double>(t - S.prev_ns) / 1e9;
						CFDictionaryRef delta = IOReportCreateSamplesDelta(S.prev, cur, nullptr);
						CFRelease(S.prev);
						if (delta) {
							Raw R;
							parse_delta(S, delta, dt, R);
							CFRelease(delta);
							s.valid = dt > 0;
							s.interval_s = dt;
							for (size_t i = 0; i < R.tiers.size() and i < s.clusters.size(); i++) {
								s.clusters[i].freq_mhz = avg_freq(R.tiers[i]);
								s.clusters[i].active_pct = active_pct(R.tiers[i]);
							}
							if (R.gpu) {
								s.gpu_usage_pct = active_pct(*R.gpu);
								s.gpu_freq_mhz = avg_freq(*R.gpu);
							}
							fill_power(S, R, s);
							if (R.amc_bytes and *R.amc_bytes > 0 and dt > 0) {
								//? 0 byte in un intervallo vuol dire contatori bloccati (c'e' sempre traffico DRAM)
								s.dram_bw_gbs = static_cast<double>(*R.amc_bytes) / dt / 1e9;
								s.sources["dram_bw"] = "amc DCS";
							}
							if (R.pmp_bw_gbs) { s.dram_bw_estimate_gbs = R.pmp_bw_gbs; s.sources["dram_bw_estimate"] = "pmp DCS BW AMCC"; }
							if (R.ane_floor_pct) { s.ane_usage_estimate_pct = R.ane_floor_pct; s.sources["ane_usage_estimate"] = "pmp ANE floor"; }
						}
					}
					S.prev = cur;
					S.prev_ns = t;
				}
			}
			fill_smc(S, s);
			fill_temps(S, s, dt > 0 ? dt : 1.0);
			fill_ticks(S, s);
			if (auto name = thermal_state_name(sitop_thermal_state()); not name.empty()) s.thermal_state = name;
			return s;
		}
	} // namespace

	bool init() {
		auto& S = st();
		if (S.tried) return S.ok;
		S.tried = true;
		S.chip = sysctl_str("machdep.cpu.brand_string").value_or("");
		S.os_version = sysctl_str("kern.osproductversion").value_or("");
		S.os = os_major(S.os_version);
		if (sysctl_int("hw.optional.arm64").value_or(0) != 1) return false;

		init_topology(S);
		if (S.tiers.empty()) return false;
		init_dvfs(S);
		init_ioreport(S);
		init_smc(S);
		S.hid = std::make_unique<Cpu::HidThermal>([](const std::string& n) { return classify_hid_sensor(n) != 0; });
		S.tier_filters.assign(S.tiers.size(), TempFilter{});
		S.ok = true;
		//? campione di base per il primo delta
		S.last = take(S);
		S.last_ns = now_ns();
		return true;
	}

	bool available() {
		return st().ok;
	}

	bool has_cpu_power() {
		const auto& S = st();
		return S.ok and (S.clpc_channels or (S.os and *S.os < 27));
	}

	void shutdown() {
		auto& S = st();
		if (S.prev) { CFRelease(S.prev); S.prev = nullptr; }
		if (S.subscribed) { CFRelease(S.subscribed); S.subscribed = nullptr; }
		if (S.chan) { CFRelease(S.chan); S.chan = nullptr; }
		if (S.sub) { CFRelease((CFTypeRef)S.sub); S.sub = nullptr; }
		S.smc.reset();
		S.hid.reset();
		S.ok = false;
	}

	const Sample& collect(uint64_t max_age_ms) {
		auto& S = st();
		if (not S.ok) return S.last;
		const uint64_t t = now_ns();
		if (S.last_ns != 0 and t - S.last_ns < max_age_ms * 1'000'000ULL) return S.last;
		S.last = take(S);
		S.last_ns = t;
		return S.last;
	}

	const Sample& last() {
		return st().last;
	}

	std::string to_json(const Sample& s) {
		std::string out = fmt::format("{{\"timestamp_ms\":{},\"valid\":{},\"interval_ms\":{}", s.timestamp_ms, s.valid ? "true" : "false", json_number(s.valid ? std::optional<double>(s.interval_s * 1000.0) : std::nullopt, 1));
		const auto& S = st();
		out += fmt::format(",\"chip\":{},\"os\":{},\"clpc_driver\":{}", json_string(S.chip), json_string(S.os_version), S.clpc_bundle.empty() ? "null" : json_string(S.clpc_bundle));
		out += ",\"clusters\":[";
		for (size_t i = 0; i < s.clusters.size(); i++) {
			const auto& c = s.clusters[i];
			if (i) out += ',';
			std::string cores;
			for (size_t k = 0; k < c.cores.size(); k++) cores += (k ? "," : "") + std::to_string(c.cores[k]);
			out += fmt::format("{{\"name\":{},\"label\":{},\"cores\":[{}],\"freq_mhz\":{},\"freq_max_mhz\":{},\"active_pct\":{},\"ticks_usage_pct\":{},\"temp_c\":{}}}",
				json_string(c.name), json_string(c.label), cores, json_number(c.freq_mhz, 0), json_number(c.freq_max_mhz, 0),
				json_number(c.active_pct, 2), json_number(c.ticks_usage_pct, 2), json_number(c.temp_c, 2));
		}
		out += "]";
		out += fmt::format(",\"gpu\":{{\"usage_pct\":{},\"freq_mhz\":{},\"freq_max_mhz\":{},\"power_w\":{},\"temp_c\":{}}}",
			json_number(s.gpu_usage_pct, 2), json_number(s.gpu_freq_mhz, 0), json_number(s.gpu_freq_max_mhz, 0), json_number(s.gpu_power_w, 3), json_number(s.gpu_temp_c, 2));
		out += fmt::format(",\"power\":{{\"cpu_w\":{},\"gpu_w\":{},\"ane_w\":{},\"dram_w\":{},\"system_w\":{}}}",
			json_number(s.cpu_power_w), json_number(s.gpu_power_w), json_number(s.ane_power_w), json_number(s.dram_power_w), json_number(s.system_power_w));
		out += fmt::format(",\"ane\":{{\"power_w\":{},\"usage_estimate_pct\":{}}}", json_number(s.ane_power_w), json_number(s.ane_usage_estimate_pct, 2));
		out += fmt::format(",\"dram_bw_gbs\":{},\"dram_bw_estimate_gbs\":{}", json_number(s.dram_bw_gbs), json_number(s.dram_bw_estimate_gbs));
		out += fmt::format(",\"thermal_state\":{}", s.thermal_state ? json_string(*s.thermal_state) : "null");
		out += ",\"sources\":{";
		bool first = true;
		for (const auto& [k, v] : s.sources) {
			out += fmt::format("{}{}:{}", first ? "" : ",", json_string(k), json_string(v));
			first = false;
		}
		out += "}}";
		return out;
	}

	int dump(uint32_t count, uint32_t interval_ms) {
		if (not init()) {
			std::fputs("sitop: --dump-soc richiede un Mac Apple Silicon\n", stderr);
			return 1;
		}
		count = std::max<uint32_t>(count, 1);
		interval_ms = std::max<uint32_t>(interval_ms, 10);
		uint64_t next = st().last_ns;
		for (uint32_t i = 0; i < count; i++) {
			next += static_cast<uint64_t>(interval_ms) * 1'000'000ULL;
			const uint64_t t = now_ns();
			if (next > t) std::this_thread::sleep_for(std::chrono::nanoseconds(next - t));
			const auto& s = collect(0);
			std::fputs((to_json(s) + "\n").c_str(), stdout);
			std::fflush(stdout);
		}
		return 0;
	}

} // namespace AppleSoc
