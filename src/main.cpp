#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <print>
#include <regex>

#include "argparse.hpp"
#include "BS_thread_pool.hpp"
#include "steam2.hpp"
#include "win32console.hpp"

using namespace steam2;

util::KeyStore g_keystore;

std::vector<std::string_view> split(std::string_view str, char delim) {
	std::vector<std::string_view> result;
	auto left = str.begin();

	for (auto it = left; it != str.end(); ++it) {
		if (*it == delim) {
			result.emplace_back(&*left, it - left);
			left = it + 1;
		}
	}

	if (left != str.end())
		result.emplace_back(&*left, str.end() - left);

	return result;
}

struct color {
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

color pallete[] = {
	{255, 173, 173},
	{255, 214, 165},
	{253, 255, 182},
	{202, 255, 191},
	{155, 246, 255},
	{160, 196, 255},
	{189, 178, 255},
	{255, 198, 255}
};

// rainbow :3
template<typename... Args>
void pretty_print(const std::format_string<Args...> fmt, Args&&... args) {
	static std::atomic<int> i = 0;
	static std::hash<std::thread::id> hasher;
	
	color current_color = pallete[hasher(std::this_thread::get_id()) % 8];
	std::print(std::cout, "\033[38;2;{};{};{}m", current_color.r, current_color.g, current_color.b);
	i++;
	std::println(std::cout, fmt, std::forward<decltype(args)>(args)...);
}

std::filesystem::path sanitize_path(std::filesystem::path& original) {
	std::string sanitized = original.string();
    sanitized.erase(std::remove(sanitized.begin(), sanitized.end(), ':'), sanitized.end());
    return std::filesystem::path(sanitized);
}

void cc_extract(argparse::ArgumentParser& args) {
	Manifest manifest(args.get("manifest"));
	steam2::Index::version v;

	if (args["--v2"] == true) {
		v = steam2::Index::version::v2;
	}
	else {
		v = steam2::Index::version::v3;
	}

	Index index(args.get("index"), v);
	std::string key = g_keystore.has_key(manifest.m_header.cacheid) ? g_keystore.get(manifest.m_header.cacheid) : args.get("--key");
	Storage storage(args.get("storage"), key);
	std::filesystem::path base;
	std::regex re;
	bool filter = false;

	try {
		re = args.get("--filter");
		filter = true;
	} catch (std::regex_error err) {
		std::cerr << err.what() << std::endl;
		return;
	} catch (std::exception& ex){
		filter = false; // we need this to make filter optional
	}

	try {
		base = args.get("--out");
	} catch (...) {		
		base = (std::filesystem::path(".") / std::format("{}_{}", manifest.m_header.cacheid, manifest.m_header.gcfversion));
	}

	BS::thread_pool tp;

	auto start = std::chrono::system_clock::now();

	for (const auto& entry : manifest.m_direntries) {
		if (entry.dirtype == 0)
			continue;

		std::filesystem::path path = manifest.full_path_for_entry(entry);
		if (filter && !std::regex_match(path.string(), re))
			continue;

		std::filesystem::path final = (base.make_preferred() / path.make_preferred());
		std::filesystem::path final_dir = final;
		final_dir.remove_filename();
		std::filesystem::create_directories(sanitize_path(final_dir));

		tp.detach_task([&, final, re]() {
			pretty_print("[thread {}]\textracting file: {}", std::this_thread::get_id(), final.string());
			std::ofstream out(final, std::ios::binary);
			storage.extract_file(out, index, entry.fileid);
			out.close();
		});
	}

	tp.wait();

	auto end = std::chrono::system_clock::now();
	std::chrono::duration<double> elapsed_seconds = end - start;
	std::cout << "Took " << elapsed_seconds << "\n";
}

void cc_ls(argparse::ArgumentParser& args) {
	Manifest manifest(args.get("manifest"));

	std::cout << std::format("File list for cache {} version {}:", manifest.m_header.cacheid, manifest.m_header.gcfversion) << std::endl;

	for (const auto& entry : manifest.m_direntries) {
		std::filesystem::path name = manifest.full_path_for_entry(entry);
		if (name.string() == "") {
			continue;
		}

		std::cout << name.make_preferred().string() << "\n";
	}
}

void cc_validate(argparse::ArgumentParser& args) {
	std::string key;
	std::string cacheid = args.get("--cacheid");

	if (cacheid != "") {
		key = g_keystore.get(std::stoi(cacheid));
	} else {
		key = args.get("--key");
	}

	Storage s(args.get("storage"), key);
	Index i(args.get("index"));
	Checksum c(args.get("checksum"));
	int j = 0;
	bool onlybad = args["--onlybad"] == true;
	BS::thread_pool tp;

	std::println("Validating cache {}", args.get("storage"));
	auto start = std::chrono::system_clock::now();

	for (const auto& chksum : c.m_map) {
		if (chksum.count == 0) {
			j++;
			continue;
		}

		tp.detach_task([&, j, chksum]() {
			uint32_t first = chksum.firstidx;
			uint32_t count = chksum.count;
			//read file

			std::ostringstream fb(std::ios_base::binary);
			s.extract_file(fb, i, j);
			size_t left = fb.str().length();

			for (int k = 0; k < count; k++) {
				uint32_t correct_sum = c.m_entries[first + k].sum;
				size_t to_read = std::min(static_cast<size_t>(0x8000), left);
				uint32_t sum = Checksum::hashblock(fb.str().data() + (k * 0x8000), to_read);

				if (correct_sum != sum) {
					std::println("Bad checksum for file {}: got {} expected {}", j, sum, correct_sum);
				} else if (!onlybad) {
					std::println("File {} part {} OK", j, k);
				}
				left -= to_read;
			}
		});

		j++;
	}

	std::println("Tasks submitted, waiting!");
	tp.wait();

	auto end = std::chrono::system_clock::now();
	std::chrono::duration<double> elapsed_seconds = end - start;
	std::println("took {}", elapsed_seconds);
}

void cc_iton(argparse::ArgumentParser& args) {
	int id = args.get<int>("id");
	Manifest m(args.get("manifest"));
	for (const auto& entry : m.m_direntries) {
		if (entry.fileid == id) std::cout << m.full_path_for_entry(entry) << std::endl;
	}
}

void cc_lsblk(argparse::ArgumentParser& args) {
	steam2::Index::version v;

	if (args["--v2"] == true) {
		v = Index::version::v2;
	} else {
		v = Index::version::v3;
	}

	Index i(args.get("index"), v);
	bool onlyid = args["--onlyid"] == true;

	for (const auto& pairs : i.m_indexes) {
		if (onlyid) {
			std::println("{}", pairs.first);
		} else {
			std::println("{} | {}", pairs.first, Index::filetype_to_string(pairs.second.m_type));
		}
	}
}

#ifdef STEAM2_BUILD_NET
void cc_download(argparse::ArgumentParser& args) {
	std::string clsstr = args.get("cls");
	unsigned depot = args.get<int>("depot");
	unsigned version = args.get<int>("version");
	std::string key = g_keystore.has_key(depot) ? g_keystore.get(depot) : args.get("--key");
	std::filesystem::path base;
	std::regex re;
	bool filter = false;
	bool skipcls = args["--skipcls"] == true;

	try {
		re = args.get("--filter");
		filter = true;
	} catch (std::regex_error& err) {
		std::cerr << err.what() << std::endl;
		return;
	}

	try {
		base = args.get("--outpath");
	} catch (...) {
		base = std::filesystem::path(std::format(".\\{}_{}\\", depot, version));
	}

	steam2::net::addr addr;
	auto splitstr = split(clsstr, ':');
	addr.ip = asio::ip::address_v4::from_string(std::string(splitstr[0]));
	int port = std::stoi(std::string(splitstr[1]));
	addr.port = static_cast<uint16_t>(port);
	steam2::net::addr cmaddr;

	if (!skipcls) {
		auto servers = steam2::net::get_fileservers(addr, depot, version, 2);
		for (auto& s : servers) {
			std::cout << s.ip.to_string() << " " << s.port << "\n";
		}
		//net::FileClient fc(servers[1], depot, version);
		cmaddr = servers[1];
	} else {
		cmaddr = addr;
	}

	net::FileClient fc(cmaddr, depot, version);

	Manifest m = fc.download_manifest();
	Checksum c = fc.download_checksums();

	auto start = std::chrono::system_clock::now();

	for (const auto& entry : m.m_direntries) {
		if (entry.fileid == 0xFFFFFFFF)
			continue;

		std::filesystem::path path = m.full_path_for_entry(entry);
		std::filesystem::path final = (base.make_preferred() / path.make_preferred());

		if (filter && !std::regex_match(path.string(), re))
			continue;

		if (filter) {
			std::filesystem::path p2 = final;
			p2.remove_filename();
			std::filesystem::create_directories(p2);
		}

		if (entry.dirtype == 0) {
			std::filesystem::create_directory(final);
			continue;
		}

		std::ofstream out(final, std::ios::binary);

		Index::filetype t;
		auto thefile = fc.get_file(entry.fileid, c.num_checksums(entry.fileid), t);
		std::cout << "Downloading: " << final << "\n";

		for (auto const& chunk : thefile) {
			std::istringstream ss(chunk, std::ios_base::binary);
			Storage::handle_chunk(out, t, ss, chunk.length(), key);
		}

		out.close();
	}

	auto end = std::chrono::system_clock::now();
	std::chrono::duration<double> elapsed_seconds = end - start;
	std::cout << "Took " << elapsed_seconds << "\n";
}

void cc_lsr(argparse::ArgumentParser& args) {
	std::string clsstr = args.get("cls");
	unsigned depot = args.get<int>("depot");
	unsigned version = args.get<int>("version");
	steam2::net::addr addr;
	auto splitstr = split(clsstr, ':');

	addr.ip = asio::ip::address_v4::from_string(std::string(splitstr[0]));
	int port = std::stoi(std::string(splitstr[1]));
	addr.port = static_cast<uint16_t>(port);

	auto servers = steam2::net::get_fileservers(addr, depot, version, 2);
	for (auto& s : servers) {
		std::cout << s.ip.to_string() << " " << s.port << "\n";
	}

	net::FileClient fc(servers[1], depot, version);
	Manifest m = fc.download_manifest();
	std::println("File list for cache {} version {}:", m.m_header.cacheid, m.m_header.gcfversion);

	for (const auto& entry : m.m_direntries) {
		std::filesystem::path name = m.full_path_for_entry(entry);
		if (name.string() == "") {
			continue;
		}
		std::cout << name.make_preferred().string() << "\n";
	}
}

void cc_dlcdr(argparse::ArgumentParser& args) {
	std::string srv = args.get("ip");
	auto splitstr = split(srv, ':');
	steam2::net::addr addr;
	addr.ip = asio::ip::address_v4::from_string(std::string(splitstr[0]));
	int port = std::stoi(std::string(splitstr[1]));
	addr.port = static_cast<uint16_t>(port);
	std::ofstream f("cdr.bin", std::ios_base::binary);
	net::download_cdr(addr, f);
	f.close();
}
#endif

int main(int argc, const char* argv[]) {
	w32::enable_truecolor();
	argparse::ArgumentParser program(argv[0]);

	using callback_fn = std::function<void(argparse::ArgumentParser&)>;
	struct parse_node {
		parse_node(std::string pname, callback_fn fn) : p(pname), f(fn) {};
		argparse::ArgumentParser p;
		callback_fn f;
	};

	std::list<parse_node> parsers;
	auto parser = [&](std::string n, callback_fn cb) -> argparse::ArgumentParser& {
		return (parsers.emplace_back(n, cb)).p;
	};

	// extract
	auto& extract_command = parser("x", cc_extract);
	extract_command.add_description("Extract storage");

	extract_command.add_argument("storage")
		.help("the .data file")
		.required();

	extract_command.add_argument("manifest")
		.help("the .manifest file")
		.required();

	extract_command.add_argument("index")
		.help("the .index file")
		.required();

	extract_command.add_argument("--key")
		.help("the decryption key")
		.default_value(std::string{ "00000000000000000000000000000000" });

	extract_command.add_argument("--out")
		.help("Output directory");
		//.default_value(std::string{ "./output/" });

	extract_command.add_argument("--filter")
		.help("Regex filter");

	extract_command.add_argument("--v2")
		.help("treat index as v2")
		.flag();

	//list
	auto& list_command = parser("ls", cc_ls);
	list_command.add_description("List files in manifest");
	list_command.add_argument("manifest")
		.help("the .manifest file")
		.required();

	// validate
	auto& validate_command = parser("v", cc_validate);
	validate_command.add_description("Validate storage");
	validate_command.add_argument("storage")
		.help("the .data file")
		.required();

	validate_command.add_argument("index")
		.help("the .index file")
		.required();

	validate_command.add_argument("checksum")
		.help("the .checksums file")
		.required();

	validate_command.add_argument("--key")
		.help("decryption key if depot contains encrypted files")
		.default_value(std::string{ "00000000000000000000000000000000" });

	validate_command.add_argument("--cacheid")
		.help("cacheid for keystore lookup")
		.default_value(std::string{ "" });

	validate_command.add_argument("--onlybad")
		.help("show only bad parts")
		.flag();

	// iton
	auto& iton = parser("iton", cc_iton);
	iton.add_description("fileid to name");
	iton.add_argument("id")
		.required()
		.help("id to lookup")
		.scan<'i', int>();

	iton.add_argument("manifest")
		.help("the .manifest file")
		.required();

	//lsblk
	auto& lsblk = parser("lsblk", cc_lsblk);
	lsblk.add_description("List blocks in index");

	lsblk.add_argument("index")
		.help("the .index file")
		.required();

	lsblk.add_argument("--onlyid")
		.help("show only fileids")
		.flag();

	lsblk.add_argument("--v2")
		.help("v2")
		.flag();
#ifdef STEAM2_BUILD_NET
	auto& download_command = parser("dl", cc_download);
	download_command.add_description("Download depot from a content server");

	download_command.add_argument("cls")
		.help("Content Server List Server (ip:port)")
		.required();

	download_command.add_argument("depot")
		.help("depot id")
		.required()
		.scan<'i', int>();

	download_command.add_argument("version")
		.help("version")
		.required()
		.scan<'i', int>();

	download_command.add_argument("--key")
		.help("the decryption key")
		.default_value(std::string{ "00000000000000000000000000000000" });

	download_command.add_argument("--outpath")
		.help("Output directory");
	//.default_value(std::string{ "./output/" });

	download_command.add_argument("--filter")
		.help("Regex filter");

	download_command.add_argument("--skipcls")
		.help("connect with the ip directly to cm")
		.flag();

	auto& dlcdr = parser("dlcdr", cc_dlcdr);
	dlcdr.add_description("download cdr");
	dlcdr.add_argument("ip")
		.help("config server ip address")
		.required();
		
	auto& list_command_remote = parser("lsr", cc_lsr);
	list_command_remote.add_description("List files in manifest (remote)");
	list_command_remote.add_argument("cls")
		.help("Content server list server")
		.required();

	list_command_remote.add_argument("depot")
		.help("depot id")
		.required()
		.scan<'i', int>();

	list_command_remote.add_argument("version")
		.help("version")
		.required()
		.scan<'i', int>();		
#endif
	for (auto& i : parsers) {
		program.add_subparser(i.p);
	}

	try {
		program.parse_args(argc, argv);
		for (auto& i : parsers) {
			if (program.is_subcommand_used(i.p)) { 
				i.f(i.p);
				break;
			};
		}
	} catch (const std::exception& err) {
		std::cout << err.what() << std::endl;
		std::cout << program;
		w32::disable_truecolor();
		return 1;
	}

	w32::disable_truecolor();
	return 0;
}