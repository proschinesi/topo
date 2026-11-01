/* Copyright 2026 the topo contributors

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
   Tabella CLPC, scala DVFS, acc-clusters e calcolo delle residenze portati da
   macmon (MIT, Copyright (c) 2024 vladkens); istogrammi PMP, floor ANE e chiavi
   SMC di potenza da mactop (MIT, Copyright (c) 2024-2026 Carsen Klock).

indent = tab
tab-size = 4
*/

//? Funzioni pure di AppleSoc: nessuna chiamata IOKit, solo dati in ingresso -> dati in uscita.

#include "apple_soc.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>

#include <fmt/format.h>

namespace AppleSoc::Pure {

	//? ---- CLPC ----
	namespace {
		struct ClpcKeys {
			std::string_view bundle;
			uint32_t cpu, gpu, ane;
		};
		//? macOS 27.0 (26A428) e 27.0.1 (26A434): indici di tabella condivisi da tutti i driver
		constexpr uint32_t INDEX_CPU_27 = 16, INDEX_GPU_27 = 25, INDEX_ANE_27 = 24;
		//? chiavi dei contatori aggregati (inclusi i due die degli Ultra), da macmon src/clpc.rs
		constexpr std::array<ClpcKeys, 17> CLPC_KEYS {{
			{"com.apple.driver.AppleT6000CLPCv3", 0x47479059, 0x4519bf1e, 0x9aaa17f8},
			{"com.apple.driver.AppleT6002CLPC",   0xb501816b, 0x9e2c3e8b, 0x14d5a574},
			{"com.apple.driver.AppleT6020CLPC",   0xe301cd75, 0x8f34cc7a, 0x7415fb0f},
			{"com.apple.driver.AppleT6022CLPC",   0x76f5fd21, 0x90624499, 0x3c097307},
			{"com.apple.driver.AppleT6030CLPC",   0xbe03a01a, 0x472d6c9c, 0xb8990c38},
			{"com.apple.driver.AppleT6031CLPC",   0xd81e0084, 0x1085f98b, 0x313a3be1},
			{"com.apple.driver.AppleT6032CLPC",   0xf4a21308, 0x85e80513, 0x63dc8e25},
			{"com.apple.driver.AppleT6041CLPC",   0x263cf1f0, 0x22e4cf59, 0x7849fb5c},
			{"com.apple.driver.AppleT6050CLPC",   0xd3ab60bb, 0x27cd4ce5, 0x8c0f1c59},
			{"com.apple.driver.AppleT6050dCLPC",  0x18b3010e, 0xedeca11a, 0x188bd883},
			{"com.apple.driver.AppleT8103CLPCv3", 0x2cdd31f2, 0x21becf42, 0x0667fb81},
			{"com.apple.driver.AppleT8112CLPC",   0xb5435137, 0x638d9d52, 0xea089c36},
			{"com.apple.driver.AppleT8122CLPC",   0x54b51c99, 0xce2dd9dc, 0x960afe2f},
			{"com.apple.driver.AppleT8132CLPC",   0x4a854d94, 0x3b3e3b79, 0x1ff4a9ec},
			{"com.apple.driver.AppleT8140CLPC",   0x9bf54436, 0xef70589c, 0x569d941b},
			{"com.apple.driver.AppleT8142CLPC",   0x43708744, 0x6a010933, 0xbc5efdf0},
			{"com.apple.driver.AppleT8152CLPC",   0x485d4bfc, 0x11784654, 0x9380c6bb},
		}};
		constexpr uint64_t report_id(uint32_t index, uint32_t key) {
			return (static_cast<uint64_t>(index) << 32) | key;
		}
	}

	auto os_major(std::string_view os_version) -> std::optional<int> {
		int value = 0;
		size_t i = 0;
		for (; i < os_version.size() and std::isdigit(static_cast<unsigned char>(os_version[i])); i++) {
			value = value * 10 + (os_version[i] - '0');
			if (value > 1000) return std::nullopt;
		}
		if (i == 0 or (i < os_version.size() and os_version[i] != '.')) return std::nullopt;
		return value;
	}

	auto clpc_energy_channels(std::string_view bundle, std::string_view os_version) -> std::optional<std::array<ClpcChannel, 3>> {
		if (os_major(os_version) != 27) return std::nullopt;
		for (const auto& k : CLPC_KEYS) {
			if (k.bundle == bundle) {
				return std::array<ClpcChannel, 3> {{
					{report_id(INDEX_CPU_27, k.cpu), "CPU Energy"},
					{report_id(INDEX_GPU_27, k.gpu), "GPU Energy"},
					{report_id(INDEX_ANE_27, k.ane), "ANE"},
				}};
			}
		}
		return std::nullopt;
	}

	bool clpc_build_verified(std::string_view os_build) {
		return os_build == "26A428" or os_build == "26A434";
	}

	auto ClpcValidator::feed(std::optional<double> clpc_cpu, std::optional<double> clpc_gpu, std::optional<double> clpc_ane,
		std::optional<double> em_gpu) -> ClpcCheck {
		if (state_ == ClpcCheck::rejected) return state_;
		//? -1 discorde, 0 non confrontabile, +1 concorde
		int verdict = 0;
		for (auto v : {clpc_cpu, clpc_gpu, clpc_ane})
			if (v and (not std::isfinite(*v) or *v > max_plausible_w)) verdict = -1;
		if (verdict == 0 and clpc_gpu and em_gpu) {
			if (*em_gpu >= min_w) {
				const double ratio = *clpc_gpu / *em_gpu;
				verdict = (ratio >= ratio_lo and ratio <= ratio_hi) ? 1 : -1;
			} else if (*clpc_gpu >= 4 * min_w and *clpc_gpu > 4 * *em_gpu) {
				//? GPU ferma per l'Energy Model ma non per CLPC: indici sbagliati
				verdict = -1;
			}
		}
		if (verdict > 0) {
			disagree_ = 0;
			if (++agree_ >= verify_after) state_ = ClpcCheck::verified;
		} else if (verdict < 0) {
			if (++disagree_ >= reject_after) state_ = ClpcCheck::rejected;
		}
		return state_;
	}

	std::string clpc_check_name(ClpcCheck c) {
		switch (c) {
			case ClpcCheck::pending: return "pending";
			case ClpcCheck::verified: return "verified";
			case ClpcCheck::rejected: return "rejected";
		}
		return "";
	}

	auto HeldValue::feed(std::optional<double> v) -> std::optional<double> {
		if (v) {
			last_ = v;
			missing_ = 0;
			return v;
		}
		if (last_ and ++missing_ <= max_missing) return last_;
		last_.reset();
		return std::nullopt;
	}

	//? ---- GPU per processo ----
	auto GpuTimeTracker::feed(const std::vector<GpuClient>& clients, uint64_t now_ns) -> std::map<int, double> {
		std::map<int, double> out;
		if (clients.empty()) return out; //? lettura fallita: si tiene la base precedente
		std::map<uint64_t, uint64_t> cur;
		const bool first = prev_ns_ == 0;
		const double interval = first or now_ns <= prev_ns_ ? 0.0 : static_cast<double>(now_ns - prev_ns_);
		std::map<int, uint64_t> delta;
		for (const auto& c : clients) {
			cur[c.id] = c.gpu_ns;
			if (c.pid <= 0) continue;
			auto& d = delta[c.pid];
			//? client nuovo: il suo tempo accumulato puo' venire da prima dell'intervallo, fa solo da base
			if (auto p = prev_.find(c.id); p != prev_.end()) d += c.gpu_ns > p->second ? c.gpu_ns - p->second : 0;
		}
		if (interval > 0)
			for (const auto& [pid, ns] : delta)
				out[pid] = std::clamp(100.0 * static_cast<double>(ns) / interval, 0.0, 100.0);
		prev_ = std::move(cur);
		prev_ns_ = now_ns;
		return out;
	}

	auto gpu_ns_by_pid(const std::vector<GpuClient>& clients) -> std::map<int, uint64_t> {
		std::map<int, uint64_t> out;
		for (const auto& c : clients)
			if (c.pid > 0) out[c.pid] += c.gpu_ns;
		return out;
	}

	auto parse_creator_pid(std::string_view creator) -> std::optional<int> {
		if (not creator.starts_with("pid ")) return std::nullopt;
		creator.remove_prefix(4);
		int pid = 0;
		size_t i = 0;
		for (; i < creator.size() and std::isdigit(static_cast<unsigned char>(creator[i])); i++) {
			pid = pid * 10 + (creator[i] - '0');
			if (pid > 100'000'000) return std::nullopt;
		}
		if (i == 0) return std::nullopt;
		return pid;
	}

	//? ---- energia ----
	auto energy_to_watts(int64_t delta, std::string_view unit, double seconds) -> std::optional<double> {
		if (delta < 0 or not (seconds > 0)) return std::nullopt;
		while (not unit.empty() and unit.front() == ' ') unit.remove_prefix(1);
		while (not unit.empty() and unit.back() == ' ') unit.remove_suffix(1);
		double scale = 0;
		if (unit == "mJ") scale = 1e-3;
		else if (unit == "uJ" or unit == "\xc2\xb5J") scale = 1e-6;
		else if (unit == "nJ") scale = 1e-9;
		else return std::nullopt;
		return static_cast<double>(delta) * scale / seconds;
	}

	auto plausible_power(double w) -> std::optional<double> {
		if (std::isfinite(w) and w > 0 and w <= 1000.0) return w;
		return std::nullopt;
	}

	//? ---- temperature ----
	bool plausible_temp(double c) {
		return std::isfinite(c) and c > 0 and c <= 150.0;
	}

	bool plausible_die_temp(double c) {
		return plausible_temp(c) and c >= 20.0;
	}

	auto average_valid(std::span<const double> values) -> std::optional<double> {
		double sum = 0;
		int n = 0;
		for (double v : values) {
			if (plausible_temp(v)) { sum += v; n++; }
		}
		if (n == 0) return std::nullopt;
		return sum / n;
	}

	auto TempFilter::feed(std::optional<double> value, double dt_s) -> std::optional<double> {
		if (not value or not plausible_temp(*value)) return std::nullopt;
		//? un die puo' scaldarsi di parecchi gradi al secondo sotto carico improvviso, non di decine
		const double max_jump = 25.0 + 10.0 * std::clamp(dt_s, 0.0, 10.0);
		if (last_ and std::abs(*value - *last_) > max_jump and rejected_ < 3) {
			rejected_++;
			return std::nullopt;
		}
		rejected_ = 0;
		last_ = value;
		return value;
	}

	char classify_hid_sensor(std::string_view name) {
		if (name.starts_with("pACC")) return 'P';
		if (name.starts_with("eACC")) return 'E';
		if (name.starts_with("sACC")) return 'S';
		if (name.starts_with("GPU MTR Temp Sensor")) return 'G';
		//? PMU TP<n>g: sensori GPU sui chip recenti (M2 Max: TP1g..TP3g)
		if (name.starts_with("PMU TP") and name.size() > 7 and name.back() == 'g') {
			auto digits = name.substr(6, name.size() - 7);
			if (not digits.empty() and std::all_of(digits.begin(), digits.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
				return 'G';
		}
		return 0;
	}

	char classify_smc_temp_key(std::string_view key, bool has_super_tier) {
		if (key.size() != 4 or key[0] != 'T') return 0;
		switch (key[1]) {
			case 'e': return 'E';
			case 'p': return 'P';
			case 'f': return 'F'; //? Tf*: core P su M3; su M2 Max sono altro (valori negativi o fissi): solo ripiego
			case 's': return has_super_tier ? 'S' : 0; //? su M1-M4 Ts* sono altri sensori (SSD, palmrest)
			case 'g': return 'G';
			default: return 0;
		}
	}

	//? ---- topologia ----
	std::string tier_label(std::string_view perflevel_name) {
		if (perflevel_name == "Efficiency") return "E";
		if (perflevel_name == "Performance") return "P";
		if (perflevel_name == "Super") return "S";
		//? nome sconosciuto (es. "Standard" di una VM): nessun tipo di core, come macmon
		return "_";
	}

	namespace {
		int type_rank(char t) {
			switch (t) {
				case 'E': return 0;
				case 'M': return 1;
				case 'P': return 2;
				default: return -1;
			}
		}
	}

	auto build_tiers(std::span<const PerfLevel> perflevels, std::span<const char> cluster_types) -> std::vector<Tier> {
		std::vector<Tier> tiers;
		int total = 0;
		for (auto it = perflevels.rbegin(); it != perflevels.rend(); ++it) {
			tiers.push_back(Tier{.name = it->name, .label = tier_label(it->name), .cluster_type = 0, .cores = {}});
			total += it->logical_cpus;
		}
		if (tiers.empty()) return tiers;

		//? cluster-type dell'IORegistry: E < M < P, mappati ai livelli dal meno al piu' prestante
		bool types_ok = std::cmp_equal(cluster_types.size(), total)
			and std::all_of(cluster_types.begin(), cluster_types.end(), [](char t) { return type_rank(t) >= 0; });
		if (types_ok) {
			std::set<int> ranks;
			for (char t : cluster_types) ranks.insert(type_rank(t));
			if (ranks.size() == tiers.size()) {
				std::vector<int> ordered(ranks.begin(), ranks.end());
				for (size_t i = 0; i < tiers.size(); i++) {
					tiers[i].cluster_type = "EMP"[ordered[i]];
					for (size_t c = 0; c < cluster_types.size(); c++)
						if (type_rank(cluster_types[c]) == ordered[i]) tiers[i].cores.push_back(static_cast<int>(c));
				}
				bool counts_ok = true;
				for (size_t i = 0; i < tiers.size(); i++)
					counts_ok = counts_ok and std::cmp_equal(tiers[i].cores.size(), perflevels[perflevels.size() - 1 - i].logical_cpus);
				if (counts_ok) return tiers;
				for (auto& t : tiers) { t.cores.clear(); t.cluster_type = 0; }
			}
		}
		//? ripiego: core contigui, dal livello meno prestante (cosi' su M1-M5)
		int next = 0;
		for (size_t i = 0; i < tiers.size(); i++) {
			int n = perflevels[perflevels.size() - 1 - i].logical_cpus;
			for (int c = 0; c < n; c++) tiers[i].cores.push_back(next++);
		}
		return tiers;
	}

	char channel_cluster_type(std::string_view channel) {
		if (channel.find("ECPU") != std::string_view::npos) return 'E';
		if (channel.find("MCPU") != std::string_view::npos) return 'M';
		if (channel.find("PCPU") != std::string_view::npos) return 'P';
		return 0;
	}

	auto tier_for_channel(std::span<const Tier> tiers, std::string_view channel) -> std::optional<size_t> {
		const char t = channel_cluster_type(channel);
		if (t == 0 or tiers.empty()) return std::nullopt;
		for (size_t i = 0; i < tiers.size(); i++)
			if (tiers[i].cluster_type == t) return i;
		if (std::any_of(tiers.begin(), tiers.end(), [](const Tier& x) { return x.cluster_type != 0; })) return std::nullopt;
		//? senza cluster-type: PCPU e' il livello piu' alto, ECPU il piu' basso, MCPU il medio su 3 livelli
		if (t == 'P') return tiers.size() - 1;
		if (t == 'M' and tiers.size() >= 3) return 1;
		return 0;
	}

	//? ---- DVFS ----
	auto parse_dvfs(std::span<const uint8_t> data) -> std::vector<uint32_t> {
		std::vector<uint32_t> freqs;
		for (size_t i = 0; i + 8 <= data.size(); i += 8) {
			uint32_t f = static_cast<uint32_t>(data[i]) | (static_cast<uint32_t>(data[i + 1]) << 8)
				| (static_cast<uint32_t>(data[i + 2]) << 16) | (static_cast<uint32_t>(data[i + 3]) << 24);
			freqs.push_back(f);
		}
		return freqs;
	}

	auto dvfs_to_mhz(std::span<const uint32_t> raw) -> std::vector<uint32_t> {
		uint32_t max = 0;
		for (auto f : raw) max = std::max(max, f);
		uint32_t scale = 0;
		if (max > 100'000'000u) scale = 1'000'000; //? Hz (M1-M3, GPU)
		else if (max > 100'000u) scale = 1'000;    //? kHz (M4+ CPU)
		else return {};                            //? non e' una tabella di frequenze
		std::vector<uint32_t> out;
		for (auto f : raw)
			if (f > 0) out.push_back(static_cast<uint32_t>(std::lround(static_cast<double>(f) / scale)));
		return out;
	}

	auto parse_acc_clusters(std::span<const uint8_t> data) -> std::vector<std::pair<int, int>> {
		std::vector<std::pair<int, int>> out;
		for (size_t i = 0; i + 8 <= data.size(); i += 8)
			out.emplace_back(data[i + 1], data[i]);
		std::stable_sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.first < b.first; });
		return out;
	}

	auto dvfs_keys_for_tiers(size_t tiers, std::span<const std::pair<int, int>> acc_clusters) -> std::vector<std::string> {
		std::vector<std::string> keys(tiers);
		if (tiers == 0) return keys;
		if (not acc_clusters.empty()) {
			//? M5+: il tipo piu' alto va al livello piu' alto, a scendere
			for (size_t i = 0; i < tiers and i < acc_clusters.size(); i++) {
				const auto& entry = acc_clusters[acc_clusters.size() - 1 - i];
				keys[tiers - 1 - i] = fmt::format("voltage-states{}-sram", entry.second);
			}
			return keys;
		}
		//? M1-M4: E in voltage-states1, P in voltage-states5
		if (tiers >= 2) {
			keys.front() = "voltage-states1-sram";
			keys.back() = "voltage-states5-sram";
		}
		return keys;
	}

	//? ---- residenze ----
	bool is_idle_state(std::string_view name) {
		return name == "IDLE" or name == "OFF" or name == "DOWN";
	}

	namespace {
		size_t active_offset(const States& states) {
			size_t offset = 0;
			while (offset < states.size() and is_idle_state(states[offset].first)) offset++;
			return offset;
		}
	}

	bool freqs_fit(const States& states, std::span<const uint32_t> freqs_mhz) {
		if (freqs_mhz.empty()) return false;
		const size_t offset = active_offset(states);
		return states.size() - offset >= freqs_mhz.size() and offset > 0;
	}

	auto residency(const States& states, std::span<const uint32_t> freqs_mhz) -> Residency {
		Residency r;
		const size_t offset = active_offset(states);
		for (size_t s = 0; s < states.size(); s++) {
			const int64_t res = std::max<int64_t>(0, states[s].second);
			r.total += res;
			if (s < offset) continue;
			r.active += res;
			const size_t idx = s - offset;
			if (idx < freqs_mhz.size()) {
				r.weighted_mhz += static_cast<double>(res) * freqs_mhz[idx];
				r.weighted_ticks += res;
			}
		}
		return r;
	}

	void accumulate(Residency& into, const Residency& r) {
		into.total += r.total;
		into.active += r.active;
		into.weighted_mhz += r.weighted_mhz;
		into.weighted_ticks += r.weighted_ticks;
	}

	auto avg_freq(const Residency& r) -> std::optional<double> {
		if (r.weighted_ticks <= 0) return std::nullopt;
		return r.weighted_mhz / static_cast<double>(r.weighted_ticks);
	}

	auto active_pct(const Residency& r) -> std::optional<double> {
		if (r.total <= 0) return std::nullopt;
		return 100.0 * static_cast<double>(r.active) / static_cast<double>(r.total);
	}

	//? ---- istogrammi PMP ----
	auto histogram_avg_gbs(const States& states) -> std::optional<double> {
		if (states.size() <= 1) return std::nullopt;
		int64_t total = 0;
		double weighted = 0;
		for (size_t s = 0; s < states.size(); s++) {
			const auto& name = states[s].first;
			if (not name.ends_with("GB/s")) return std::nullopt;
			const char* p = name.c_str();
			char* end = nullptr;
			const double gbs = std::strtod(p, &end);
			if (end == p or not (gbs > 0)) return std::nullopt;
			const int64_t res = std::max<int64_t>(0, states[s].second);
			total += res;
			if (s > 0) weighted += gbs * static_cast<double>(res); //? bucket 0 = sotto soglia -> 0 GB/s
		}
		if (total <= 0) return std::nullopt;
		return weighted / static_cast<double>(total);
	}

	auto floor_active_pct(const States& states) -> std::optional<double> {
		if (states.size() <= 1) return std::nullopt;
		static constexpr std::array<std::string_view, 9> idle {"OFF", "IDLE", "DOWN", "SLEEP", "VMIN", "F1", "F2", "0%", "NON_IDLE"};
		int64_t total = 0, active = 0;
		for (const auto& [name, res_raw] : states) {
			const int64_t res = std::max<int64_t>(0, res_raw);
			total += res;
			if (std::find(idle.begin(), idle.end(), name) == idle.end()) active += res;
		}
		if (total <= 0) return std::nullopt;
		return 100.0 * static_cast<double>(active) / static_cast<double>(total);
	}

	//? ---- SMC ----
	auto SmcTempRing::due(std::string_view needed, size_t budget) -> std::vector<size_t> {
		std::vector<size_t> out;
		if (keys.empty() or budget == 0) return out;
		const size_t n = keys.size();
		size_t i = next % n;
		for (size_t visited = 0; visited < n and out.size() < budget; visited++, i = (i + 1) % n)
			if (needed.find(keys[i].cls) != std::string_view::npos) out.push_back(i);
		next = i;
		return out;
	}

	auto SmcTempRing::average(char cls) const -> std::optional<double> {
		std::vector<double> values;
		for (const auto& k : keys)
			if (k.cls == cls and k.value) values.push_back(*k.value);
		return average_valid(values);
	}

	auto decode_smc(std::string_view type, std::span<const uint8_t> b) -> std::optional<double> {
		double v = 0;
		if (type == "flt " and b.size() >= 4) {
			uint32_t raw = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
			v = std::bit_cast<float>(raw);
		} else if (type == "sp78" and b.size() >= 2) {
			v = static_cast<int16_t>((static_cast<uint16_t>(b[0]) << 8) | b[1]) / 256.0;
		} else if (type == "fpe2" and b.size() >= 2) {
			v = ((static_cast<unsigned>(b[0]) << 6) | (static_cast<unsigned>(b[1]) >> 2));
		} else if (type == "ui8 " and b.size() >= 1) {
			v = b[0];
		} else if (type == "ui16" and b.size() >= 2) {
			v = (static_cast<unsigned>(b[0]) << 8) | b[1];
		} else if (type == "ui32" and b.size() >= 4) {
			v = static_cast<double>((static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) | (static_cast<uint32_t>(b[2]) << 8) | b[3]);
		} else {
			return std::nullopt;
		}
		if (not std::isfinite(v)) return std::nullopt;
		return v;
	}

	//? ---- uscita ----
	std::string json_number(std::optional<double> v, int decimals) {
		if (not v or not std::isfinite(*v)) return "null";
		return fmt::format("{:.{}f}", *v, decimals);
	}

	std::string json_string(std::string_view s) {
		std::string out = "\"";
		for (char c : s) {
			switch (c) {
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\t': out += "\\t"; break;
				default:
					if (static_cast<unsigned char>(c) < 0x20) out += fmt::format("\\u{:04x}", c);
					else out += c;
			}
		}
		return out + "\"";
	}

	std::string thermal_state_name(int state) {
		switch (state) {
			case 0: return "nominal";
			case 1: return "fair";
			case 2: return "serious";
			case 3: return "critical";
			default: return "";
		}
	}

	std::string format_freq(double mhz) {
		if (mhz >= 999.5) return fmt::format("{:.1f} GHz", mhz / 1000.0);
		return fmt::format("{:.0f} MHz", mhz);
	}

} // namespace AppleSoc::Pure
