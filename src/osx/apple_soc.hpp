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
#include <unordered_map>
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
		//? valori CLPC mostrati prima che il controllo incrociato li abbia verificati (UI: "~")
		bool cpu_power_unverified = false;
		bool ane_power_unverified = false;

		//? stima: residenza delle richieste di "floor" di prestazione dell'ANE (PMP), non uso misurato
		std::optional<double> ane_usage_estimate_pct;

		std::optional<double> dram_bw_gbs;           //? contatori AMC diretti (byte), se il kernel li espone
		std::optional<double> dram_bw_estimate_gbs;  //? stima dagli istogrammi PMP DCS BW

		std::optional<std::string> thermal_state;    //? NSProcessInfo.thermalState
		std::string clpc_check;                      //? controllo incrociato CLPC: "pending", "verified", "rejected" o vuoto
		std::optional<double> clpc_gpu_w;            //? GPU da CLPC (diagnostica: confronto con gpu_power_w dell'Energy Model)

		std::map<std::string, std::string> sources;  //? da dove arriva ogni grandezza (diagnostica)
	};

	//* Cosa serve campionare oltre al minimo (deciso dalle opzioni dell'interfaccia).
	//* Gli istogrammi PMP (stima banda DRAM, stima d'uso ANE) hanno una sottoscrizione IOReport a parte,
	//* campionata solo se qualcuno li mostra: costano circa 0,5 ms di CPU per campione.
	struct Wants {
		bool ane_estimate = true;     //? stima d'uso ANE (floor PMP)
		bool dram_bw_estimate = true; //? stima banda DRAM (PMP), usata solo se manca il dato diretto AMC
	};
	void set_wants(const Wants& w);

	//* Inizializza le sorgenti (idempotente). Ritorna false se non e' un Mac Apple Silicon.
	bool init();
	bool available();
	void shutdown();
	//* La potenza CPU e' arrivata davvero almeno una volta (canali CLPC su macOS 27, Energy Model prima).
	//* Prima del primo valore reale resta false: l'interfaccia non riserva spazio a un "0.00W" finto.
	bool has_cpu_power();

	//* Nuovo campione se l'ultimo e' piu' vecchio di max_age_ms, altrimenti quello in cache.
	//* Cpu::collect e Gpu::collect dello stesso ciclo condividono cosi' un solo campionamento.
	const Sample& collect(uint64_t max_age_ms = 50);

	//* Ultimo campione senza campionare (vuoto prima del primo collect).
	const Sample& last();

	//* Client Metal della GPU (AGXDeviceUserClient nell'IORegistry, leggibili senza sudo):
	//* (id del client, pid del creatore, tempo GPU accumulato in ns). Costo: ~100 client, 2 proprieta' ciascuno.
	struct GpuClient {
		uint64_t id = 0;
		int pid = -1;
		uint64_t gpu_ns = 0;
	};
	auto gpu_clients() -> std::vector<GpuClient>;

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
		//? build di macOS su cui la tabella CLPC e' stata verificata a mano (27.0 26A428, 27.0.1 26A434)
		bool clpc_build_verified(std::string_view os_build);

		//? Controllo incrociato dei canali CLPC (costruiti a mano da una tabella per build): il contatore
		//? GPU di CLPC deve concordare con "GPU Energy" dell'Energy Model, che su macOS 27 funziona ancora.
		//? Un campione e' confrontabile se la GPU consuma almeno min_w; dopo reject_after campioni
		//? discordanti di fila i canali CLPC vengono scartati (definitivo per la sessione).
		enum class ClpcCheck { pending, verified, rejected };
		class ClpcValidator {
		public:
			static constexpr double min_w = 0.2;          //? sotto questa potenza GPU il rapporto non e' affidabile
			static constexpr double ratio_lo = 0.8, ratio_hi = 1.25; //? misurato su M2 Max sotto carico: 0,98
			static constexpr double max_plausible_w = 500.0;
			static constexpr int verify_after = 3, reject_after = 5;
			auto feed(std::optional<double> clpc_cpu, std::optional<double> clpc_gpu, std::optional<double> clpc_ane,
				std::optional<double> em_gpu) -> ClpcCheck;
			auto state() const -> ClpcCheck { return state_; }
		private:
			ClpcCheck state_ = ClpcCheck::pending;
			int agree_ = 0, disagree_ = 0;
		};
		std::string clpc_check_name(ClpcCheck c);

		//? Ultimo valore valido tenuto per al massimo max_missing campioni senza dato, poi assente:
		//? un campione invalido isolato non fa sparire e riapparire un valore nell'interfaccia.
		class HeldValue {
		public:
			static constexpr int max_missing = 2;
			auto feed(std::optional<double> v) -> std::optional<double>;
			bool fresh() const { return missing_ == 0; } //? l'ultimo feed aveva un dato
		private:
			std::optional<double> last_;
			int missing_ = 0;
		};

		//? ---- GPU per processo ----
		//? Uso GPU per pid a delta tra due letture dei client: somma su tutti i client del pid di
		//? (tempo attuale - tempo precedente) / intervallo. Un contatore che cala vale 0. Prima lettura: nessun valore.
		class GpuTimeTracker {
		public:
			//? un client mai visto fa solo da base (uso 0 nel suo primo intervallo); una lettura vuota
			//? (fallita) non tocca lo stato e non da' valori
			auto feed(const std::vector<GpuClient>& clients, uint64_t now_ns) -> std::map<int, double>;
		private:
			std::map<uint64_t, uint64_t> prev_; //? id client -> ns
			uint64_t prev_ns_ = 0;
		};
		//? topo: tempo GPU accumulato per pid, somma dei client dello stesso pid (pid <= 0 ignorati)
		auto gpu_ns_by_pid(const std::vector<GpuClient>& clients) -> std::map<int, uint64_t>;
		//? "pid 402, WindowServer" -> 402
		auto parse_creator_pid(std::string_view creator) -> std::optional<int>;

		//? ---- potenza per processo (colonna W) ----
		//? topo: lettura di proc_pid_rusage(RUSAGE_INFO_V6) di un processo leggibile: inizio del processo
		//? (ri_proc_start_abstime, distingue i pid riciclati) ed energia CPU cumulativa (ri_energy_nj)
		struct ProcEnergyReading {
			int pid = -1;
			uint64_t start = 0;
			uint64_t energy_nj = 0;
		};
		//? watt da un delta di energia in nJ su dt_ns: nullopt se l'intervallo e' nullo o il contatore cala
		auto energy_delta_watts(uint64_t prev_nj, uint64_t cur_nj, uint64_t dt_ns) -> std::optional<double>;
		//? Watt CPU per processo a delta tra due letture, chiave (pid, inizio del processo). Il primo campione
		//? di un processo (o di un pid riciclato) fa solo da base; i processi non piu' letti escono dallo stato.
		//? now_ns e' un orologio monotono. Stesso istante: nessun valore e base invariata. Orologio che torna
		//? indietro o contatore che cala: nessun valore, nuova base dalla lettura.
		class ProcEnergyTracker {
		public:
			//? risultato parallelo a readings (nullopt = nessun valore in questo ciclo)
			auto feed(const std::vector<ProcEnergyReading>& readings, uint64_t now_ns) -> std::vector<std::optional<double>>;
			auto size() const -> size_t { return prev_.size(); }
		private:
			struct Prev { uint64_t start = 0, energy_nj = 0, t_ns = 0, gen = 0; };
			std::unordered_map<int, Prev> prev_; //? riusata tra i cicli: i pid non letti escono per generazione
			uint64_t gen_ = 0;
		};
		//? Potenza GPU per processo: gpu_w * uso / somma degli usi (a riposo la GPU Apple e' quasi spenta, quindi
		//? la potenza media e' gia' proporzionale al tempo attivo: si divide tra i client che hanno lavorato).
		//? Somma nulla: 0 W a tutti (tutto alla quota system). nullopt se la potenza GPU manca o usage e' vuota
		//? (lettura dei client fallita o primo ciclo).
		auto gpu_power_by_pid(std::optional<double> gpu_w, const std::map<int, double>& usage) -> std::optional<std::map<int, double>>;
		//? Quota non attribuita ai processi: (CPU totale - somma CPU dei processi) + (GPU totale - somma GPU
		//? attribuita), ogni parte >= 0; la parte GPU conta solo se nota. nullopt senza CPU totale.
		auto unattributed_power(std::optional<double> cpu_total_w, double cpu_procs_w,
			std::optional<double> gpu_total_w, double gpu_procs_w) -> std::optional<double>;

		//? ---- energia -> potenza ----
		//? delta di energia nell'unita' indicata (mJ, uJ/µJ, nJ) su seconds secondi -> watt.
		//? nullopt per unita' sconosciuta, delta negativo (sentinella IOReport/azzeramento) o intervallo nullo.
		auto energy_to_watts(int64_t delta, std::string_view unit, double seconds) -> std::optional<double>;
		//? potenza SMC plausibile (0 < w <= 1000)
		auto plausible_power(double w) -> std::optional<double>;

		//? ---- filtro sensori di temperatura ----
		bool plausible_temp(double c); //? 0 < c <= 150
		//? chiavi SMC di die (Tp/Te/Ts/Tg): sotto 20 °C sono blocchi spenti dal power gating che riportano
		//? valori non assoluti (M2 Max: 4-8 °C su Tp* dei core P fermi), non temperature
		bool plausible_die_temp(double c); //? 20 <= c <= 150
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
		//? Lettura a rotazione delle chiavi di temperatura: ogni chiave tiene l'ultimo valore letto,
		//? a ogni campione se ne rileggono al massimo budget (le classi coperte dall'HID si saltano).
		//? La media di una classe usa tutti i valori noti, anche di qualche campione prima.
		struct SmcTempRing {
			struct Key { std::string key; char cls = 0; std::optional<double> value; };
			std::vector<Key> keys;
			size_t next = 0;
			//? indici delle chiavi da rileggere ora (classi in needed), in ordine di rotazione
			auto due(std::string_view needed, size_t budget) -> std::vector<size_t>;
			auto average(char cls) const -> std::optional<double>;
		};
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
