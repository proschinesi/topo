// SPDX-License-Identifier: Apache-2.0
// Modified for topo (fork of btop for Apple Silicon), 2026: see NOTICE.
// Test degli ordinamenti "gpu" e "power" e dei watt per processo (solo macOS: su Linux/BSD l'ordinamento non esiste).

#include <gtest/gtest.h>

#include <vector>

#include "btop_shared.hpp"

namespace {
	auto make(std::initializer_list<std::pair<size_t, double>> v) -> std::vector<Proc::proc_info> {
		std::vector<Proc::proc_info> out;
		for (auto [pid, gpu] : v) {
			Proc::proc_info p;
			p.pid = pid;
			p.gpu_p = gpu;
			out.push_back(p);
		}
		return out;
	}
	auto pids(const std::vector<Proc::proc_info>& v) -> std::vector<size_t> {
		std::vector<size_t> out;
		for (const auto& p : v) out.push_back(p.pid);
		return out;
	}
}

TEST(proc_sort, gpu_descending_without_clients_last) {
	auto v = make({{1, -1}, {2, 0.0}, {3, 12.5}, {4, -1}, {5, 3.0}});
	Proc::proc_sorter(v, "gpu", false);
	EXPECT_EQ(pids(v), (std::vector<size_t>{3, 5, 2, 1, 4}));
}

TEST(proc_sort, gpu_reverse_keeps_without_clients_last) {
	auto v = make({{1, -1}, {2, 0.0}, {3, 12.5}, {4, -1}, {5, 3.0}});
	Proc::proc_sorter(v, "gpu", true);
	EXPECT_EQ(pids(v), (std::vector<size_t>{2, 5, 3, 1, 4}));
}

namespace {
	auto make_pwr(std::initializer_list<std::pair<size_t, double>> v) -> std::vector<Proc::proc_info> {
		std::vector<Proc::proc_info> out;
		for (auto [pid, w] : v) {
			Proc::proc_info p;
			p.pid = pid;
			p.pwr_w = w;
			out.push_back(p);
		}
		return out;
	}
}

TEST(proc_sort, power_descending_without_value_last) {
	auto v = make_pwr({{1, -1}, {2, 0.0}, {3, 1.37}, {4, -1}, {5, 0.42}});
	Proc::proc_sorter(v, "power", false);
	EXPECT_EQ(pids(v), (std::vector<size_t>{3, 5, 2, 1, 4}));
}

TEST(proc_sort, power_reverse_keeps_without_value_last) {
	auto v = make_pwr({{1, -1}, {2, 0.0}, {3, 1.37}, {4, -1}, {5, 0.42}});
	Proc::proc_sorter(v, "power", true);
	EXPECT_EQ(pids(v), (std::vector<size_t>{2, 5, 3, 1, 4}));
}

TEST(proc_power, format_watts) {
	EXPECT_EQ(Proc::format_watts(-1), "");
	EXPECT_EQ(Proc::format_watts(0.0), "0.00");
	EXPECT_EQ(Proc::format_watts(0.42), "0.42");
	EXPECT_EQ(Proc::format_watts(3.14), "3.1");
	EXPECT_EQ(Proc::format_watts(12.4), "12");
	EXPECT_EQ(Proc::format_watts(0.42, true), "~0.42");
	EXPECT_EQ(Proc::format_watts(123.0, true), "~123");
}

TEST(proc_power, format_watts_split) {
	// decimali per ciascuna parte
	EXPECT_EQ(Proc::format_watts_split(2.1, 0.8), "2.1+0.80");
	EXPECT_EQ(Proc::format_watts_split(0.42, 0.0), "0.42+0.00");
	EXPECT_EQ(Proc::format_watts_split(12.3, 0.42), "12+0.42");
	// senza client GPU niente "+GPU"
	EXPECT_EQ(Proc::format_watts_split(1.37, -1), "1.4");
	// senza parte CPU (primo campione o illeggibile): solo la GPU, marcata
	EXPECT_EQ(Proc::format_watts_split(-1, 0.8), "~0.80 GPU");
	EXPECT_EQ(Proc::format_watts_split(-1, -1), "");
}

TEST(proc_power, combine_and_aggregate) {
	Proc::proc_info p;
	// CPU nota: somma con la GPU (se nota)
	p.pwr_cpu_w = 1.0; p.pwr_gpu_w = 0.5;
	Proc::combine_power(p);
	EXPECT_DOUBLE_EQ(p.pwr_w, 1.5);
	EXPECT_FALSE(p.pwr_partial);
	p.pwr_gpu_w = -1;
	Proc::combine_power(p);
	EXPECT_DOUBLE_EQ(p.pwr_w, 1.0);
	// CPU sconosciuta: solo GPU > 0, parziale; altrimenti nessun valore
	p.pwr_cpu_w = -1; p.pwr_gpu_w = 0.8;
	Proc::combine_power(p);
	EXPECT_DOUBLE_EQ(p.pwr_w, 0.8);
	EXPECT_TRUE(p.pwr_partial);
	p.pwr_gpu_w = 0.0;
	Proc::combine_power(p);
	EXPECT_DOUBLE_EQ(p.pwr_w, -1);
	EXPECT_FALSE(p.pwr_partial);

	// albero: padre senza valori + figlio vuoto (primo campione) + figlio pieno + figlio parziale
	Proc::proc_info parent, a, b, c;
	a.pwr_cpu_w = 1.0; a.pwr_gpu_w = 0.0; Proc::combine_power(a);
	b.pwr_cpu_w = -1; b.pwr_cpu_unknown = true; b.pwr_gpu_w = 0.5; Proc::combine_power(b);
	Proc::add_power(parent, c);
	EXPECT_DOUBLE_EQ(parent.pwr_w, -1);
	EXPECT_FALSE(parent.pwr_partial);
	Proc::add_power(parent, a);
	EXPECT_DOUBLE_EQ(parent.pwr_w, 1.0);
	EXPECT_FALSE(parent.pwr_partial);
	Proc::add_power(parent, b);
	EXPECT_DOUBLE_EQ(parent.pwr_w, 1.5);
	EXPECT_DOUBLE_EQ(parent.pwr_cpu_w, 1.0);
	EXPECT_DOUBLE_EQ(parent.pwr_gpu_w, 0.5);
	EXPECT_TRUE(parent.pwr_partial);
	EXPECT_TRUE(parent.pwr_cpu_unknown);

	// figlio con CPU illeggibile e nessun valore (root senza GPU): il padre diventa parziale
	Proc::proc_info p2, root_child;
	p2.pwr_cpu_w = 2.0; p2.pwr_gpu_w = 0.0; Proc::combine_power(p2);
	root_child.pwr_cpu_unknown = true; root_child.pwr_gpu_w = 0.0; Proc::combine_power(root_child);
	EXPECT_DOUBLE_EQ(root_child.pwr_w, -1);
	Proc::add_power(p2, root_child);
	EXPECT_DOUBLE_EQ(p2.pwr_w, 2.0);
	EXPECT_TRUE(p2.pwr_partial);

	// padre con CPU illeggibile e figlio pieno: somma del figlio, parziale
	Proc::proc_info p3, child;
	p3.pwr_cpu_unknown = true; p3.pwr_gpu_w = 0.0; Proc::combine_power(p3);
	child.pwr_cpu_w = 0.3; child.pwr_gpu_w = 0.0; Proc::combine_power(child);
	Proc::add_power(p3, child);
	EXPECT_DOUBLE_EQ(p3.pwr_w, 0.3);
	EXPECT_TRUE(p3.pwr_partial);
}
