/*
Copyright 2020 Kaylyn Bogle <kaylyn@kaylyn.ink>

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <algorithm>
#include <chrono>
#include <exception>
#include <fcntl.h>
#include <getopt.h>
#include <iostream>
#include <linux/input.h>
#include <optional>
#include <random>
#include <sstream>
#include <stdlib.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <wiringPi.h>

const int g_pin_input = 0;
const int g_pin_output = 2;

struct program_config {
	int iterations = 1000;
	int delay_min = 10000;
	int delay_max = 20000;
	bool pin = false;
	std::optional<unsigned int> usb = {};
	std::optional<unsigned int> key = {};
	bool events = false;
	bool summary = false;
};

program_config config;

void print_config() {
	const auto tf = [](bool a) { return a ? "true" : "false"; };

	const auto opt = [](std::optional<int> a) {
		std::stringstream ss;
		
		if (a) {
			ss << *a;
		} else {
			ss << "null";
		}

		return ss.str();
	};

	std::cout << "{\"iterations\":" << config.iterations << ","
	          << "\"delay_min\":" << config.delay_min << ","
	          << "\"delay_max\":" << config.delay_max << ","
	          << "\"pin\":" << tf(config.pin) << ","
	          << "\"usb\":" << opt(config.usb) << ","
	          << "\"key\":" << opt(config.key) << "}" << std::endl;
}

class Event {
	public:
	
	class OpenException : public std::exception {};

	Event(const int event_id) : _fd(-1), _id(event_id) {
		std::ostringstream ss;
		ss << "/dev/input/event" << event_id;

		_fd = open(ss.str().c_str(), O_RDONLY | O_NONBLOCK);

		if (_fd < 0) {
			throw OpenException();
		}
	}

	~Event() {
		if (_fd >= 0) {
			close(_fd);
		}
	}

	std::string name() const {
		char event_name[256] = "";
		ioctl(_fd, EVIOCGNAME(sizeof(event_name)), event_name);

		return std::string(event_name);
	}

	int fd() const {
		return _fd;
	}

	int id() const {
		return _id;
	}

	private:
	int _fd;
	int _id;
};

void init_pins() {
	wiringPiSetup();

	pinMode(g_pin_input, INPUT);
	pullUpDnControl(g_pin_input, PUD_UP);

	pinMode(g_pin_output, OUTPUT);
	digitalWrite(g_pin_output, LOW);
}

std::vector<std::chrono::microseconds> get_delays() {
	// Don't really care about real randomness, as we're only using this to get
	// a uniform distribution.
	std::mt19937 rand_gen(30378);
	std::uniform_int_distribution<int> int_dist { config.delay_min, config.delay_max };
	std::vector<std::chrono::microseconds> ret(config.iterations);

	std::generate(std::begin(ret), std::end(ret), [&]() { return std::chrono::microseconds(int_dist(rand_gen)); });

	return ret;
}

void print_event_paths() {
	for (int event_id = 0; event_id < 256; ++event_id) {
		try {
			Event event(event_id);
			std::cout << "[" << event.id() << "] " << event.name() << std::endl;
		} catch (const Event::OpenException&) {
			continue;
		}
	}
}

template <typename F>
std::vector<std::chrono::nanoseconds> measure_loop(F detect) {
	if (config.summary) {
		print_config();
	}

	init_pins();

	auto delays = get_delays();

	std::vector<std::chrono::nanoseconds> times(config.iterations);

	for (int i = 0; i < config.iterations; ++i) {
		std::this_thread::sleep_for(delays[i]);

		auto start = std::chrono::high_resolution_clock::now();

		digitalWrite(g_pin_output, HIGH);
		detect(true);

		times[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - start);

		digitalWrite(g_pin_output, LOW);
		detect(false);
	}

	return times;
}

std::vector<std::chrono::nanoseconds> measure_usb(const int event_id) {
	try {
		Event event(event_id);

		auto fd = event.fd();

		return measure_loop([&](const bool pressed) {
			while (true) {
				input_event keyboard_event;

				int ret = read(fd, &keyboard_event, sizeof(input_event));

				if (ret == -1) {
					continue;
				}

				if (
					keyboard_event.type == EV_KEY &&
					keyboard_event.code == config.key &&
					keyboard_event.value == pressed ? 1 : 0
				) {
					break;
				}
			}
		});
	} catch (const Event::OpenException&) {
		std::cerr << "Could not open fd for " << event_id << std::endl;
		exit(1);
	}
}

std::vector<std::chrono::nanoseconds> measure_pin() {
	return measure_loop([&](const bool pressed) {
		while (true) {
			if (digitalRead(g_pin_input) == pressed ? LOW : HIGH) {
				break;
			}
		}
	});
}

template <typename F>
void measure(F measure_fn) {
	const auto times = measure_fn();

	std::stringstream tss;
	for (const auto& t : times) {
		tss << t.count() << std::endl;
	}
	std::cout << tss.str();
}

void help(const bool err) {
	program_config defaults;

	std::stringstream help_msg;
	help_msg << "-i, --iterations <n>   Number of iterations to perform (default: " << defaults.iterations << ")." << std::endl
	         << "-d, --delaymin <n>     Minimum delay between measurements (default: " << defaults.delay_min << ")." << std::endl
	         << "-D, --delaymax <n>     Maximum delay between measurements (default: " << defaults.delay_max << ")." << std::endl
	         << "-p, --pin              Run pin-based measurement." << std::endl
	         << "-u, --usb <event_id>   Run usb-based measurement. Pass an evdev event id." << std::endl
	         << "-k, --key <event_code> Event code of the key used for measurement." << std::endl
	         << "                       See kernel 'input-event-codes.h'." << std::endl
	         << "-e, --events           List names of evdev events." << std::endl
	         << "-s, --summary          Print summary of measurements." << std::endl
	         << "-h, --help             Show help." << std::endl;

	if (err) {
		std::cerr << help_msg.str();
		exit(1);
	} else {
		std::cout << help_msg.str();
		exit(0);
	}
}

void parse_args(int argc, char** argv) {
	const char* const optstring = "i:d:D:pu:k:esh";
	const option longopts[] = {
		{"iterations", required_argument, nullptr, 'i'},
		{"delaymin", required_argument, nullptr, 'd'},
		{"delaymax", required_argument, nullptr, 'D'},
		{"pin", no_argument, nullptr, 'p'},
		{"usb", required_argument, nullptr, 'u'},
		{"key", required_argument, nullptr, 'k'},
		{"events", no_argument, nullptr, 'e'},
		{"help", no_argument, nullptr, 'h'},
		{"summary", no_argument, nullptr, 's'},
		{nullptr, no_argument, nullptr, 0},
	};

	const auto get_num = [](const std::string name, const char* opt) {
		int val;
		try {
			val = std::stoi(opt);
		} catch (const std::invalid_argument&) {
			std::cerr << name << " must be a number." << std::endl;
			help(true);
		}

		return val;
	};

	const auto get_positive = [&](const std::string name, const char* opt, const bool allow_zero = false) {
		const auto val = get_num(name, opt);

		if (val < 0 || (!allow_zero && 0 == val)) {
			std::cerr << name << " must be greater than zero." << std::endl;
			help(true);
		}

		return val;
	};

	while (true) {
		const auto opt = getopt_long(argc, argv, optstring, longopts, nullptr);

		if (-1 == opt) {
			break;
		}

		switch (opt) {
			case 'i':
				config.iterations = get_positive("iterations", optarg);
				break;

			case 'd':
				config.delay_min = get_positive("delaymin", optarg, true);
				break;

			case 'D':
				config.delay_max = get_positive("delaymax", optarg, true);
				break;

			case 'p':
				config.pin = true;
				break;

			case 'u':
				config.usb = get_num("usb", optarg);
				break;

			case 'k':
				config.key = get_num("key", optarg);
				break;

			case 'e':
				config.events = true;
				break;

			case 's':
				config.summary = true;
				break;

			case 'h':
				help(false);
				break;

			case '?':
			default:
				help(true);
				break;
		}
	}

	if (config.delay_max < config.delay_min) {
		std::cerr << "delaymin must be smaller or equal to delaymin" << std::endl;
		help(true);
	}

	unsigned int num_cmds = 0;
	if (config.pin) ++num_cmds;
	if (config.usb) ++num_cmds;
	if (config.events) ++num_cmds;

	if (num_cmds == 0) {
		std::cerr << "Must pass one of: pin, usb, events" << std::endl;
		help(true);
	}

	if (num_cmds > 1) {
		std::cerr << "Passed conflicting mutually exclusive commands: pin, usb, events" << std::endl;
		help(true);
	}

	if (config.usb && !config.key) {
		std::cerr << "Must pass --key when using usb measurement" << std::endl;
		help(true);
	}
}

int main(int argc, char* argv[]) {
	parse_args(argc, argv);

	if (config.events) {
		print_event_paths();
	} else if (config.pin) {
		measure([]() { return measure_pin(); });
	} else if (config.usb) {
		measure([]() { return measure_usb(*config.usb); });
	}

	return 0;
}
