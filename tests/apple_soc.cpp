// SPDX-License-Identifier: Apache-2.0
// Modified for topo (fork of btop for Apple Silicon), 2026: see NOTICE.
// Test delle funzioni pure di src/osx/apple_soc (nessun accesso all'hardware).
// I casi CLPC riprendono quelli di macmon src/clpc.rs (MIT, Copyright (c) 2024 vladkens).

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <set>

#include "osx/apple_soc.hpp"

using namespace AppleSoc::Pure;

// ---- CLPC ----

TEST(apple_soc, clpc_ids_per_driver) {
	struct Case { const char* bundle; uint64_t cpu, gpu, ane; };
	for (auto c : {
		Case{"com.apple.driver.AppleT6002CLPC", 0x0000'0010'b501'816bULL, 0x0000'0019'9e2c'3e8bULL, 0x0000'0018'14d5'a574ULL},
		Case{"com.apple.driver.AppleT8112CLPC", 0x0000'0010'b543'5137ULL, 0x0000'0019'638d'9d52ULL, 0x0000'0018'ea08'9c36ULL},
		Case{"com.apple.driver.AppleT8142CLPC", 0x0000'0010'4370'8744ULL, 0x0000'0019'6a01'0933ULL, 0x0000'0018'bc5e'fdf0ULL},
		// le due macchine di prova: M2 Max e M5 Pro
		Case{"com.apple.driver.AppleT6020CLPC", 0x0000'0010'e301'cd75ULL, 0x0000'0019'8f34'cc7aULL, 0x0000'0018'7415'fb0fULL},
		Case{"com.apple.driver.AppleT6050CLPC", 0x0000'0010'd3ab'60bbULL, 0x0000'0019'27cd'4ce5ULL, 0x0000'0018'8c0f'1c59ULL},
	}) {
		for (auto os : {"27.0", "27.0.1"}) {
			auto ch = clpc_energy_channels(c.bundle, os);
			ASSERT_TRUE(ch.has_value()) << c.bundle;
			EXPECT_EQ((*ch)[0].id, c.cpu);
			EXPECT_EQ((*ch)[1].id, c.gpu);
			EXPECT_EQ((*ch)[2].id, c.ane);
			EXPECT_EQ((*ch)[0].name, "CPU Energy");
			EXPECT_EQ((*ch)[1].name, "GPU Energy");
			EXPECT_EQ((*ch)[2].name, "ANE");
		}
	}
}

TEST(apple_soc, clpc_unknown_driver_or_os_is_absent) {
	EXPECT_FALSE(clpc_energy_channels("com.apple.driver.AppleUnknownCLPC", "27.0.1"));
	EXPECT_FALSE(clpc_energy_channels("AppleT8112CLPC", "27.0.1"));
	for (auto os : {"", "15.8", "26.6.2", "28.0", "127.0", "27a"})
		EXPECT_FALSE(clpc_energy_channels("com.apple.driver.AppleT8112CLPC", os)) << os;
}

TEST(apple_soc, os_major) {
	EXPECT_EQ(os_major("27.0.1"), 27);
	EXPECT_EQ(os_major("27"), 27);
	EXPECT_EQ(os_major("15.8"), 15);
	EXPECT_FALSE(os_major(""));
	EXPECT_FALSE(os_major("x27"));
}

// ---- energia -> watt ----

TEST(apple_soc, energy_units_and_interval) {
	EXPECT_DOUBLE_EQ(*energy_to_watts(1500, "mJ", 1.0), 1.5);
	EXPECT_DOUBLE_EQ(*energy_to_watts(1500, "mJ", 0.5), 3.0);
	EXPECT_DOUBLE_EQ(*energy_to_watts(2'000'000, "uJ", 2.0), 1.0);
	EXPECT_DOUBLE_EQ(*energy_to_watts(2'000'000, "\xc2\xb5J", 1.0), 2.0);
	EXPECT_DOUBLE_EQ(*energy_to_watts(666'832'007, "nJ", 1.0), 0.666832007);
	EXPECT_DOUBLE_EQ(*energy_to_watts(0, "nJ", 1.0), 0.0); // zero vero (es. ANE a riposo)
	EXPECT_DOUBLE_EQ(*energy_to_watts(1000, " mJ ", 1.0), 1.0);
}

TEST(apple_soc, energy_invalid_is_absent) {
	EXPECT_FALSE(energy_to_watts(-1, "mJ", 1.0));                 // azzeramento
	EXPECT_FALSE(energy_to_watts(INT64_MIN, "nJ", 1.0));          // sentinella IOReport
	EXPECT_FALSE(energy_to_watts(100, "J/s", 1.0));               // unita' sconosciuta
	EXPECT_FALSE(energy_to_watts(100, "mJ", 0.0));                // intervallo nullo
}

TEST(apple_soc, plausible_power) {
	EXPECT_EQ(plausible_power(13.6), 13.6);
	EXPECT_FALSE(plausible_power(0.0));
	EXPECT_FALSE(plausible_power(1500.0));
	EXPECT_FALSE(plausible_power(NAN));
}

// ---- filtro sensori ----

TEST(apple_soc, temp_plausible_and_average) {
	EXPECT_FALSE(plausible_temp(0.0));
	EXPECT_FALSE(plausible_temp(-9201.1));
	EXPECT_FALSE(plausible_temp(151.0));
	EXPECT_TRUE(plausible_temp(45.0));
	std::vector<double> v {0.0, 40.0, 50.0, -999.0, 200.0};
	EXPECT_DOUBLE_EQ(*average_valid(v), 45.0);
	std::vector<double> none {0.0, -1.0};
	EXPECT_FALSE(average_valid(none));
}

TEST(apple_soc, temp_filter_rejects_impossible_jumps) {
	TempFilter f;
	EXPECT_EQ(f.feed(50.0, 1.0), 50.0);
	EXPECT_EQ(f.feed(60.0, 1.0), 60.0);      // +10 in 1 s: plausibile
	EXPECT_FALSE(f.feed(130.0, 1.0));        // +70 in 1 s: salto impossibile
	EXPECT_FALSE(f.feed(0.0, 1.0));          // 0 °C: assente
	EXPECT_FALSE(f.feed(std::nullopt, 1.0));
	EXPECT_EQ(f.feed(62.0, 1.0), 62.0);
	// un nuovo livello stabile viene accettato dopo 3 scarti
	EXPECT_FALSE(f.feed(120.0, 1.0));
	EXPECT_FALSE(f.feed(120.0, 1.0));
	EXPECT_FALSE(f.feed(120.0, 1.0));
	EXPECT_EQ(f.feed(120.0, 1.0), 120.0);
}

TEST(apple_soc, classify_sensors) {
	EXPECT_EQ(classify_hid_sensor("pACC MTR Temp Sensor2"), 'P');
	EXPECT_EQ(classify_hid_sensor("eACC MTR Temp Sensor0"), 'E');
	EXPECT_EQ(classify_hid_sensor("GPU MTR Temp Sensor1"), 'G');
	EXPECT_EQ(classify_hid_sensor("PMU TP1g"), 'G');
	EXPECT_EQ(classify_hid_sensor("PMU TP12g"), 'G');
	EXPECT_EQ(classify_hid_sensor("PMU TP1s"), 0);
	EXPECT_EQ(classify_hid_sensor("PMU tdie3"), 0);
	EXPECT_EQ(classify_hid_sensor("PMU TPxg"), 0);
	EXPECT_EQ(classify_smc_temp_key("Te04", false), 'E');
	EXPECT_EQ(classify_smc_temp_key("Tp0C", false), 'P');
	EXPECT_EQ(classify_smc_temp_key("Tg0X", false), 'G');
	EXPECT_EQ(classify_smc_temp_key("Tf04", false), 'F');
	EXPECT_EQ(classify_smc_temp_key("Ts0C", false), 0);   // M1-M4: Ts* non sono core
	EXPECT_EQ(classify_smc_temp_key("Ts0C", true), 'S');
	EXPECT_EQ(classify_smc_temp_key("TPD0", true), 0);
	EXPECT_EQ(classify_smc_temp_key("Tp0", true), 0);
}

// ---- topologia ----

TEST(apple_soc, topology_m2_max) {
	std::vector<PerfLevel> levels {{"Performance", 8}, {"Efficiency", 4}};
	std::vector<char> types {'E', 'E', 'E', 'E', 'P', 'P', 'P', 'P', 'P', 'P', 'P', 'P'};
	auto tiers = build_tiers(levels, types);
	ASSERT_EQ(tiers.size(), 2u);
	EXPECT_EQ(tiers[0].label, "E");
	EXPECT_EQ(tiers[0].cores, (std::vector<int>{0, 1, 2, 3}));
	EXPECT_EQ(tiers[1].label, "P");
	EXPECT_EQ(tiers[1].cores, (std::vector<int>{4, 5, 6, 7, 8, 9, 10, 11}));
	EXPECT_EQ(tier_for_channel(tiers, "ECPU010"), 0u);
	EXPECT_EQ(tier_for_channel(tiers, "PCPU130"), 1u);
	EXPECT_FALSE(tier_for_channel(tiers, "MCPU00"));
}

TEST(apple_soc, topology_m5_pro) {
	// M5 Pro: perflevel0 Super x6, perflevel1 Performance x12; cpu0-11 cluster-type M, cpu12-17 P
	std::vector<PerfLevel> levels {{"Super", 6}, {"Performance", 12}};
	std::vector<char> types(18, 'M');
	for (int i = 12; i < 18; i++) types[i] = 'P';
	auto tiers = build_tiers(levels, types);
	ASSERT_EQ(tiers.size(), 2u);
	EXPECT_EQ(tiers[0].name, "Performance");
	EXPECT_EQ(tiers[0].label, "P");
	EXPECT_EQ(tiers[0].cores.size(), 12u);
	EXPECT_EQ(tiers[0].cores.front(), 0);
	EXPECT_EQ(tiers[1].name, "Super");
	EXPECT_EQ(tiers[1].label, "S");
	EXPECT_EQ(tiers[1].cores, (std::vector<int>{12, 13, 14, 15, 16, 17}));
	EXPECT_EQ(tier_for_channel(tiers, "MCPU15"), 0u);
	EXPECT_EQ(tier_for_channel(tiers, "PCPU3"), 1u);
}

TEST(apple_soc, topology_fallback_without_cluster_types) {
	std::vector<PerfLevel> levels {{"Super", 6}, {"Performance", 12}};
	auto tiers = build_tiers(levels, {});
	ASSERT_EQ(tiers.size(), 2u);
	EXPECT_EQ(tiers[0].cores.front(), 0);
	EXPECT_EQ(tiers[0].cores.back(), 11);
	EXPECT_EQ(tiers[1].cores.front(), 12);
	// senza tipi: PCPU al livello piu' alto, MCPU/ECPU al piu' basso
	EXPECT_EQ(tier_for_channel(tiers, "PCPU0"), 1u);
	EXPECT_EQ(tier_for_channel(tiers, "MCPU10"), 0u);
	// tipi incoerenti con i conteggi -> ripiego contiguo
	std::vector<char> wrong(18, 'P');
	auto t2 = build_tiers(levels, wrong);
	EXPECT_EQ(t2[1].cores.front(), 12);
	EXPECT_EQ(tier_label("Efficiency"), "E");
	EXPECT_EQ(tier_label("Standard"), "_");
	EXPECT_EQ(tier_label(""), "_");
}

// ---- DVFS ----

static std::vector<uint8_t> dvfs_bytes(std::initializer_list<uint32_t> freqs) {
	std::vector<uint8_t> out;
	for (uint32_t f : freqs) {
		for (int i = 0; i < 4; i++) out.push_back((f >> (8 * i)) & 0xff);
		for (int i = 0; i < 4; i++) out.push_back(0x11); // tensione
	}
	return out;
}

TEST(apple_soc, dvfs_scale_hz_and_khz) {
	// M2 Max voltage-states1-sram (Hz)
	auto hz = dvfs_to_mhz(parse_dvfs(dvfs_bytes({912000000, 1284000000, 2424000000u})));
	EXPECT_EQ(hz, (std::vector<uint32_t>{912, 1284, 2424}));
	// M5 Pro voltage-states5-sram (kHz)
	auto khz = dvfs_to_mhz(parse_dvfs(dvfs_bytes({1308000, 4608000})));
	EXPECT_EQ(khz, (std::vector<uint32_t>{1308, 4608}));
	// GPU voltage-states9 con lo 0 iniziale
	auto gpu = dvfs_to_mhz(parse_dvfs(dvfs_bytes({0, 444000000, 1398000000})));
	EXPECT_EQ(gpu, (std::vector<uint32_t>{444, 1398}));
	// tabelle di tensioni (valori piccoli) non sono frequenze
	EXPECT_TRUE(dvfs_to_mhz(parse_dvfs(dvfs_bytes({71859, 51040}))).empty());
	// byte spaiati in coda ignorati
	auto odd = dvfs_bytes({912000000});
	odd.push_back(1);
	EXPECT_EQ(parse_dvfs(odd).size(), 1u);
}

TEST(apple_soc, dvfs_keys) {
	auto legacy = dvfs_keys_for_tiers(2, {});
	EXPECT_EQ(legacy, (std::vector<std::string>{"voltage-states1-sram", "voltage-states5-sram"}));
	// M5 Pro acc-clusters: 16 00.. 17 01.. 05 02.. (byte0 = indice, byte1 = tipo)
	std::vector<uint8_t> acc {0x16, 0, 0, 0, 0, 0, 0, 0, 0x17, 1, 0, 0, 0, 0, 0, 0, 0x05, 2, 0, 0, 0, 0, 0, 0};
	auto parsed = parse_acc_clusters(acc);
	ASSERT_EQ(parsed.size(), 3u);
	EXPECT_EQ(parsed[2], (std::pair<int, int>{2, 5}));
	auto keys = dvfs_keys_for_tiers(2, parsed);
	EXPECT_EQ(keys, (std::vector<std::string>{"voltage-states23-sram", "voltage-states5-sram"}));
}

// ---- residenze ----

TEST(apple_soc, residency_frequency_and_active) {
	std::vector<uint32_t> freqs {1000, 2000, 3000};
	States s {{"DOWN", 0}, {"IDLE", 600}, {"V0P2", 100}, {"V1P1", 0}, {"V2P0", 300}};
	ASSERT_TRUE(freqs_fit(s, freqs));
	auto r = residency(s, freqs);
	EXPECT_EQ(r.total, 1000);
	EXPECT_EQ(r.active, 400);
	EXPECT_DOUBLE_EQ(*avg_freq(r), (100 * 1000.0 + 300 * 3000.0) / 400);
	EXPECT_DOUBLE_EQ(*active_pct(r), 40.0);
	// accumulo su piu' core
	Residency sum;
	accumulate(sum, r);
	accumulate(sum, residency(States{{"IDLE", 1000}, {"V0", 0}, {"V1", 0}, {"V2", 0}}, freqs));
	EXPECT_DOUBLE_EQ(*active_pct(sum), 20.0);
	EXPECT_DOUBLE_EQ(*avg_freq(sum), *avg_freq(r));
	// tutto a riposo: frequenza assente, non 0
	auto idle = residency(States{{"IDLE", 1000}, {"V0", 0}}, freqs);
	EXPECT_FALSE(avg_freq(idle));
	EXPECT_DOUBLE_EQ(*active_pct(idle), 0.0);
	// tabella troppo lunga per il canale -> non si usa
	EXPECT_FALSE(freqs_fit(States{{"IDLE", 1}, {"V0", 1}}, freqs));
	EXPECT_FALSE(active_pct(Residency{}));
}

TEST(apple_soc, gpu_states_beyond_table) {
	// GPUPH: OFF + 15 stati, tabella di 2 frequenze (M2 Max ne ha 8)
	States s {{"OFF", 500}, {"P1", 250}, {"P2", 250}, {"P3", 0}};
	auto r = residency(s, std::vector<uint32_t>{444, 612});
	EXPECT_DOUBLE_EQ(*active_pct(r), 50.0);
	EXPECT_DOUBLE_EQ(*avg_freq(r), 528.0);
}

TEST(apple_soc, pmp_histogram_and_floor) {
	States bw {{"  32GB/s", 4312}, {"  64GB/s", 31}, {"  96GB/s", 1}};
	EXPECT_NEAR(*histogram_avg_gbs(bw), (31 * 64.0 + 96.0) / 4344, 1e-9);
	EXPECT_FALSE(histogram_avg_gbs(States{{"VMIN", 1}, {"VMAX", 2}}));
	EXPECT_FALSE(histogram_avg_gbs(States{{"16GB/s", 0}, {"32GB/s", 0}}));
	States floor {{"F1", 75}, {"F2", 0}, {"F3", 25}};
	EXPECT_DOUBLE_EQ(*floor_active_pct(floor), 25.0);
	EXPECT_FALSE(floor_active_pct(States{{"VMIN", 0}, {"VMAX", 0}}));
}

// ---- SMC ----

TEST(apple_soc, smc_decode) {
	float f = 13.681f;
	uint8_t le[4];
	std::memcpy(le, &f, 4);
	EXPECT_FLOAT_EQ(static_cast<float>(*decode_smc("flt ", std::span<const uint8_t>(le, 4))), 13.681f);
	uint8_t sp78[2] {0x2d, 0x80}; // 45.5
	EXPECT_DOUBLE_EQ(*decode_smc("sp78", sp78), 45.5);
	uint8_t sp78neg[2] {0xff, 0x00}; // -1.0
	EXPECT_DOUBLE_EQ(*decode_smc("sp78", sp78neg), -1.0);
	uint8_t fpe2[2] {0x13, 0x88};
	EXPECT_DOUBLE_EQ(*decode_smc("fpe2", fpe2), 1250.0);
	uint8_t ui32[4] {0, 0, 0x09, 0xdb};
	EXPECT_DOUBLE_EQ(*decode_smc("ui32", ui32), 2523.0);
	EXPECT_FALSE(decode_smc("flag", ui32));
	EXPECT_FALSE(decode_smc("flt ", std::span<const uint8_t>(le, 2)));
	uint32_t nan_bits = 0x7fc00000;
	uint8_t nanb[4];
	std::memcpy(nanb, &nan_bits, 4);
	EXPECT_FALSE(decode_smc("flt ", nanb));
}

// ---- uscita ----

TEST(apple_soc, json_helpers) {
	EXPECT_EQ(json_number(std::nullopt), "null");
	EXPECT_EQ(json_number(1.23456, 2), "1.23");
	EXPECT_EQ(json_number(NAN), "null");
	EXPECT_EQ(json_string("a\"b\\c"), "\"a\\\"b\\\\c\"");
	EXPECT_EQ(thermal_state_name(0), "nominal");
	EXPECT_EQ(thermal_state_name(3), "critical");
	EXPECT_EQ(thermal_state_name(9), "");
	EXPECT_EQ(format_freq(3504), "3.5 GHz");
	EXPECT_EQ(format_freq(912), "912 MHz");
}

// ---- controllo incrociato CLPC ----

TEST(apple_soc, clpc_build_verified) {
	EXPECT_TRUE(clpc_build_verified("26A428"));
	EXPECT_TRUE(clpc_build_verified("26A434"));
	EXPECT_FALSE(clpc_build_verified("26B101"));
	EXPECT_FALSE(clpc_build_verified(""));
}

TEST(apple_soc, clpc_validator_verifies_on_agreement) {
	ClpcValidator v;
	// GPU ferma: niente confronto, resta in attesa
	for (int i = 0; i < 10; i++) EXPECT_EQ(v.feed(1.0, 0.01, 0.0, 0.0), ClpcCheck::pending);
	// sotto carico i due contatori concordano (M2 Max: rapporto 0,98)
	EXPECT_EQ(v.feed(2.0, 16.79, 0.0, 16.96), ClpcCheck::pending);
	EXPECT_EQ(v.feed(2.0, 17.42, 0.0, 17.72), ClpcCheck::pending);
	EXPECT_EQ(v.feed(2.0, 17.68, 0.0, 18.01), ClpcCheck::verified);
}

TEST(apple_soc, clpc_validator_rejects_wrong_counters) {
	ClpcValidator v;
	// indici sbagliati: il contatore "GPU" di CLPC non segue la GPU dell'Energy Model
	for (int i = 0; i < ClpcValidator::reject_after - 1; i++) EXPECT_EQ(v.feed(3.0, 0.4, 0.0, 15.0), ClpcCheck::pending);
	EXPECT_EQ(v.feed(3.0, 0.4, 0.0, 15.0), ClpcCheck::rejected);
	// definitivo: anche un campione concorde non lo riabilita
	EXPECT_EQ(v.feed(3.0, 15.0, 0.0, 15.0), ClpcCheck::rejected);
}

TEST(apple_soc, clpc_validator_rejects_busy_counter_on_idle_gpu_and_absurd_values) {
	ClpcValidator idle;
	for (int i = 0; i < ClpcValidator::reject_after; i++) idle.feed(1.0, 5.0, 0.0, 0.0);
	EXPECT_EQ(idle.state(), ClpcCheck::rejected);

	ClpcValidator absurd;
	for (int i = 0; i < ClpcValidator::reject_after; i++) absurd.feed(1e6, std::nullopt, 0.0, std::nullopt);
	EXPECT_EQ(absurd.state(), ClpcCheck::rejected);

	// un disaccordo isolato si azzera col primo accordo
	ClpcValidator noisy;
	for (int r = 0; r < 4; r++) {
		for (int i = 0; i < ClpcValidator::reject_after - 1; i++) noisy.feed(2.0, 1.0, 0.0, 10.0);
		noisy.feed(2.0, 10.0, 0.0, 10.0);
	}
	EXPECT_NE(noisy.state(), ClpcCheck::rejected);
	EXPECT_EQ(clpc_check_name(ClpcCheck::verified), "verified");
}

// ---- SMC a rotazione ----

TEST(apple_soc, smc_temp_ring_rotation_and_average) {
	SmcTempRing ring;
	for (int i = 0; i < 6; i++) ring.keys.push_back({.key = "Tp0" + std::to_string(i), .cls = 'P', .value = 50.0 + i});
	ring.keys.push_back({.key = "Te00", .cls = 'E', .value = 40.0});
	ring.keys.push_back({.key = "Tg00", .cls = 'G', .value = std::nullopt});

	// solo le classi richieste, al massimo budget per volta, poi si riprende da dove si era rimasti
	auto a = ring.due("PE", 4);
	EXPECT_EQ(a, (std::vector<size_t>{0, 1, 2, 3}));
	auto b = ring.due("PE", 4);
	EXPECT_EQ(b, (std::vector<size_t>{4, 5, 6, 0}));
	auto g = ring.due("G", 4);
	EXPECT_EQ(g, (std::vector<size_t>{7}));
	EXPECT_TRUE(ring.due("", 4).empty());

	EXPECT_DOUBLE_EQ(*ring.average('P'), 52.5);
	EXPECT_DOUBLE_EQ(*ring.average('E'), 40.0);
	EXPECT_FALSE(ring.average('G').has_value());
	EXPECT_FALSE(ring.average('S').has_value());
}

// ---- GPU per processo ----

TEST(apple_soc, gpu_time_tracker) {
	GpuTimeTracker t;
	using C = AppleSoc::GpuClient;
	// prima lettura: nessun valore
	EXPECT_TRUE(t.feed({C{1, 100, 5'000'000'000}, C{2, 200, 0}}, 1'000'000'000).empty());
	// 1 s dopo: il pid 100 ha usato 0,3 s sul client noto; il client 3 e' nuovo e fa solo da base
	// (il suo tempo accumulato puo' venire da prima dell'intervallo); il 200 nulla
	auto u = t.feed({C{1, 100, 5'300'000'000}, C{3, 100, 200'000'000}, C{2, 200, 0}}, 2'000'000'000);
	EXPECT_NEAR(u.at(100), 30.0, 1e-9);
	EXPECT_NEAR(u.at(200), 0.0, 1e-9);
	// lettura vuota (fallita): nessun valore e la base resta
	EXPECT_TRUE(t.feed({}, 2'500'000'000).empty());
	// contatore che cala (client ricreato con lo stesso id) = 0, mai negativo; oltre il 100% si taglia
	u = t.feed({C{1, 100, 1'000}, C{3, 100, 2'200'000'000}, C{2, 200, 0}}, 3'000'000'000);
	EXPECT_NEAR(u.at(100), 100.0, 1e-9);
	EXPECT_NEAR(u.at(200), 0.0, 1e-9);
	// un client che compare con molto tempo accumulato non porta il suo pid al 100%
	u = t.feed({C{1, 100, 1'000}, C{3, 100, 2'200'000'000}, C{2, 200, 0}, C{9, 300, 50'000'000'000}}, 4'000'000'000);
	EXPECT_NEAR(u.at(300), 0.0, 1e-9);
	EXPECT_NEAR(u.at(100), 0.0, 1e-9);
}

TEST(apple_soc, gpu_ns_by_pid) {
	using C = AppleSoc::GpuClient;
	EXPECT_TRUE(gpu_ns_by_pid({}).empty());
	// piu' client dello stesso pid si sommano; pid <= 0 ignorati; un pid con 0 ns resta (vale 0)
	auto m = gpu_ns_by_pid({C{1, 100, 300}, C{2, 200, 0}, C{3, 100, 700}, C{4, -1, 999}, C{5, 0, 5}});
	EXPECT_EQ(m.size(), 2u);
	EXPECT_EQ(m.at(100), 1000u);
	EXPECT_EQ(m.at(200), 0u);
	EXPECT_FALSE(m.contains(-1));
	EXPECT_FALSE(m.contains(0));
	// valori grandi (mesi di tempo GPU in ns) senza sconfinamenti
	m = gpu_ns_by_pid({C{1, 7, 5'000'000'000'000'000}, C{2, 7, 5'000'000'000'000'000}});
	EXPECT_EQ(m.at(7), 10'000'000'000'000'000u);
}

// ---- potenza per processo ----

TEST(apple_soc, energy_delta_watts) {
	// 1 J in 1 s = 1 W; 1,37 W di un "yes" su 2 s
	EXPECT_NEAR(*energy_delta_watts(0, 1'000'000'000, 1'000'000'000), 1.0, 1e-12);
	EXPECT_NEAR(*energy_delta_watts(5'000'000'000, 7'740'000'000, 2'000'000'000), 1.37, 1e-9);
	EXPECT_NEAR(*energy_delta_watts(42, 42, 1'000'000'000), 0.0, 1e-12);
	EXPECT_FALSE(energy_delta_watts(0, 100, 0).has_value());   // intervallo nullo
	EXPECT_FALSE(energy_delta_watts(100, 99, 1000).has_value()); // contatore che cala
}

TEST(apple_soc, proc_energy_tracker) {
	using R = ProcEnergyReading;
	ProcEnergyTracker t;
	const uint64_t s = 1'000'000'000;
	// primo campione: solo base
	auto w = t.feed({R{10, 111, 1'000'000'000}, R{20, 222, 0}}, 1 * s);
	ASSERT_EQ(w.size(), 2u);
	EXPECT_FALSE(w[0].has_value());
	EXPECT_FALSE(w[1].has_value());
	EXPECT_EQ(t.size(), 2u);
	// secondo campione a 2 s: 10 consuma 3 J in 2 s, 20 nulla; 30 e' nuovo (solo base)
	w = t.feed({R{10, 111, 4'000'000'000}, R{20, 222, 0}, R{30, 333, 9'000'000'000}}, 3 * s);
	ASSERT_EQ(w.size(), 3u);
	EXPECT_NEAR(*w[0], 1.5, 1e-12);
	EXPECT_NEAR(*w[1], 0.0, 1e-12);
	EXPECT_FALSE(w[2].has_value());
	// pid 10 riciclato (inizio diverso): nuova base, nessun valore; 20 sparito esce dallo stato
	w = t.feed({R{10, 999, 5'000'000'000}, R{30, 333, 10'000'000'000}}, 4 * s);
	EXPECT_FALSE(w[0].has_value());
	EXPECT_NEAR(*w[1], 1.0, 1e-12);
	EXPECT_EQ(t.size(), 2u);
	// il nuovo 10 dal campione dopo; contatore di 30 che cala: nessun valore e nuova base
	w = t.feed({R{10, 999, 6'000'000'000}, R{30, 333, 1'000}}, 5 * s);
	EXPECT_NEAR(*w[0], 1.0, 1e-12);
	EXPECT_FALSE(w[1].has_value());
	w = t.feed({R{30, 333, 2'000'001'000}}, 6 * s);
	EXPECT_NEAR(*w[0], 2.0, 1e-12);
	// stesso istante: nessun valore, la base resta quella di prima
	w = t.feed({R{30, 333, 2'500'001'000}}, 6 * s);
	EXPECT_FALSE(w[0].has_value());
	w = t.feed({R{30, 333, 3'000'001'000}}, 7 * s);
	EXPECT_NEAR(*w[0], 1.0, 1e-12);
	// orologio che torna indietro: nessun valore, nuova base dalla lettura (anche l'istante)
	w = t.feed({R{30, 333, 4'000'001'000}}, 5 * s);
	EXPECT_FALSE(w[0].has_value());
	w = t.feed({R{30, 333, 4'500'001'000}}, 5 * s + s / 2);
	EXPECT_NEAR(*w[0], 1.0, 1e-12);
	// lettura vuota (tutti illeggibili o spariti): stato vuoto
	EXPECT_TRUE(t.feed({}, 8 * s).empty());
	EXPECT_EQ(t.size(), 0u);
}

TEST(apple_soc, gpu_power_by_pid) {
	EXPECT_FALSE(gpu_power_by_pid(std::nullopt, {{1, 50.0}}).has_value());
	// lettura dei client vuota (fallita o primo ciclo): parte GPU sconosciuta, non 0
	EXPECT_FALSE(gpu_power_by_pid(2.0, {}).has_value());
	// in proporzione al tempo GPU dei client: 30 + 10 -> 3/4 e 1/4 di 2 W (la somma e' il totale)
	auto m = gpu_power_by_pid(2.0, {{1, 30.0}, {2, 10.0}, {3, 0.0}});
	ASSERT_TRUE(m.has_value());
	EXPECT_NEAR(m->at(1), 1.5, 1e-12);
	EXPECT_NEAR(m->at(2), 0.5, 1e-12);
	EXPECT_NEAR(m->at(3), 0.0, 1e-12);
	// client presenti ma nessuno lavora: 0 W a tutti (tutto alla quota system)
	m = gpu_power_by_pid(2.0, {{1, 0.0}, {2, 0.0}});
	EXPECT_NEAR(m->at(1), 0.0, 1e-12);
	EXPECT_NEAR(m->at(2), 0.0, 1e-12);
	// quote sovrapposte oltre il 100%: la somma resta il totale
	m = gpu_power_by_pid(2.0, {{1, 90.0}, {2, 60.0}});
	EXPECT_NEAR(m->at(1), 1.2, 1e-12);
	EXPECT_NEAR(m->at(2), 0.8, 1e-12);
}

TEST(apple_soc, unattributed_power) {
	EXPECT_FALSE(unattributed_power(std::nullopt, 1.0, 2.0, 1.0).has_value());
	EXPECT_NEAR(*unattributed_power(4.0, 1.5, std::nullopt, 0.0), 2.5, 1e-12);
	EXPECT_NEAR(*unattributed_power(4.0, 1.5, 1.0, 0.25), 3.25, 1e-12);
	// clamp di ogni parte a 0: processi oltre il totale (intervalli non allineati)
	EXPECT_NEAR(*unattributed_power(1.0, 1.5, 1.0, 0.25), 0.75, 1e-12);
	EXPECT_NEAR(*unattributed_power(1.0, 0.5, 1.0, 2.0), 0.5, 1e-12);
}

TEST(apple_soc, held_value_bridges_short_gaps_only) {
	HeldValue h;
	EXPECT_FALSE(h.feed(std::nullopt).has_value());
	EXPECT_DOUBLE_EQ(*h.feed(3.0), 3.0);
	EXPECT_TRUE(h.fresh());
	// uno o due campioni senza dato: resta l'ultimo valore
	EXPECT_DOUBLE_EQ(*h.feed(std::nullopt), 3.0);
	EXPECT_FALSE(h.fresh());
	EXPECT_DOUBLE_EQ(*h.feed(std::nullopt), 3.0);
	// al terzo sparisce (es. CLPC scartata) e non torna da solo
	EXPECT_FALSE(h.feed(std::nullopt).has_value());
	EXPECT_FALSE(h.feed(std::nullopt).has_value());
	EXPECT_DOUBLE_EQ(*h.feed(4.0), 4.0);
}

TEST(apple_soc, parse_creator_pid) {
	EXPECT_EQ(parse_creator_pid("pid 402, WindowServer"), 402);
	EXPECT_EQ(parse_creator_pid("pid 7"), 7);
	EXPECT_FALSE(parse_creator_pid("WindowServer").has_value());
	EXPECT_FALSE(parse_creator_pid("pid , x").has_value());
}

TEST(apple_soc, smc_temp_ring_keeps_keys_unreadable_at_start) {
	// un sensore GPU a 0 all'avvio (power gating) resta in rotazione e conta appena torna plausibile
	SmcTempRing ring;
	ring.keys.push_back({.key = "Tg0a", .cls = 'G', .value = std::nullopt});
	ring.keys.push_back({.key = "Tg0b", .cls = 'G', .value = std::nullopt});
	EXPECT_FALSE(ring.average('G').has_value());
	for (size_t i : ring.due("G", 16)) ring.keys[i].value = 60.0 + static_cast<double>(i);
	EXPECT_DOUBLE_EQ(*ring.average('G'), 60.5);
}

TEST(apple_soc, plausible_die_temp) {
	EXPECT_FALSE(plausible_die_temp(6.3)); // Tp* di un core P spento (M2 Max)
	EXPECT_FALSE(plausible_die_temp(0.0));
	EXPECT_TRUE(plausible_die_temp(20.0));
	EXPECT_TRUE(plausible_die_temp(55.5));
	EXPECT_FALSE(plausible_die_temp(151.0));
}
