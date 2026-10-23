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
   Parti portate da macmon (MIT, Copyright (c) 2024 vladkens) e
   mactop (MIT, Copyright (c) 2024-2026 Carsen Klock): vedi NOTICE.

indent = tab
tab-size = 4
*/

//? Raccolta unica delle metriche del SoC Apple Silicon (un campionamento per ciclo, tutto a delta).
//? Ogni valore e' std::optional: un dato che la macchina non espone resta assente, mai 0 finto.
//? Le funzioni pure (senza IOKit) stanno in AppleSoc::Pure e sono coperte dai test.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AppleSoc {

	//* Un livello di prestazione (hw.perflevelN): Efficiency, Performance, Super...
	struct ClusterSample {
		std::string name;                       //? nome da sysctl hw.perflevelN.name
		std::string label;                      //? E, P, S (o iniziale del nome)
		std::vector<int> cores;                 //? indici dei core come li numera btop (host_processor_info)
		std::optional<double> freq_mhz;         //? frequenza media pesata sulla residenza attiva
		std::optional<double> freq_max_mhz;     //? ultima voce della tabella DVFS
		std::optional<double> active_pct;       //? residenza negli stati attivi (IOReport)
		std::optional<double> ticks_usage_pct;  //? uso dai tick del kernel sugli stessi core (controllo mappatura)
		std::optional<double> temp_c;           //? temperatura del livello (HID, ripiego SMC)
	};

	struct Sample {
		bool valid = false;                     //? false sul primo campione (manca il delta)
		uint64_t timestamp_ms = 0;              //? tempo Unix in ms
		double interval_s = 0;                  //? durata reale del delta

		std::vector<ClusterSample> clusters;

		std::optional<double> gpu_usage_pct;    //? residenza attiva GPUPH
		std::optional<double> gpu_freq_mhz;
		std::optional<double> gpu_freq_max_mhz;
		std::optional<double> gpu_power_w;
		std::optional<double> gpu_temp_c;

		std::optional<double> cpu_power_w;
		std::optional<double> ane_power_w;
		std::optional<double> dram_power_w;
		std::optional<double> system_power_w;

		//? stima: residenza delle richieste di "floor" di prestazione dell'ANE (PMP), non uso misurato
		std::optional<double> ane_usage_estimate_pct;

		std::optional<double> dram_bw_gbs;           //? contatori AMC diretti (byte), se il kernel li espone
		std::optional<double> dram_bw_estimate_gbs;  //? stima dagli istogrammi PMP DCS BW

		std::optional<std::string> thermal_state;    //? NSProcessInfo.thermalState

		std::map<std::string, std::string> sources;  //? da dove arriva ogni grandezza (diagnostica)
	};

	//* Inizializza le sorgenti (idempotente). Ritorna false se non e' un Mac Apple Silicon.
	bool init();
	bool available();
	void shutdown();
	//* La potenza CPU e' leggibile (canali CLPC su macOS 27, Energy Model prima)
	bool has_cpu_power();

	//* Nuovo campione se l'ultimo e' piu' vecchio di max_age_ms, altrimenti quello in cache.
	//* Cpu::collect e Gpu::collect dello stesso ciclo condividono cosi' un solo campionamento.
	const Sample& collect(uint64_t max_age_ms = 50);

	//* Ultimo campione senza campionare (vuoto prima del primo collect).
	const Sample& last();

	//* Sample in JSON su una riga, null per i valori assenti.
	std::string to_json(const Sample& s);

	//* Esegue --dump-soc: count campioni a intervallo interval_ms su stdout. Ritorna il codice di uscita.
	int dump(uint32_t count, uint32_t interval_ms);

	//* Funzioni pure, testabili senza hardware.
	namespace Pure {
		//? ---- CLPC (macOS 27): tabella driver -> ID dei report di energia (da macmon clpc.rs) ----
		struct ClpcChannel {
			uint64_t id;
			std::string_view name; //? "CPU Energy", "GPU Energy", "ANE"
		};
		//? nullopt se macOS non e' 27.x o il driver non e' in tabella
		auto clpc_energy_channels(std::string_view bundle, std::string_view os_version) -> std::optional<std::array<ClpcChannel, 3>>;
		auto os_major(std::string_view os_version) -> std::optional<int>;

		//? ---- energia -> potenza ----
		//? delta di energia nell'unita' indicata (mJ, uJ/µJ, nJ) su seconds secondi -> watt.
		//? nullopt per unita' sconosciuta, delta negativo (sentinella IOReport/azzeramento) o intervallo nullo.
		auto energy_to_watts(int64_t delta, std::string_view unit, double seconds) -> std::optional<double>;
		//? potenza SMC plausibile (0 < w <= 1000)
		auto plausible_power(double w) -> std::optional<double>;

		//? ---- filtro sensori di temperatura ----
		bool plausible_temp(double c); //? 0 < c <= 150
		auto average_valid(std::span<const double> values) -> std::optional<double>;
		//? scarta i salti impossibili tra campioni consecutivi; dopo 3 scarti di fila accetta il nuovo livello
		class TempFilter {
		public:
			auto feed(std::optional<double> value, double dt_s) -> std::optional<double>;
		private:
			std::optional<double> last_;
			int rejected_ = 0;
		};
		//? sensore HID -> 'E','P','S' per i livelli CPU, 'G' per la GPU, 0 se non classificato
		char classify_hid_sensor(std::string_view name);
		//? chiave SMC di temperatura -> 'E' (Te*), 'P' (Tp*), 'F' (Tf*, ripiego per P), 'S' (Ts*, solo se esiste un livello S), 'G' (Tg*), 0
		char classify_smc_temp_key(std::string_view key, bool has_super_tier);

		//? ---- topologia ----
		struct PerfLevel {
			std::string name;
			int logical_cpus = 0;
		};
		struct Tier {
			std::string name;
			std::string label;
			char cluster_type = 0;      //? 'E', 'M', 'P' dall'IORegistry (0 se ignoto)
			std::vector<int> cores;     //? indici btop
		};
		//? etichetta breve dal nome del perflevel
		std::string tier_label(std::string_view perflevel_name);
		//? perflevels: indice 0 = piu' prestante (come sysctl). cluster_types[i] = cluster-type del cpu<i> (o vuoto).
		//? Ritorna i livelli dal meno al piu' prestante con i core btop assegnati.
		auto build_tiers(std::span<const PerfLevel> perflevels, std::span<const char> cluster_types) -> std::vector<Tier>;
		//? prefisso dei canali "CPU Core Performance States" -> cluster-type ('E' per ECPU, 'M' per MCPU, 'P' per PCPU)
		char channel_cluster_type(std::string_view channel);
		//? indice del livello (in tiers) a cui appartiene un canale core
		auto tier_for_channel(std::span<const Tier> tiers, std::string_view channel) -> std::optional<size_t>;

		//? ---- DVFS ----
		//? coppie (frequenza, tensione) da 4+4 byte little endian -> frequenze grezze
		auto parse_dvfs(std::span<const uint8_t> data) -> std::vector<uint32_t>;
		//? grezze -> MHz: Hz fino a M3 (valori > 1e8), kHz da M4 (valori > 1e5); zeri scartati
		auto dvfs_to_mhz(std::span<const uint32_t> raw) -> std::vector<uint32_t>;
		//? acc-clusters: voci da 8 byte, byte0 = indice voltage-states, byte1 = tipo (0 = livello piu' basso)
		auto parse_acc_clusters(std::span<const uint8_t> data) -> std::vector<std::pair<int, int>>; //? (tipo, indice)
		//? chiave voltage-states*-sram per ciascun livello (dal meno al piu' prestante)
		auto dvfs_keys_for_tiers(size_t tiers, std::span<const std::pair<int, int>> acc_clusters) -> std::vector<std::string>;

		//? ---- residenze ----
		using States = std::vector<std::pair<std::string, int64_t>>;
		struct Residency {
			int64_t total = 0;
			int64_t active = 0;
			double weighted_mhz = 0; //? somma residenza * frequenza sugli stati attivi con frequenza nota
			int64_t weighted_ticks = 0;
		};
		bool is_idle_state(std::string_view name); //? IDLE, OFF, DOWN
		//? true se la tabella copre tutti gli stati attivi del canale
		bool freqs_fit(const States& states, std::span<const uint32_t> freqs_mhz);
		auto residency(const States& states, std::span<const uint32_t> freqs_mhz) -> Residency;
		void accumulate(Residency& into, const Residency& r);
		auto avg_freq(const Residency& r) -> std::optional<double>;
		auto active_pct(const Residency& r) -> std::optional<double>;

		//? ---- istogrammi PMP ----
		//? media pesata dei bucket "  16GB/s"; il bucket 0 (sotto soglia) conta come 0. nullopt se non e' un istogramma
		auto histogram_avg_gbs(const States& states) -> std::optional<double>;
		//? percentuale del tempo fuori dagli stati minimi (VMIN, F1, IDLE...) per i canali floor dell'ANE
		auto floor_active_pct(const States& states) -> std::optional<double>;

		//? ---- SMC ----
		//? decodifica un valore SMC per tipo ("flt ", "sp78", "fpe2", "ui8 ", "ui16", "ui32")
		auto decode_smc(std::string_view type, std::span<const uint8_t> bytes) -> std::optional<double>;

		//? ---- uscita ----
		std::string json_number(std::optional<double> v, int decimals = 3);
		std::string json_string(std::string_view s);
		std::string thermal_state_name(int state);
		//? "3.2 GHz" / "912 MHz" come il campo frequenza di btop
		std::string format_freq(double mhz);
	} // namespace Pure

} // namespace AppleSoc
